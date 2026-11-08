// socket.cpp -- BSD sockets for guest code, on Winsock.
//
// A guest socket is a descriptor object (fd.cpp) around a SOCKET, so read,
// write, close, poll and epoll work on it like on any other descriptor.
// Winsock is BSD sockets with different numbers in places, which are
// translated at the boundary:
//
//   AF_INET6      Linux 10, Windows 23 (AF_INET 2 and AF_UNIX 1 agree). The
//                 sockaddr layouts agree; only the family field changes.
//   SOL_SOCKET    Linux 1 with its own option numbers, Windows 0xffff;
//                 IP, IPv6 and TCP options partly differ too (kOptions).
//   errno         WSAGetLastError() values map to Linux's.
//   MSG_*         MSG_WAITALL differs; MSG_DONTWAIT has no Winsock flag and
//                 is checked against readiness; MSG_NOSIGNAL is moot.
//
// struct addrinfo matches: bionic, like Windows, keeps BSD's field order and
// AI_*/NI_*/EAI_* values; ai_addrlen is socklen_t followed by padding where
// Windows has a size_t, which reads the same.

#include "bionic_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>

namespace winapk::bionic {

namespace {

constexpr int kLinuxAfInet6 = 10;
constexpr int kSockNonblock = 04000, kSockCloexec = 02000000;

void winsock_init() {
    static const bool started = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)started;
}

int linux_from_wsa(int e) {
    switch (e) {
        case WSAEWOULDBLOCK: return kEAGAIN;
        case WSAEINPROGRESS: return 115;
        case WSAEALREADY: return 114;
        case WSAENOTSOCK: return 88;
        case WSAEDESTADDRREQ: return 89;
        case WSAEMSGSIZE: return 90;
        case WSAEPROTOTYPE: return 91;
        case WSAENOPROTOOPT: return 92;
        case WSAEPROTONOSUPPORT: return 93;
        case WSAESOCKTNOSUPPORT: return 94;
        case WSAEOPNOTSUPP: return kENOTSUP;
        case WSAEPFNOSUPPORT: return 96;
        case WSAEAFNOSUPPORT: return 97;
        case WSAEADDRINUSE: return 98;
        case WSAEADDRNOTAVAIL: return 99;
        case WSAENETDOWN: return 100;
        case WSAENETUNREACH: return 101;
        case WSAENETRESET: return 102;
        case WSAECONNABORTED: return 103;
        case WSAECONNRESET: return 104;
        case WSAENOBUFS: return 105;
        case WSAEISCONN: return 106;
        case WSAENOTCONN: return 107;
        case WSAESHUTDOWN: return 108;
        case WSAETIMEDOUT: return kETIMEDOUT;
        case WSAECONNREFUSED: return 111;
        case WSAEHOSTDOWN: return 112;
        case WSAEHOSTUNREACH: return 113;
        case WSAEINTR: return kEINTR;
        case WSAEBADF: return kEBADF;
        case WSAEACCES: return kEACCES;
        case WSAEFAULT: return kEFAULT;
        case WSAEINVAL: return kEINVAL;
        case WSAEMFILE: return kEMFILE;
        default: return kEIO;
    }
}
int fail_wsa() {
    *guest_errno() = linux_from_wsa(WSAGetLastError());
    return -1;
}

// ---- addresses ------------------------------------------------------------------------
int host_family(int f) { return f == kLinuxAfInet6 ? AF_INET6 : f; }
int guest_family(int f) { return f == AF_INET6 ? kLinuxAfInet6 : f; }

// A guest sockaddr copied with the host family; false if it is too short.
bool to_host(const void* guest, int len, sockaddr_storage* out, int* out_len) {
    if (!guest || len < 2 || len > static_cast<int>(sizeof(*out))) return false;
    std::memcpy(out, guest, static_cast<size_t>(len));
    out->ss_family = static_cast<ADDRESS_FAMILY>(host_family(out->ss_family));
    *out_len = len;
    return true;
}
// Copies a host sockaddr out to the guest's buffer and length, BSD-style
// (truncated to the buffer, the full length reported).
void to_guest(const sockaddr_storage& addr, int len, void* guest, uint32_t* guest_len) {
    if (!guest || !guest_len) return;
    sockaddr_storage copy = addr;
    copy.ss_family = static_cast<ADDRESS_FAMILY>(guest_family(copy.ss_family));
    std::memcpy(guest, &copy, std::min<size_t>(*guest_len, static_cast<size_t>(len)));
    *guest_len = static_cast<uint32_t>(len);
}

// ---- the per-app proxy ----------------------------------------------------------------
// WINAPK_PROXY=socks5://[user[:password]@]host:port is the launcher's per-app
// proxy; Java code reaches it through socksProxyHost. An app's own network stack
// in its .so (WhatsApp's chat socket) calls connect() here instead, so stream
// connects to addresses outside this machine and the LAN go through the same
// proxy. The proxy is asked for the address the guest resolved; UDP stays
// direct, and so does a socket the guest bound to one local address (it chose
// the interface). A connect the proxy cannot carry fails rather than going
// direct. WINAPK_NATIVE_PROXY=0 turns this off.
struct ProxyConfig {
    sockaddr_storage addr{};
    int addr_len = 0;
    std::string user, password;
};
const ProxyConfig* proxy_config() {
    static const ProxyConfig* const config = []() -> const ProxyConfig* {
        const char* off = std::getenv("WINAPK_NATIVE_PROXY");
        const char* v = std::getenv("WINAPK_PROXY");
        if ((off && std::strcmp(off, "0") == 0) || !v || !*v) return nullptr;
        std::string s(v);
        const size_t scheme = s.find("://");
        std::string kind = scheme == std::string::npos ? "" : s.substr(0, scheme);
        for (char& ch : kind) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (kind != "socks5" && kind != "socks5h" && kind != "socks") {
            std::fprintf(stderr, "[win.apk] WINAPK_PROXY ignored by native sockets, expected socks5://host:port\n");
            return nullptr;
        }
        s.erase(0, scheme + 3);
        s = s.substr(0, s.find_first_of("/ "));  // the authority only
        auto* c = new ProxyConfig;
        if (const size_t at = s.rfind('@'); at != std::string::npos) {
            const std::string cred = s.substr(0, at);
            const size_t colon = cred.find(':');
            c->user = cred.substr(0, colon);
            if (colon != std::string::npos) c->password = cred.substr(colon + 1);
            s.erase(0, at + 1);
        }
        std::string host, port;
        if (!s.empty() && s[0] == '[') {  // [IPv6]:port
            const size_t close = s.find(']');
            if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == ':') {
                host = s.substr(1, close - 1);
                port = s.substr(close + 2);
            }
        } else if (const size_t colon = s.rfind(':'); colon != std::string::npos) {
            host = s.substr(0, colon);
            port = s.substr(colon + 1);
        }
        // IPv4 first: an IPv4 socket cannot reach an IPv6 proxy, while an IPv6
        // socket reaches an IPv4 one dual-stack ("localhost" is ::1 first).
        winsock_init();
        ADDRINFOA* list = nullptr;
        for (const int family : {AF_INET, AF_UNSPEC}) {
            ADDRINFOA hints{};
            hints.ai_family = family;
            hints.ai_socktype = SOCK_STREAM;
            if (!host.empty() && !port.empty() && ::getaddrinfo(host.c_str(), port.c_str(), &hints, &list) == 0 && list)
                break;
            list = nullptr;
        }
        if (!list) {
            std::fprintf(stderr, "[win.apk] WINAPK_PROXY ignored by native sockets: cannot resolve its host:port\n");
            delete c;
            return nullptr;
        }
        std::memcpy(&c->addr, list->ai_addr, list->ai_addrlen);
        c->addr_len = static_cast<int>(list->ai_addrlen);
        freeaddrinfo(list);
        const bool v6 = host.find(':') != std::string::npos;
        std::fprintf(stderr, "[win.apk] native TCP connects go through the SOCKS5 proxy %s%s%s:%s\n", v6 ? "[" : "",
                     host.c_str(), v6 ? "]" : "", port.c_str());
        return c;
    }();
    return config;
}

// A destination as SOCKS5 names it: IPv4 (a v4-mapped IPv6 address included) or
// IPv6, and the port in network order.
struct Target {
    bool v4;
    uint8_t ip[16];
    uint16_t port;
};
bool target_of(const sockaddr_storage& a, Target* t) {
    if (a.ss_family == AF_INET) {
        const auto& s = reinterpret_cast<const sockaddr_in&>(a);
        t->v4 = true;
        std::memcpy(t->ip, &s.sin_addr, 4);
        t->port = s.sin_port;
        return true;
    }
    if (a.ss_family == AF_INET6) {
        const auto& s = reinterpret_cast<const sockaddr_in6&>(a);
        static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        t->v4 = std::memcmp(s.sin6_addr.s6_addr, kMapped, 12) == 0;
        std::memcpy(t->ip, s.sin6_addr.s6_addr + (t->v4 ? 12 : 0), t->v4 ? 4 : 16);
        t->port = s.sin6_port;
        return true;
    }
    return false;
}
// This machine, the LAN (private, CGNAT/Tailscale, benchmark/fake-IP ranges),
// link-local and multicast: never through the proxy.
bool is_local(const Target& t) {
    const uint8_t* b = t.ip;
    if (t.v4) {
        return b[0] == 127 || b[0] == 0 || b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) ||
               (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254) || (b[0] == 100 && (b[1] & 0xC0) == 64) ||
               (b[0] == 198 && (b[1] & 0xFE) == 18) || b[0] >= 224;
    }
    static const uint8_t kZero[15] = {};
    return (std::memcmp(b, kZero, 15) == 0 && b[15] <= 1) ||  // :: and ::1
           (b[0] & 0xFE) == 0xFC || (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) || b[0] == 0xFF;
}
bool is_unspecified_or_loopback(const Target& t) {
    static const uint8_t kZero[16] = {};
    if (t.v4) return t.ip[0] == 127 || std::memcmp(t.ip, kZero, 4) == 0;
    return std::memcmp(t.ip, kZero, 15) == 0 && t.ip[15] <= 1;
}
std::string describe(const Target& t) {
    char ip[64] = "?";
    ::inet_ntop(t.v4 ? AF_INET : AF_INET6, t.ip, ip, sizeof(ip));
    return (t.v4 ? std::string(ip) : "[" + std::string(ip) + "]") + ":" + std::to_string(ntohs(t.port));
}
// A refused or failed proxied connect: the first 20, then every 100th.
void log_proxy_failure(const Target& t, int e) {
    static std::atomic<unsigned> count{0};
    const unsigned n = ++count;
    if (n <= 20 || n % 100 == 0)
        std::fprintf(stderr, "[win.apk] proxy: connect to %s failed (errno %d, failure #%u)\n", describe(t).c_str(), e, n);
}

constexpr ULONGLONG kProxyTimeoutMs = 75000;
using Abandoned = std::function<bool()>;

// Waits until `h` is readable (or writable) or `deadline` passes: 0, or a Linux
// errno. Short waits, so a handshake whose socket the guest closed ends early.
int wait_socket(SOCKET h, bool write, ULONGLONG deadline, const Abandoned& abandoned) {
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return kETIMEDOUT;
        if (abandoned()) return 125;  // ECANCELED
        fd_set ready, failed;
        FD_ZERO(&ready);
        FD_ZERO(&failed);
        FD_SET(h, &ready);
        FD_SET(h, &failed);
        timeval tv{0, static_cast<long>(std::min<ULONGLONG>(deadline - now, 200) * 1000)};
        const int r = ::select(0, write ? nullptr : &ready, write ? &ready : nullptr, &failed, &tv);
        if (r == SOCKET_ERROR) return linux_from_wsa(WSAGetLastError());
        if (r == 0) continue;
        if (FD_ISSET(h, &failed)) {  // a failed connect
            int e = 0, el = sizeof(e);
            getsockopt(h, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &el);
            return e ? linux_from_wsa(e) : 111;  // ECONNREFUSED
        }
        return 0;
    }
}
int send_all(SOCKET h, const uint8_t* p, size_t n, ULONGLONG deadline, const Abandoned& abandoned) {
    while (n) {
        const int r = ::send(h, reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
        if (r > 0) {
            p += r;
            n -= static_cast<size_t>(r);
            continue;
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) return linux_from_wsa(WSAGetLastError());
        if (const int e = wait_socket(h, true, deadline, abandoned)) return e;
    }
    return 0;
}
int recv_exact(SOCKET h, uint8_t* p, size_t n, ULONGLONG deadline, const Abandoned& abandoned) {
    while (n) {
        if (const int e = wait_socket(h, false, deadline, abandoned)) return e;
        const int r = ::recv(h, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
        if (r > 0) {
            p += r;
            n -= static_cast<size_t>(r);
            continue;
        }
        if (r == 0) return 104;  // ECONNRESET: the proxy hung up
        if (WSAGetLastError() != WSAEWOULDBLOCK) return linux_from_wsa(WSAGetLastError());
    }
    return 0;
}

// Connects `h` to the proxy at `via` and has it open a stream to `t`; 0 when the
// stream reaches `t`, else a Linux errno. Works on blocking and non-blocking
// sockets alike, reading no byte past the proxy's reply.
int socks5_connect(SOCKET h, const ProxyConfig& p, const sockaddr_storage& via, int via_len, const Target& t,
                   const Abandoned& abandoned) {
    const ULONGLONG deadline = GetTickCount64() + kProxyTimeoutMs;
    if (::connect(h, reinterpret_cast<const sockaddr*>(&via), via_len) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) return linux_from_wsa(WSAGetLastError());
        if (const int e = wait_socket(h, true, deadline, abandoned)) return e;
        int e = 0, el = sizeof(e);
        getsockopt(h, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &el);
        if (e) return linux_from_wsa(e);
    }
    constexpr int kEPROTO = 71;
    const bool auth = !p.user.empty();
    uint8_t b[520];
    const uint8_t hello[] = {5, static_cast<uint8_t>(auth ? 2 : 1), 0, 2};  // no auth, user/password
    if (const int e = send_all(h, hello, auth ? 4 : 3, deadline, abandoned)) return e;
    if (const int e = recv_exact(h, b, 2, deadline, abandoned)) return e;
    if (b[0] != 5) return kEPROTO;
    if (b[1] == 2 && auth) {  // RFC 1929
        const size_t ul = std::min<size_t>(p.user.size(), 255), pl = std::min<size_t>(p.password.size(), 255);
        size_t n = 0;
        b[n++] = 1;
        b[n++] = static_cast<uint8_t>(ul);
        std::memcpy(b + n, p.user.data(), ul);
        n += ul;
        b[n++] = static_cast<uint8_t>(pl);
        std::memcpy(b + n, p.password.data(), pl);
        n += pl;
        if (const int e = send_all(h, b, n, deadline, abandoned)) return e;
        if (const int e = recv_exact(h, b, 2, deadline, abandoned)) return e;
        if (b[1] != 0) return kEACCES;
    } else if (b[1] != 0) {
        return kEACCES;  // no method both sides accept
    }
    size_t n = 0;
    b[n++] = 5;
    b[n++] = 1;  // CONNECT
    b[n++] = 0;
    b[n++] = t.v4 ? 1 : 4;
    std::memcpy(b + n, t.ip, t.v4 ? 4 : 16);
    n += t.v4 ? 4 : 16;
    std::memcpy(b + n, &t.port, 2);
    n += 2;
    if (const int e = send_all(h, b, n, deadline, abandoned)) return e;
    if (const int e = recv_exact(h, b, 4, deadline, abandoned)) return e;
    if (b[0] != 5) return kEPROTO;
    switch (b[1]) {
        case 0: break;
        case 2: return kEACCES;            // not allowed by the ruleset
        case 3: return 101;                // ENETUNREACH
        case 4: return 113;                // EHOSTUNREACH
        case 6: return kETIMEDOUT;         // TTL expired
        default: return 111;               // ECONNREFUSED (refused, general failure, ...)
    }
    size_t bound = b[3] == 1 ? 4 + 2 : b[3] == 4 ? 16 + 2 : 0;  // the proxy's bound address, skipped
    if (b[3] == 3) {
        if (const int e = recv_exact(h, b, 1, deadline, abandoned)) return e;
        bound = b[0] + 2u;
    }
    if (!bound) return kEPROTO;
    if (const int e = recv_exact(h, b, bound, deadline, abandoned)) return e;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) std::fprintf(stderr, "[win.apk] proxy: first native connect through it reached %s\n", describe(t).c_str());
    return 0;
}

// ---- the descriptor object ------------------------------------------------------------
class Socket final : public FdObject {
public:
    explicit Socket(SOCKET s) : s_(s) {}
    ~Socket() override { closesocket(s_); }
    SOCKET handle() const { return s_; }

    // A stream connect() sent through the per-app proxy runs the SOCKS5 handshake
    // first; until it is done the stream is not the guest's.
    enum ProxyState { kDirect, kHandshaking, kProxied, kProxyFailed };
    std::atomic<int> proxy_state{kDirect};
    std::atomic<int> proxy_error{0};  // a failed handshake's errno, until SO_ERROR reads it
    sockaddr_storage proxy_peer{};    // the destination, for getpeername
    int proxy_peer_len = 0;

    int64_t read(void* buf, size_t n) override { return recv_flags(buf, n, 0); }
    int64_t write(const void* buf, size_t n) override { return send_flags(buf, n, 0); }
    short ready(short events) override {
        switch (proxy_state.load()) {
            case kHandshaking: return 0;
            case kProxyFailed: return static_cast<short>(0x8 | 0x10 | (events & 0x5));  // as a refused connect
            default: break;
        }
        WSAPOLLFD p{s_, 0, 0};
        if (events & 0x1) p.events |= POLLRDNORM;
        if (events & 0x4) p.events |= POLLWRNORM;
        if (WSAPoll(&p, 1, 0) < 0) return 0x20;  // POLLNVAL
        short r = 0;
        if (p.revents & (POLLRDNORM | POLLRDBAND)) r |= 0x1;
        if (p.revents & POLLWRNORM) r |= 0x4;
        if (p.revents & POLLERR) r |= 0x8;
        if (p.revents & POLLHUP) r |= 0x10 | (events & 0x1);  // readable at EOF, as on Linux
        return r;
    }
    unsigned mode() const override { return 0140000; }  // S_IFSOCK
    bool needs_polling() const override { return true; }
    void set_nonblocking(bool on) override {
        u_long v = on ? 1 : 0;
        ioctlsocket(s_, FIONBIO, &v);
        nonblocking = on;
    }
    int ioctl(int request, void* arg) override {
        if (request == 0x541B) {  // FIONREAD
            if (const int ps = proxy_state.load(); ps == kHandshaking || ps == kProxyFailed) {  // the proxy's bytes
                *static_cast<int*>(arg) = 0;
                return 0;
            }
            u_long n = 0;
            if (ioctlsocket(s_, FIONREAD, &n) != 0) return fail_wsa();
            *static_cast<int*>(arg) = static_cast<int>(n);
            return 0;
        }
        return FdObject::ioctl(request, arg);
    }

    // True (errno set) when a send/recv cannot go ahead: during a proxy
    // handshake (EAGAIN; a blocking call waits it out, as for a connect in
    // progress) or after it failed (ENOTCONN), and for MSG_DONTWAIT (0x40) on a
    // blocking socket that is not ready (EAGAIN).
    bool would_block(int flags, short want) {
        if (!nonblocking && !(flags & 0x40)) {
            while (proxy_state.load() == kHandshaking) Sleep(2);
        }
        if (const int ps = proxy_state.load(); ps == kHandshaking || ps == kProxyFailed) {
            *guest_errno() = ps == kHandshaking ? kEAGAIN : 107;  // ENOTCONN
            return true;
        }
        if (!(flags & 0x40) || nonblocking) return false;
        if (ready(want) & (want | 0x18)) return false;
        *guest_errno() = kEAGAIN;
        return true;
    }
    static int host_msg_flags(int f) {
        int h = f & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE);
        if (f & 0x100) h |= MSG_WAITALL;
        return h;
    }
    int64_t recv_flags(void* buf, size_t n, int flags) {
        if (would_block(flags, 0x1)) return -1;
        const int r = ::recv(s_, static_cast<char*>(buf), static_cast<int>(std::min<size_t>(n, INT_MAX)),
                             host_msg_flags(flags));
        if (r == SOCKET_ERROR) return fail_wsa();
        fd_notify();
        return r;
    }
    int64_t send_flags(const void* buf, size_t n, int flags) {
        if (would_block(flags, 0x4)) return -1;
        const int r = ::send(s_, static_cast<const char*>(buf), static_cast<int>(std::min<size_t>(n, INT_MAX)),
                             host_msg_flags(flags));
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAESHUTDOWN) {
                *guest_errno() = 32;  // EPIPE
                return -1;
            }
            return fail_wsa();
        }
        fd_notify();
        return r;
    }

private:
    SOCKET s_;
};

std::shared_ptr<Socket> socket_of(int fd) {
    auto s = std::dynamic_pointer_cast<Socket>(fd_object(fd));
    if (!s) *guest_errno() = fd_object(fd) || fd < 0 ? 88 : kEBADF;  // ENOTSOCK
    return s;
}

int install(SOCKET h, int type_flags) {
    auto s = std::make_shared<Socket>(h);
    if (type_flags & kSockNonblock) s->set_nonblocking(true);
    const int fd = fd_install(s);
    return fd;
}

// ---- calls -------------------------------------------------------------------------------
WINAPK_GUEST_ABI int g_socket(int family, int type, int protocol) {
    winsock_init();
    const SOCKET h = ::socket(host_family(family), type & ~(kSockNonblock | kSockCloexec), protocol);
    if (h == INVALID_SOCKET) return fail_wsa();
    if (family == kLinuxAfInet6) {  // dual-stack, Linux's default (bindv6only=0); Windows starts v6-only
        DWORD off = 0;
        setsockopt(h, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    }
    return install(h, type);
}

// socketpair: Winsock has none; a connected loopback TCP pair stands in (AF_UNIX
// stream semantics, which is what callers use it for).
WINAPK_GUEST_ABI int g_socketpair(int, int type, int, int out[2]) {
    winsock_init();
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(a);
    SOCKET c = INVALID_SOCKET, s = INVALID_SOCKET;
    if (listener != INVALID_SOCKET && ::bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 &&
        ::listen(listener, 1) == 0 && getsockname(listener, reinterpret_cast<sockaddr*>(&a), &len) == 0 &&
        (c = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) != INVALID_SOCKET &&
        ::connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 &&
        (s = ::accept(listener, nullptr, nullptr)) != INVALID_SOCKET) {
        closesocket(listener);
        BOOL one = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        out[0] = install(c, type);
        out[1] = install(s, type);
        return out[0] >= 0 && out[1] >= 0 ? 0 : -1;
    }
    const int r = fail_wsa();
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (c != INVALID_SOCKET) closesocket(c);
    return r;
}

WINAPK_GUEST_ABI int g_bind(int fd, const void* addr, uint32_t len) {
    auto s = socket_of(fd);
    sockaddr_storage h;
    int hl;
    if (!s) return -1;
    if (!to_host(addr, static_cast<int>(len), &h, &hl)) {
        *guest_errno() = kEINVAL;
        return -1;
    }
    return ::bind(s->handle(), reinterpret_cast<sockaddr*>(&h), hl) == 0 ? 0 : fail_wsa();
}
WINAPK_GUEST_ABI int g_listen(int fd, int backlog) {
    auto s = socket_of(fd);
    if (!s) return -1;
    return ::listen(s->handle(), backlog) == 0 ? 0 : fail_wsa();
}
WINAPK_GUEST_ABI int g_accept4(int fd, void* addr, uint32_t* len, int flags) {
    auto s = socket_of(fd);
    if (!s) return -1;
    sockaddr_storage h{};
    int hl = sizeof(h);
    const SOCKET c = ::accept(s->handle(), reinterpret_cast<sockaddr*>(&h), &hl);
    if (c == INVALID_SOCKET) return fail_wsa();
    to_guest(h, hl, addr, len);
    // A socket accepted from a non-blocking listener is blocking on Linux;
    // Winsock copies the listener's mode, so it is set explicitly.
    u_long v = (flags & kSockNonblock) ? 1 : 0;
    ioctlsocket(c, FIONBIO, &v);
    return install(c, flags);
}
WINAPK_GUEST_ABI int g_accept(int fd, void* addr, uint32_t* len) { return g_accept4(fd, addr, len, 0); }

// connect() through the per-app proxy when `dest` should go there; false leaves
// the connect to the caller. A non-blocking socket gets EINPROGRESS and the
// handshake runs on a thread, as a direct connect would run in the kernel.
bool proxy_connect(const std::shared_ptr<Socket>& s, const ProxyConfig& p, const sockaddr_storage& dest, int dest_len,
                   int* result) {
    Target t, pt;
    int type = 0, tl = sizeof(type);
    const int full_len = dest.ss_family == AF_INET ? int(sizeof(sockaddr_in)) : int(sizeof(sockaddr_in6));
    if (dest_len < (dest.ss_family == AF_INET ? int(sizeof(sockaddr_in)) : 24) ||  // too short: Winsock refuses it
        !target_of(dest, &t) || is_local(t) || !target_of(p.addr, &pt) ||
        (t.v4 == pt.v4 && t.port == pt.port && std::memcmp(t.ip, pt.ip, t.v4 ? 4 : 16) == 0) ||
        getsockopt(s->handle(), SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &tl) != 0 || type != SOCK_STREAM) {
        return false;
    }
    // Bound to one local address: the guest picked the interface, which the
    // loopback hop to the proxy cannot keep, so it connects directly.
    sockaddr_storage bound{};
    int bl = sizeof(bound);
    Target bt;
    if (getsockname(s->handle(), reinterpret_cast<sockaddr*>(&bound), &bl) == 0 && target_of(bound, &bt) &&
        !is_unspecified_or_loopback(bt)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            std::fprintf(stderr, "[win.apk] proxy: a socket bound to %s connects directly\n", describe(bt).c_str());
        return false;
    }
    auto refuse = [&](const char* why) {  // the proxy cannot carry it: fail, never go direct
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) std::fprintf(stderr, "[win.apk] proxy: %s, connect refused\n", why);
        *guest_errno() = 101;  // ENETUNREACH
        *result = -1;
        return true;
    };
    // The socket has the destination's family; an IPv6 one reaches an IPv4 proxy dual-stack.
    sockaddr_storage via = p.addr;
    int via_len = p.addr_len;
    if (dest.ss_family != via.ss_family) {
        if (dest.ss_family != AF_INET6) return refuse("an IPv4 socket cannot reach the IPv6 proxy");
        DWORD v6only = 1;
        int vl = sizeof(v6only);
        if (getsockopt(s->handle(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), &vl) != 0 || v6only) {
            const DWORD off = 0;
            if (setsockopt(s->handle(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off)) != 0)
                return refuse("an IPv6-only socket cannot reach the IPv4 proxy");
        }
        sockaddr_in6 m{};
        m.sin6_family = AF_INET6;
        m.sin6_port = pt.port;
        m.sin6_addr.s6_addr[10] = m.sin6_addr.s6_addr[11] = 0xFF;
        std::memcpy(m.sin6_addr.s6_addr + 12, pt.ip, 4);
        via = {};
        std::memcpy(&via, &m, sizeof(m));
        via_len = sizeof(m);
    }
    int expected = Socket::kDirect;
    if (!s->proxy_state.compare_exchange_strong(expected, Socket::kHandshaking)) {
        *guest_errno() = 114;  // EALREADY: another thread's connect got there first
        *result = -1;
        return true;
    }
    s->proxy_peer = {};
    std::memcpy(&s->proxy_peer, &dest, static_cast<size_t>(std::min(dest_len, full_len)));
    s->proxy_peer_len = full_len;
    if (!s->nonblocking) {
        const int e = socks5_connect(s->handle(), p, via, via_len, t, [] { return false; });
        if (e) {
            log_proxy_failure(t, e);
            // Not even connected to the proxy: the socket can try again, as after a failed connect.
            sockaddr_storage peer;
            int pl = sizeof(peer);
            const bool connected = getpeername(s->handle(), reinterpret_cast<sockaddr*>(&peer), &pl) == 0;
            s->proxy_state = connected ? Socket::kProxyFailed : Socket::kDirect;
            *guest_errno() = e;
        } else {
            s->proxy_state = Socket::kProxied;
        }
        *result = e ? -1 : 0;
        return true;
    }
    try {
        std::thread([s, &p, via, via_len, t] {
            // Only this thread still holding the socket: the guest closed it.
            const int e = socks5_connect(s->handle(), p, via, via_len, t, [&s] { return s.use_count() == 1; });
            if (e) {
                if (e != 125) log_proxy_failure(t, e);
                s->proxy_error = e;
            }
            s->proxy_state = e ? Socket::kProxyFailed : Socket::kProxied;
            fd_notify();
        }).detach();
    } catch (...) {  // no thread: nothing was sent, the socket stays as it was
        s->proxy_state = Socket::kDirect;
        *guest_errno() = 105;  // ENOBUFS
        *result = -1;
        return true;
    }
    *guest_errno() = 115;  // EINPROGRESS
    *result = -1;
    return true;
}

WINAPK_GUEST_ABI int g_connect(int fd, const void* addr, uint32_t len) {
    auto s = socket_of(fd);
    sockaddr_storage h;
    int hl;
    if (!s) return -1;
    if (!to_host(addr, static_cast<int>(len), &h, &hl)) {
        *guest_errno() = kEINVAL;
        return -1;
    }
    if ((h.ss_family == AF_INET && hl < static_cast<int>(sizeof(sockaddr_in))) || (h.ss_family == AF_INET6 && hl < 24)) {
        *guest_errno() = kEINVAL;  // as Linux answers a short address
        return -1;
    }
    if (const int ps = s->proxy_state.load(); ps != Socket::kDirect) {
        // connect() again on a proxied stream: EALREADY, EISCONN, or the failure once
        const int e = ps == Socket::kProxyFailed ? s->proxy_error.exchange(0) : 0;
        *guest_errno() = ps == Socket::kHandshaking ? 114 : ps == Socket::kProxied ? 106 : e ? e : 103;
        return -1;
    }
    int proxied = 0;
    if (const ProxyConfig* p = proxy_config(); p && proxy_connect(s, *p, h, hl, &proxied)) return proxied;
    if (::connect(s->handle(), reinterpret_cast<sockaddr*>(&h), hl) == 0) return 0;
    // A non-blocking connect in progress is EINPROGRESS on Linux.
    if (WSAGetLastError() == WSAEWOULDBLOCK) {
        *guest_errno() = 115;
        return -1;
    }
    return fail_wsa();
}
WINAPK_GUEST_ABI int g_shutdown(int fd, int how) {
    auto s = socket_of(fd);
    if (!s) return -1;
    const int r = ::shutdown(s->handle(), how) == 0 ? 0 : fail_wsa();
    fd_notify();
    return r;
}

WINAPK_GUEST_ABI int64_t g_send(int fd, const void* buf, size_t n, int flags) {
    auto s = socket_of(fd);
    return s ? s->send_flags(buf, n, flags) : -1;
}
WINAPK_GUEST_ABI int64_t g_recv(int fd, void* buf, size_t n, int flags) {
    auto s = socket_of(fd);
    return s ? s->recv_flags(buf, n, flags) : -1;
}
WINAPK_GUEST_ABI int64_t g_sendto(int fd, const void* buf, size_t n, int flags, const void* addr, uint32_t len) {
    auto s = socket_of(fd);
    if (!s) return -1;
    if (!addr) return s->send_flags(buf, n, flags);
    sockaddr_storage h;
    int hl;
    if (!to_host(addr, static_cast<int>(len), &h, &hl)) {
        *guest_errno() = kEINVAL;
        return -1;
    }
    if (s->would_block(flags, 0x4)) return -1;
    const int r = ::sendto(s->handle(), static_cast<const char*>(buf), static_cast<int>(n),
                           Socket::host_msg_flags(flags), reinterpret_cast<sockaddr*>(&h), hl);
    return r == SOCKET_ERROR ? fail_wsa() : r;
}
WINAPK_GUEST_ABI int64_t g_recvfrom(int fd, void* buf, size_t n, int flags, void* addr, uint32_t* len) {
    auto s = socket_of(fd);
    if (!s) return -1;
    if (s->would_block(flags, 0x1)) return -1;
    sockaddr_storage h{};
    int hl = sizeof(h);
    const int r = ::recvfrom(s->handle(), static_cast<char*>(buf), static_cast<int>(n), Socket::host_msg_flags(flags),
                             reinterpret_cast<sockaddr*>(&h), &hl);
    if (r == SOCKET_ERROR) return fail_wsa();
    if (addr && len) to_guest(h, hl, addr, len);
    return r;
}

// struct msghdr / iovec (x86_64): name, namelen (u32), iov, iovlen (size_t),
// control, controllen (size_t), flags.
struct GuestIovec {
    void* base;
    size_t len;
};
struct GuestMsghdr {
    void* name;
    uint32_t namelen;
    GuestIovec* iov;
    size_t iovlen;
    void* control;
    size_t controllen;
    int flags;
};
WINAPK_GUEST_ABI int64_t g_sendmsg(int fd, const GuestMsghdr* m, int flags) {
    auto s = socket_of(fd);
    if (!s) return -1;
    if (s->would_block(flags, 0x4)) return -1;
    std::vector<WSABUF> bufs(m->iovlen);
    for (size_t i = 0; i < m->iovlen; ++i) bufs[i] = {static_cast<ULONG>(m->iov[i].len), static_cast<char*>(m->iov[i].base)};
    sockaddr_storage h;
    int hl = 0;
    if (m->name && !to_host(m->name, static_cast<int>(m->namelen), &h, &hl)) {
        *guest_errno() = kEINVAL;
        return -1;
    }
    DWORD sent = 0;
    if (WSASendTo(s->handle(), bufs.data(), static_cast<DWORD>(bufs.size()), &sent,
                  static_cast<DWORD>(Socket::host_msg_flags(flags)), m->name ? reinterpret_cast<sockaddr*>(&h) : nullptr,
                  hl, nullptr, nullptr) != 0) {
        return fail_wsa();
    }
    return sent;
}
WINAPK_GUEST_ABI int64_t g_recvmsg(int fd, GuestMsghdr* m, int flags) {
    auto s = socket_of(fd);
    if (!s) return -1;
    if (s->would_block(flags, 0x1)) return -1;
    std::vector<WSABUF> bufs(m->iovlen);
    for (size_t i = 0; i < m->iovlen; ++i) bufs[i] = {static_cast<ULONG>(m->iov[i].len), static_cast<char*>(m->iov[i].base)};
    sockaddr_storage h{};
    INT hl = sizeof(h);
    DWORD got = 0, wflags = static_cast<DWORD>(Socket::host_msg_flags(flags));
    if (WSARecvFrom(s->handle(), bufs.data(), static_cast<DWORD>(bufs.size()), &got, &wflags,
                    m->name ? reinterpret_cast<sockaddr*>(&h) : nullptr, m->name ? &hl : nullptr, nullptr,
                    nullptr) != 0) {
        return fail_wsa();
    }
    if (m->name) to_guest(h, hl, m->name, &m->namelen);
    m->controllen = 0;  // no ancillary data (SCM_RIGHTS has no Winsock form)
    m->flags = (wflags & MSG_PARTIAL) ? 0x20 : 0;  // MSG_TRUNC
    return got;
}

WINAPK_GUEST_ABI int g_getsockname(int fd, void* addr, uint32_t* len) {
    auto s = socket_of(fd);
    if (!s) return -1;
    sockaddr_storage h{};
    int hl = sizeof(h);
    if (getsockname(s->handle(), reinterpret_cast<sockaddr*>(&h), &hl) != 0) return fail_wsa();
    to_guest(h, hl, addr, len);
    return 0;
}
WINAPK_GUEST_ABI int g_getpeername(int fd, void* addr, uint32_t* len) {
    auto s = socket_of(fd);
    if (!s) return -1;
    switch (s->proxy_state.load()) {
        case Socket::kProxied:  // the destination, not the proxy
            to_guest(s->proxy_peer, s->proxy_peer_len, addr, len);
            return 0;
        case Socket::kHandshaking:
        case Socket::kProxyFailed:
            *guest_errno() = 107;  // ENOTCONN
            return -1;
        default: break;
    }
    sockaddr_storage h{};
    int hl = sizeof(h);
    if (getpeername(s->handle(), reinterpret_cast<sockaddr*>(&h), &hl) != 0) return fail_wsa();
    to_guest(h, hl, addr, len);
    return 0;
}

// ---- socket options ----------------------------------------------------------------------
struct Option {
    int linux_level, linux_name, level, name;
    bool timeval;  // a struct timeval on Linux, a DWORD of milliseconds on Windows
};
const Option kOptions[] = {
    {1, 2, SOL_SOCKET, SO_REUSEADDR, false},  {1, 3, SOL_SOCKET, SO_TYPE, false},
    {1, 4, SOL_SOCKET, SO_ERROR, false},      {1, 5, SOL_SOCKET, SO_DONTROUTE, false},
    {1, 6, SOL_SOCKET, SO_BROADCAST, false},  {1, 7, SOL_SOCKET, SO_SNDBUF, false},
    {1, 8, SOL_SOCKET, SO_RCVBUF, false},     {1, 9, SOL_SOCKET, SO_KEEPALIVE, false},
    {1, 10, SOL_SOCKET, SO_OOBINLINE, false}, {1, 13, SOL_SOCKET, SO_LINGER, false},
    {1, 20, SOL_SOCKET, SO_RCVTIMEO, true},   {1, 21, SOL_SOCKET, SO_SNDTIMEO, true},
    {1, 30, SOL_SOCKET, SO_ACCEPTCONN, false},
    {6, 1, IPPROTO_TCP, TCP_NODELAY, false},  {6, 4, IPPROTO_TCP, 3, false},  // TCP_KEEPIDLE
    {6, 5, IPPROTO_TCP, 17, false},           {6, 6, IPPROTO_TCP, 16, false},  // TCP_KEEPINTVL, TCP_KEEPCNT
    {0, 1, IPPROTO_IP, IP_TOS, false},        {0, 2, IPPROTO_IP, IP_TTL, false},
    {0, 32, IPPROTO_IP, IP_MULTICAST_IF, false}, {0, 33, IPPROTO_IP, IP_MULTICAST_TTL, false},
    {0, 34, IPPROTO_IP, IP_MULTICAST_LOOP, false}, {0, 35, IPPROTO_IP, IP_ADD_MEMBERSHIP, false},
    {0, 36, IPPROTO_IP, IP_DROP_MEMBERSHIP, false},
    {41, 26, IPPROTO_IPV6, IPV6_V6ONLY, false}, {41, 16, IPPROTO_IPV6, IPV6_UNICAST_HOPS, false},
    {41, 17, IPPROTO_IPV6, IPV6_MULTICAST_IF, false}, {41, 18, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, false},
    {41, 19, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, false}, {41, 20, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, false},
    {41, 21, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, false},
};
const Option* find_option(int level, int name) {
    for (const Option& o : kOptions) {
        if (o.linux_level == level && o.linux_name == name) return &o;
    }
    return nullptr;
}

// Options Winsock lacks (SO_REUSEPORT, SO_PRIORITY, TCP_QUICKACK...) are
// accepted and ignored: they tune, they do not change what a program does.
WINAPK_GUEST_ABI int g_setsockopt(int fd, int level, int name, const void* value, uint32_t len) {
    auto s = socket_of(fd);
    if (!s) return -1;
    const Option* o = find_option(level, name);
    if (!o) return 0;
    if (o->timeval) {
        if (len < 16) {
            *guest_errno() = kEINVAL;
            return -1;
        }
        int64_t tv[2];
        std::memcpy(tv, value, 16);
        const DWORD ms = static_cast<DWORD>(tv[0] * 1000 + tv[1] / 1000);
        return setsockopt(s->handle(), o->level, o->name, reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0
                   ? 0
                   : fail_wsa();
    }
    return setsockopt(s->handle(), o->level, o->name, static_cast<const char*>(value), static_cast<int>(len)) == 0
               ? 0
               : fail_wsa();
}
WINAPK_GUEST_ABI int g_getsockopt(int fd, int level, int name, void* value, uint32_t* len) {
    auto s = socket_of(fd);
    if (!s) return -1;
    if (level == 1 && name == 4 && s->proxy_state.load() == Socket::kProxyFailed && *len >= 4) {
        *static_cast<int*>(value) = s->proxy_error.exchange(0);  // SO_ERROR of the handshake, read once
        *len = 4;
        return 0;
    }
    const Option* o = find_option(level, name);
    if (!o) {
        *guest_errno() = 92;  // ENOPROTOOPT
        return -1;
    }
    if (o->timeval) {
        DWORD ms = 0;
        int ml = sizeof(ms);
        if (getsockopt(s->handle(), o->level, o->name, reinterpret_cast<char*>(&ms), &ml) != 0) return fail_wsa();
        const int64_t tv[2] = {ms / 1000, (ms % 1000) * 1000};
        std::memcpy(value, tv, std::min<size_t>(*len, 16));
        *len = 16;
        return 0;
    }
    int hl = static_cast<int>(*len);
    if (getsockopt(s->handle(), o->level, o->name, static_cast<char*>(value), &hl) != 0) return fail_wsa();
    *len = static_cast<uint32_t>(hl);
    if (level == 1 && name == 4 && hl >= 4) {  // SO_ERROR: a WSA value, made a Linux errno
        int* e = static_cast<int*>(value);
        if (*e) *e = linux_from_wsa(*e);
    }
    return 0;
}

// ---- names -----------------------------------------------------------------------------------
// Bionic's struct addrinfo has Windows' layout (see the top of the file), so
// the host list is copied into one the guest owns, families translated.
struct GuestAddrinfo {
    int flags, family, socktype, protocol;
    uint32_t addrlen;
    char* canonname;
    void* addr;
    GuestAddrinfo* next;
};
WINAPK_GUEST_ABI int g_getaddrinfo(const char* node, const char* service, const GuestAddrinfo* hints,
                                   GuestAddrinfo** res) {
    winsock_init();
    ADDRINFOA h{};
    if (hints) {
        h.ai_flags = hints->flags;
        h.ai_family = host_family(hints->family);
        h.ai_socktype = hints->socktype;
        h.ai_protocol = hints->protocol;
    }
    ADDRINFOA* list = nullptr;
    const int r = ::getaddrinfo(node, service, hints ? &h : nullptr, &list);
    if (r != 0) {
        switch (r) {
            case WSATRY_AGAIN: return 2;          // EAI_AGAIN
            case WSAEINVAL: return 3;             // EAI_BADFLAGS
            case WSANO_RECOVERY: return 4;        // EAI_FAIL
            case WSAEAFNOSUPPORT: return 5;       // EAI_FAMILY
            case WSA_NOT_ENOUGH_MEMORY: return 6; // EAI_MEMORY
            case WSANO_DATA: return 7;            // EAI_NODATA
            case WSATYPE_NOT_FOUND: return 9;     // EAI_SERVICE
            case WSAESOCKTNOSUPPORT: return 10;   // EAI_SOCKTYPE
            default: return 8;                    // EAI_NONAME
        }
    }
    GuestAddrinfo* head = nullptr;
    GuestAddrinfo** tail = &head;
    for (ADDRINFOA* a = list; a; a = a->ai_next) {
        auto* g = static_cast<GuestAddrinfo*>(std::calloc(1, sizeof(GuestAddrinfo) + a->ai_addrlen));
        g->flags = a->ai_flags;
        g->family = guest_family(a->ai_family);
        g->socktype = a->ai_socktype;
        g->protocol = a->ai_protocol;
        g->addrlen = static_cast<uint32_t>(a->ai_addrlen);
        g->addr = g + 1;
        std::memcpy(g->addr, a->ai_addr, a->ai_addrlen);
        static_cast<sockaddr*>(g->addr)->sa_family = static_cast<ADDRESS_FAMILY>(guest_family(a->ai_addr->sa_family));
        if (a->ai_canonname) g->canonname = _strdup(a->ai_canonname);
        *tail = g;
        tail = &g->next;
    }
    freeaddrinfo(list);
    *res = head;
    return 0;
}
WINAPK_GUEST_ABI void g_freeaddrinfo(GuestAddrinfo* a) {
    while (a) {
        GuestAddrinfo* next = a->next;
        std::free(a->canonname);
        std::free(a);
        a = next;
    }
}
WINAPK_GUEST_ABI const char* g_gai_strerror(int e) {
    static const char* const kText[] = {"Success",
                                        "Address family for hostname not supported",
                                        "Temporary failure in name resolution",
                                        "Invalid value for ai_flags",
                                        "Non-recoverable failure in name resolution",
                                        "ai_family not supported",
                                        "Memory allocation failure",
                                        "No address associated with hostname",
                                        "hostname nor servname provided, or not known",
                                        "servname not supported for ai_socktype",
                                        "ai_socktype not supported",
                                        "System error returned in errno"};
    return e >= 0 && e < 12 ? kText[e] : "Unknown error";
}
WINAPK_GUEST_ABI int g_getnameinfo(const void* addr, uint32_t len, char* host, uint32_t hostlen, char* serv,
                                   uint32_t servlen, int flags) {
    winsock_init();
    sockaddr_storage h;
    int hl;
    if (!to_host(addr, static_cast<int>(len), &h, &hl)) return 5;  // EAI_FAMILY
    const int r = ::getnameinfo(reinterpret_cast<sockaddr*>(&h), hl, host, hostlen, serv, servlen, flags);
    return r == 0 ? 0 : 8;  // EAI_NONAME
}
WINAPK_GUEST_ABI int g_gethostname(char* name, size_t len) {
    winsock_init();
    return ::gethostname(name, static_cast<int>(len)) == 0 ? 0 : fail_wsa();
}

WINAPK_GUEST_ABI int g_inet_pton(int family, const char* src, void* dst) {
    winsock_init();
    const int r = ::inet_pton(host_family(family), src, dst);
    if (r < 0) *guest_errno() = 97;  // EAFNOSUPPORT
    return r;
}
WINAPK_GUEST_ABI const char* g_inet_ntop(int family, const void* src, char* dst, uint32_t len) {
    winsock_init();
    const char* r = ::inet_ntop(host_family(family), src, dst, len);
    if (!r) *guest_errno() = 28;  // ENOSPC
    return r;
}
// FORTIFY's checked forms take the buffer size after the length.
WINAPK_GUEST_ABI int64_t g_recvfrom_chk(int fd, void* buf, size_t n, size_t bn, int flags, void* addr, uint32_t* len) {
    if (n > bn) {
        std::fprintf(stderr, "[win.apk] guest FORTIFY: recvfrom: prevented buffer overflow\n");
        std::abort();
    }
    return g_recvfrom(fd, buf, n, flags, addr, len);
}
WINAPK_GUEST_ABI int64_t g_sendto_chk(int fd, const void* buf, size_t n, size_t bn, int flags, const void* addr,
                                      uint32_t len) {
    if (n > bn) {
        std::fprintf(stderr, "[win.apk] guest FORTIFY: sendto: prevented buffer over-read\n");
        std::abort();
    }
    return g_sendto(fd, buf, n, flags, addr, len);
}

WINAPK_GUEST_ABI uint32_t g_inet_addr(const char* s) {
    in_addr a;
    return ::inet_pton(AF_INET, s, &a) == 1 ? a.s_addr : INADDR_NONE;
}
WINAPK_GUEST_ABI int g_inet_aton(const char* s, in_addr* out) { return ::inet_pton(AF_INET, s, out) == 1; }
WINAPK_GUEST_ABI char* g_inet_ntoa(in_addr a) {
    static thread_local char buf[16];
    return const_cast<char*>(::inet_ntop(AF_INET, &a, buf, sizeof(buf)));
}
WINAPK_GUEST_ABI uint32_t g_htonl(uint32_t v) { return _byteswap_ulong(v); }
WINAPK_GUEST_ABI uint16_t g_htons(uint16_t v) { return _byteswap_ushort(v); }
WINAPK_GUEST_ABI unsigned g_if_nametoindex(const char*) { return 0; }

const ShimSymbol kSymbols[] = {
    WINAPK_SHIM("socket", g_socket),
    WINAPK_SHIM("socketpair", g_socketpair),
    WINAPK_SHIM("bind", g_bind),
    WINAPK_SHIM("listen", g_listen),
    WINAPK_SHIM("accept", g_accept),
    WINAPK_SHIM("accept4", g_accept4),
    WINAPK_SHIM("connect", g_connect),
    WINAPK_SHIM("shutdown", g_shutdown),
    WINAPK_SHIM("send", g_send),
    WINAPK_SHIM("recv", g_recv),
    WINAPK_SHIM("sendto", g_sendto),
    WINAPK_SHIM("recvfrom", g_recvfrom),
    WINAPK_SHIM("__recvfrom_chk", g_recvfrom_chk),
    WINAPK_SHIM("__sendto_chk", g_sendto_chk),
    WINAPK_SHIM("sendmsg", g_sendmsg),
    WINAPK_SHIM("recvmsg", g_recvmsg),
    WINAPK_SHIM("getsockname", g_getsockname),
    WINAPK_SHIM("getpeername", g_getpeername),
    WINAPK_SHIM("setsockopt", g_setsockopt),
    WINAPK_SHIM("getsockopt", g_getsockopt),
    WINAPK_SHIM("getaddrinfo", g_getaddrinfo),
    WINAPK_SHIM("freeaddrinfo", g_freeaddrinfo),
    WINAPK_SHIM("gai_strerror", g_gai_strerror),
    WINAPK_SHIM("getnameinfo", g_getnameinfo),
    WINAPK_SHIM("gethostname", g_gethostname),
    WINAPK_SHIM("inet_pton", g_inet_pton),
    WINAPK_SHIM("inet_ntop", g_inet_ntop),
    WINAPK_SHIM("inet_addr", g_inet_addr),
    WINAPK_SHIM("inet_aton", g_inet_aton),
    WINAPK_SHIM("inet_ntoa", g_inet_ntoa),
    WINAPK_SHIM("htonl", g_htonl),
    WINAPK_SHIM("ntohl", g_htonl),
    WINAPK_SHIM("htons", g_htons),
    WINAPK_SHIM("ntohs", g_htons),
    WINAPK_SHIM("if_nametoindex", g_if_nametoindex),
};

}  // namespace

ShimTable socket_symbols() { return WINAPK_SHIM_TABLE(kSymbols); }

}  // namespace winapk::bionic
