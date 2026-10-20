// fd.cpp -- descriptors that are not files: pipes, eventfds and epoll, and
// the waits over all descriptors (poll, select, epoll_wait).
//
// The UCRT's descriptors are files. Its pipes (_pipe) cannot be waited on,
// and Android code waits on descriptors all the time: ALooper and every event
// loop (libuv, libevent, Chromium's message pump) wake themselves through a
// pipe or an eventfd and sleep in epoll_wait. So these objects live here, in
// memory, with one readiness counter: every state change bumps it and wakes
// the waiters (WaitOnAddress), which then re-check what they wait for.
// Sockets (socket.cpp) cannot report changes this way and are re-checked
// every few milliseconds instead.
//
// Regular files are always ready, as on Linux.

#include "bionic_internal.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <io.h>
#include <windows.h>

namespace winapk::bionic {

// Linux poll bits.
constexpr short kPollIn = 0x1, kPollPri = 0x2, kPollOut = 0x4, kPollErr = 0x8, kPollHup = 0x10, kPollNval = 0x20;
constexpr int kONonblock = 04000;
constexpr int kEPIPE = 32;

int64_t FdObject::read(void*, size_t) {
    *guest_errno() = kEINVAL;
    return -1;
}
int64_t FdObject::write(const void*, size_t) {
    *guest_errno() = kEINVAL;
    return -1;
}
int FdObject::ioctl(int, void*) {
    *guest_errno() = 25;  // ENOTTY
    return -1;
}

namespace {

// ---- the table -------------------------------------------------------------------
std::mutex g_table_lock;
std::unordered_map<int, std::shared_ptr<FdObject>> g_table;

// ---- readiness ---------------------------------------------------------------------
std::atomic<uint32_t> g_generation{0};

// Waits until fd_notify() is called or `ms` pass (INFINITE allowed), whichever
// comes first, given the generation the caller saw before checking.
void wait_for_change(uint32_t seen, DWORD ms) {
    uint32_t expected = seen;
    WaitOnAddress(&g_generation, &expected, sizeof(expected), ms);
}

uint64_t now_ms() { return GetTickCount64(); }

// ---- pipe ---------------------------------------------------------------------------
struct PipeBuffer {
    std::mutex lock;
    std::deque<char> data;
    bool reader_open = true, writer_open = true;
    static constexpr size_t kCapacity = 65536;  // Linux's default pipe size
};

class PipeReader final : public FdObject {
public:
    explicit PipeReader(std::shared_ptr<PipeBuffer> b) : b_(std::move(b)) {}
    ~PipeReader() override {
        {
            std::lock_guard<std::mutex> hold(b_->lock);
            b_->reader_open = false;
        }
        fd_notify();
    }
    int64_t read(void* buf, size_t n) override {
        if (n == 0) return 0;
        for (;;) {
            const uint32_t seen = g_generation.load();
            {
                std::lock_guard<std::mutex> hold(b_->lock);
                if (!b_->data.empty()) {
                    const size_t k = std::min(n, b_->data.size());
                    std::copy_n(b_->data.begin(), k, static_cast<char*>(buf));
                    b_->data.erase(b_->data.begin(), b_->data.begin() + static_cast<ptrdiff_t>(k));
                    fd_notify();
                    return static_cast<int64_t>(k);
                }
                if (!b_->writer_open) return 0;  // end of file
            }
            if (nonblocking) {
                *guest_errno() = kEAGAIN;
                return -1;
            }
            wait_for_change(seen, INFINITE);
        }
    }
    short ready(short events) override {
        std::lock_guard<std::mutex> hold(b_->lock);
        short r = 0;
        if ((events & kPollIn) && !b_->data.empty()) r |= kPollIn;
        if (!b_->writer_open) r |= kPollHup;
        return r;
    }
    int ioctl(int request, void* arg) override {
        if (request == 0x541B) {  // FIONREAD
            std::lock_guard<std::mutex> hold(b_->lock);
            *static_cast<int*>(arg) = static_cast<int>(b_->data.size());
            return 0;
        }
        return FdObject::ioctl(request, arg);
    }

private:
    std::shared_ptr<PipeBuffer> b_;
};

class PipeWriter final : public FdObject {
public:
    explicit PipeWriter(std::shared_ptr<PipeBuffer> b) : b_(std::move(b)) {}
    ~PipeWriter() override {
        {
            std::lock_guard<std::mutex> hold(b_->lock);
            b_->writer_open = false;
        }
        fd_notify();
    }
    int64_t write(const void* buf, size_t n) override {
        const char* p = static_cast<const char*>(buf);
        size_t done = 0;
        while (done < n) {
            const uint32_t seen = g_generation.load();
            {
                std::lock_guard<std::mutex> hold(b_->lock);
                if (!b_->reader_open) {
                    // SIGPIPE is not delivered (no signals here); EPIPE is.
                    if (done) break;
                    *guest_errno() = kEPIPE;
                    return -1;
                }
                const size_t room = PipeBuffer::kCapacity - std::min(PipeBuffer::kCapacity, b_->data.size());
                if (room > 0) {
                    const size_t k = std::min(room, n - done);
                    b_->data.insert(b_->data.end(), p + done, p + done + k);
                    done += k;
                    fd_notify();
                    continue;
                }
            }
            if (nonblocking) {
                if (done) break;
                *guest_errno() = kEAGAIN;
                return -1;
            }
            wait_for_change(seen, INFINITE);
        }
        return static_cast<int64_t>(done);
    }
    short ready(short events) override {
        std::lock_guard<std::mutex> hold(b_->lock);
        short r = 0;
        if ((events & kPollOut) && b_->data.size() < PipeBuffer::kCapacity) r |= kPollOut;
        if (!b_->reader_open) r |= kPollErr;
        return r;
    }

private:
    std::shared_ptr<PipeBuffer> b_;
};

// ---- eventfd ---------------------------------------------------------------------------
class EventFd final : public FdObject {
public:
    EventFd(uint64_t initial, bool semaphore) : count_(initial), semaphore_(semaphore) {}
    int64_t read(void* buf, size_t n) override {
        if (n < 8) {
            *guest_errno() = kEINVAL;
            return -1;
        }
        for (;;) {
            const uint32_t seen = g_generation.load();
            {
                std::lock_guard<std::mutex> hold(lock_);
                if (count_ > 0) {
                    const uint64_t v = semaphore_ ? 1 : count_;
                    count_ -= v;
                    std::memcpy(buf, &v, 8);
                    fd_notify();
                    return 8;
                }
            }
            if (nonblocking) {
                *guest_errno() = kEAGAIN;
                return -1;
            }
            wait_for_change(seen, INFINITE);
        }
    }
    int64_t write(const void* buf, size_t n) override {
        uint64_t v;
        if (n < 8 || (std::memcpy(&v, buf, 8), v == UINT64_MAX)) {
            *guest_errno() = kEINVAL;
            return -1;
        }
        {
            std::lock_guard<std::mutex> hold(lock_);
            count_ += v;  // the counter cannot reach UINT64_MAX in practice
        }
        fd_notify();
        return 8;
    }
    short ready(short events) override {
        std::lock_guard<std::mutex> hold(lock_);
        return static_cast<short>(((events & kPollIn) && count_ > 0 ? kPollIn : 0) | (events & kPollOut));
    }

private:
    std::mutex lock_;
    uint64_t count_;
    bool semaphore_;
};

// ---- checking any descriptor ---------------------------------------------------------------
// revents for one descriptor; *polled is set when the answer may change
// without a notification.
short check(int fd, short events, bool* polled) {
    if (fd < 0) return 0;
    if (auto o = fd_object(fd)) {
        if (o->needs_polling()) *polled = true;
        return o->ready(static_cast<short>(events | kPollErr | kPollHup));
    }
    if (_get_osfhandle(fd) == -1) return kPollNval;
    return static_cast<short>(events & (kPollIn | kPollOut | kPollPri));  // a file: always ready
}

using GuestPollFd = PollFd;

// The common wait: re-checks `count` descriptors until one is ready or the
// timeout (ms, -1 = forever) passes. Returns how many are ready.
template <typename Check>
int wait_ready(int timeout_ms, Check check_all) {
    const uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : now_ms() + static_cast<uint64_t>(timeout_ms);
    for (;;) {
        const uint32_t seen = g_generation.load();
        bool polled = false;
        const int n = check_all(&polled);
        if (n > 0 || timeout_ms == 0) return n;
        const uint64_t now = now_ms();
        if (now >= deadline) return 0;
        uint64_t wait = deadline == UINT64_MAX ? INFINITE : deadline - now;
        if (polled) wait = std::min<uint64_t>(wait, 5);
        wait_for_change(seen, static_cast<DWORD>(wait));
    }
}

WINAPK_GUEST_ABI int g_poll(GuestPollFd* fds, uint64_t nfds, int timeout) {
    return wait_ready(timeout, [&](bool* polled) {
        int n = 0;
        for (uint64_t i = 0; i < nfds; ++i) {
            fds[i].revents = check(fds[i].fd, fds[i].events, polled);
            if (fds[i].revents) ++n;
        }
        return n;
    });
}
struct GuestTimespec {
    int64_t tv_sec, tv_nsec;
};
WINAPK_GUEST_ABI int g_ppoll(GuestPollFd* fds, uint64_t nfds, const GuestTimespec* ts, const void*) {
    const int ms = ts ? static_cast<int>(ts->tv_sec * 1000 + (ts->tv_nsec + 999999) / 1000000) : -1;
    return g_poll(fds, nfds, ms);
}

// select: Linux's fd_set is 1024 bits in unsigned longs.
struct GuestFdSet {
    uint64_t bits[16];
};
bool is_set(const GuestFdSet* s, int fd) { return s && (s->bits[fd / 64] >> (fd % 64)) & 1; }
void set_bit(GuestFdSet* s, int fd) { s->bits[fd / 64] |= uint64_t{1} << (fd % 64); }
struct GuestTimeval {
    int64_t tv_sec, tv_usec;
};
WINAPK_GUEST_ABI int g_select(int nfds, GuestFdSet* rd, GuestFdSet* wr, GuestFdSet* ex, GuestTimeval* tv) {
    if (nfds < 0 || nfds > 1024) {
        *guest_errno() = kEINVAL;
        return -1;
    }
    const GuestFdSet in_rd = rd ? *rd : GuestFdSet{}, in_wr = wr ? *wr : GuestFdSet{}, in_ex = ex ? *ex : GuestFdSet{};
    const int ms = tv ? static_cast<int>(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    return wait_ready(ms, [&](bool* polled) {
        GuestFdSet out_rd{}, out_wr{}, out_ex{};
        int n = 0;
        for (int fd = 0; fd < nfds; ++fd) {
            short want = 0;
            if (is_set(&in_rd, fd)) want |= kPollIn;
            if (is_set(&in_wr, fd)) want |= kPollOut;
            if (is_set(&in_ex, fd)) want |= kPollPri;
            if (!want) continue;
            const short r = check(fd, want, polled);
            if ((r & (kPollIn | kPollHup | kPollErr)) && (want & kPollIn)) set_bit(&out_rd, fd), ++n;
            if ((r & (kPollOut | kPollErr)) && (want & kPollOut)) set_bit(&out_wr, fd), ++n;
            if ((r & kPollPri) && (want & kPollPri)) set_bit(&out_ex, fd), ++n;
        }
        if (n > 0) {
            if (rd) *rd = out_rd;
            if (wr) *wr = out_wr;
            if (ex) *ex = out_ex;
        }
        return n;
    });
}
WINAPK_GUEST_ABI int g_pselect(int nfds, GuestFdSet* rd, GuestFdSet* wr, GuestFdSet* ex, const GuestTimespec* ts,
                               const void*) {
    GuestTimeval tv{};
    if (ts) tv = {ts->tv_sec, ts->tv_nsec / 1000};
    return g_select(nfds, rd, wr, ex, ts ? &tv : nullptr);
}

// ---- epoll ---------------------------------------------------------------------------------
// struct epoll_event is packed on x86_64: 4 bytes of events, 8 of data.
#pragma pack(push, 4)
struct GuestEpollEvent {
    uint32_t events;
    uint64_t data;
};
#pragma pack(pop)
static_assert(sizeof(GuestEpollEvent) == 12, "x86_64's packed struct epoll_event");
constexpr uint32_t kEpollOneshot = 1u << 30;

class Epoll final : public FdObject {
public:
    struct Interest {
        uint32_t events;
        uint64_t data;
        bool armed = true;
    };
    std::mutex lock;
    std::map<int, Interest> interests;
    short ready(short events) override {
        // An epoll descriptor is readable when one of its descriptors is.
        std::lock_guard<std::mutex> hold(lock);
        bool polled = false;
        for (auto& [fd, in] : interests) {
            if (in.armed && check(fd, static_cast<short>(in.events), &polled)) return events & kPollIn;
        }
        return 0;
    }
};

WINAPK_GUEST_ABI int g_epoll_create1(int) { return fd_install(std::make_shared<Epoll>()); }
WINAPK_GUEST_ABI int g_epoll_create(int size) {
    if (size <= 0) {
        *guest_errno() = kEINVAL;
        return -1;
    }
    return g_epoll_create1(0);
}
WINAPK_GUEST_ABI int g_epoll_ctl(int epfd, int op, int fd, const GuestEpollEvent* ev) {
    auto ep = std::dynamic_pointer_cast<Epoll>(fd_object(epfd));
    if (!ep) {
        *guest_errno() = kEBADF;
        return -1;
    }
    if (fd == epfd || (!fd_object(fd) && _get_osfhandle(fd) == -1)) {
        *guest_errno() = fd == epfd ? kEINVAL : kEBADF;
        return -1;
    }
    {
        std::lock_guard<std::mutex> hold(ep->lock);
        auto it = ep->interests.find(fd);
        switch (op) {
            case 1:  // EPOLL_CTL_ADD
                if (it != ep->interests.end()) {
                    *guest_errno() = kEEXIST;
                    return -1;
                }
                ep->interests[fd] = {ev ? ev->events : 0, ev ? ev->data : 0, true};
                break;
            case 2:  // EPOLL_CTL_DEL
                if (it == ep->interests.end()) {
                    *guest_errno() = kENOENT;
                    return -1;
                }
                ep->interests.erase(it);
                break;
            case 3:  // EPOLL_CTL_MOD
                if (it == ep->interests.end()) {
                    *guest_errno() = kENOENT;
                    return -1;
                }
                it->second = {ev ? ev->events : 0, ev ? ev->data : 0, true};
                break;
            default:
                *guest_errno() = kEINVAL;
                return -1;
        }
    }
    fd_notify();
    return 0;
}
// Edge-triggered interest is reported level-triggered: a consumer that reads
// until EAGAIN, as edge-triggered code must, sees the same events.
WINAPK_GUEST_ABI int g_epoll_wait(int epfd, GuestEpollEvent* out, int max, int timeout) {
    auto ep = std::dynamic_pointer_cast<Epoll>(fd_object(epfd));
    if (!ep || max <= 0) {
        *guest_errno() = ep ? kEINVAL : kEBADF;
        return -1;
    }
    return wait_ready(timeout, [&](bool* polled) {
        int n = 0;
        std::vector<int> closed;
        std::lock_guard<std::mutex> hold(ep->lock);
        for (auto& [fd, in] : ep->interests) {
            if (n >= max) break;
            if (!in.armed) continue;
            const short want = static_cast<short>(in.events & 0xffff);
            const short r = check(fd, want, polled);
            if (r & kPollNval) {  // closed without EPOLL_CTL_DEL: Linux drops it too
                closed.push_back(fd);
                continue;
            }
            const uint32_t got = static_cast<uint32_t>(r) & (in.events | kPollErr | kPollHup);
            if (!got) continue;
            out[n].events = got;
            out[n].data = in.data;
            ++n;
            if (in.events & kEpollOneshot) in.armed = false;
        }
        for (int fd : closed) ep->interests.erase(fd);
        return n;
    });
}
WINAPK_GUEST_ABI int g_epoll_pwait(int epfd, GuestEpollEvent* out, int max, int timeout, const void*) {
    return g_epoll_wait(epfd, out, max, timeout);
}

// ---- creation ---------------------------------------------------------------------------
WINAPK_GUEST_ABI int g_pipe2(int fds[2], int flags) {
    auto buffer = std::make_shared<PipeBuffer>();
    auto r = std::make_shared<PipeReader>(buffer);
    auto w = std::make_shared<PipeWriter>(buffer);
    r->nonblocking = w->nonblocking = (flags & kONonblock) != 0;
    const int rfd = fd_install(r);
    if (rfd < 0) return -1;
    const int wfd = fd_install(w);
    if (wfd < 0) {
        fd_close(rfd);
        return -1;
    }
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}
WINAPK_GUEST_ABI int g_pipe(int fds[2]) { return g_pipe2(fds, 0); }

WINAPK_GUEST_ABI int g_eventfd(unsigned initial, int flags) {
    auto e = std::make_shared<EventFd>(initial, (flags & 1) != 0);  // EFD_SEMAPHORE
    e->nonblocking = (flags & kONonblock) != 0;
    return fd_install(e);
}
WINAPK_GUEST_ABI int g_eventfd_read(int fd, uint64_t* value) {
    auto o = fd_object(fd);
    return o && o->read(value, 8) == 8 ? 0 : -1;
}
WINAPK_GUEST_ABI int g_eventfd_write(int fd, uint64_t value) {
    auto o = fd_object(fd);
    return o && o->write(&value, 8) == 8 ? 0 : -1;
}

const ShimSymbol kSymbols[] = {
    WINAPK_SHIM("pipe", g_pipe),
    WINAPK_SHIM("pipe2", g_pipe2),
    WINAPK_SHIM("eventfd", g_eventfd),
    WINAPK_SHIM("eventfd_read", g_eventfd_read),
    WINAPK_SHIM("eventfd_write", g_eventfd_write),
    WINAPK_SHIM("poll", g_poll),
    WINAPK_SHIM("ppoll", g_ppoll),
    WINAPK_SHIM("select", g_select),
    WINAPK_SHIM("pselect", g_pselect),
    WINAPK_SHIM("epoll_create", g_epoll_create),
    WINAPK_SHIM("epoll_create1", g_epoll_create1),
    WINAPK_SHIM("epoll_ctl", g_epoll_ctl),
    WINAPK_SHIM("epoll_wait", g_epoll_wait),
    WINAPK_SHIM("epoll_pwait", g_epoll_pwait),
};

}  // namespace

std::shared_ptr<FdObject> fd_object(int fd) {
    std::lock_guard<std::mutex> hold(g_table_lock);
    auto it = g_table.find(fd);
    return it == g_table.end() ? nullptr : it->second;
}

int fd_install(std::shared_ptr<FdObject> object) {
    // The number: a CRT descriptor on NUL, so no file can be given the same one.
    const int fd = _open("NUL", _O_RDWR | _O_BINARY);
    if (fd < 0) {
        *guest_errno() = kEMFILE;
        return -1;
    }
    std::lock_guard<std::mutex> hold(g_table_lock);
    g_table[fd] = std::move(object);
    return fd;
}

bool fd_close(int fd) {
    std::shared_ptr<FdObject> dropped;  // destroyed outside the lock
    {
        std::lock_guard<std::mutex> hold(g_table_lock);
        auto it = g_table.find(fd);
        if (it == g_table.end()) return false;
        dropped = std::move(it->second);
        g_table.erase(it);
    }
    _close(fd);
    fd_notify();  // a waiter on this number sees POLLNVAL
    return true;
}

int fd_dup(int fd, int to) {
    auto o = fd_object(fd);
    if (!o) return -2;  // not an object: the caller duplicates the file
    if (fd == to) return to;
    int n;
    if (to < 0) {
        n = _open("NUL", _O_RDWR | _O_BINARY);
    } else {
        fd_close(to);
        n = _dup2(fd, to) == 0 ? to : -1;  // `fd` is a NUL placeholder too
    }
    if (n < 0) {
        *guest_errno() = kEMFILE;
        return -1;
    }
    std::lock_guard<std::mutex> hold(g_table_lock);
    g_table[n] = std::move(o);
    return n;
}

int fd_poll(PollFd* fds, uint64_t n, int timeout_ms) { return g_poll(fds, n, timeout_ms); }

void fd_notify() {
    g_generation.fetch_add(1);
    WakeByAddressAll(&g_generation);
}

ShimTable fd_symbols() { return WINAPK_SHIM_TABLE(kSymbols); }

}  // namespace winapk::bionic
