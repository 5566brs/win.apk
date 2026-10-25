// misc.cpp -- time, memory mapping, system properties, the environment,
// process identity, limits, randomness and libm, for guest code.
//
// System properties and the environment are the guest's own stores, not the
// host's: the Java framework pushes its properties (SystemProperties.
// macapkPushToNative -> NativeCocoa.setSystemProperty) and Os.setenv lands in
// the same environment native code reads, so Java and native code agree, as
// upstream's shim arranges on the Mac.

#include "bionic_internal.h"

#include "winapk/mini_bionic.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <io.h>
#include <windows.h>
#include <bcrypt.h>
#include <getopt.h>

// BSD's optreset, which <getopt.h> declares only under _BSD_SOURCE.
extern "C" int __mingw_optreset;

namespace winapk::bionic {

namespace {

// ---- time ---------------------------------------------------------------------------
struct GuestTimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
struct GuestTimeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

int64_t realtime_ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t t = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) * 100;
}
int64_t monotonic_ns() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(c.QuadPart / freq * 1000000000LL + (c.QuadPart % freq) * 1000000000LL / freq);
}
int64_t thread_cpu_ns(HANDLE thread, bool process) {
    FILETIME created, exited, kernel, user;
    const BOOL ok = process ? GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)
                            : GetThreadTimes(thread, &created, &exited, &kernel, &user);
    if (!ok) return 0;
    auto v = [](const FILETIME& f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (v(kernel) + v(user)) * 100;
}

// Linux clock ids: REALTIME 0, MONOTONIC 1, PROCESS_CPUTIME 2, THREAD_CPUTIME 3,
// MONOTONIC_RAW 4, REALTIME_COARSE 5, MONOTONIC_COARSE 6, BOOTTIME 7.
WINAPK_GUEST_ABI int g_clock_gettime(int clock, GuestTimespec* ts) {
    int64_t ns;
    switch (clock) {
        case 0: case 5: ns = realtime_ns(); break;
        case 1: case 4: case 6: case 7: ns = monotonic_ns(); break;
        case 2: ns = thread_cpu_ns(nullptr, true); break;
        case 3: ns = thread_cpu_ns(GetCurrentThread(), false); break;
        default: *guest_errno() = kEINVAL; return -1;
    }
    ts->tv_sec = ns / 1000000000LL;
    ts->tv_nsec = ns % 1000000000LL;
    return 0;
}
WINAPK_GUEST_ABI int g_clock_getres(int, GuestTimespec* ts) {
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = 100;
    }
    return 0;
}
WINAPK_GUEST_ABI int g_gettimeofday(GuestTimeval* tv, void*) {
    const int64_t ns = realtime_ns();
    if (tv) {
        tv->tv_sec = ns / 1000000000LL;
        tv->tv_usec = (ns % 1000000000LL) / 1000;
    }
    return 0;
}
WINAPK_GUEST_ABI int64_t g_time(int64_t* t) {
    const int64_t s = realtime_ns() / 1000000000LL;
    if (t) *t = s;
    return s;
}
WINAPK_GUEST_ABI glong g_clock() { return thread_cpu_ns(nullptr, true) / 1000; }  // CLOCKS_PER_SEC 1000000

void sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    // A high-resolution waitable timer where Windows has one; Sleep otherwise.
    static const bool high_res = [] {
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (t) CloseHandle(t);
        return t != nullptr;
    }();
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, high_res ? 0x00000002 : 0, TIMER_ALL_ACCESS);
    if (!timer) {
        Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100 > 0 ? ns / 100 : 1);
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
}
WINAPK_GUEST_ABI int g_nanosleep(const GuestTimespec* req, GuestTimespec* rem) {
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL) {
        *guest_errno() = kEINVAL;
        return -1;
    }
    sleep_ns(req->tv_sec * 1000000000LL + req->tv_nsec);
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}
WINAPK_GUEST_ABI int g_clock_nanosleep(int clock, int flags, const GuestTimespec* req, GuestTimespec* rem) {
    if (flags & 1) {  // TIMER_ABSTIME
        GuestTimespec now;
        g_clock_gettime(clock, &now);
        sleep_ns((req->tv_sec - now.tv_sec) * 1000000000LL + (req->tv_nsec - now.tv_nsec));
        return 0;
    }
    return g_nanosleep(req, rem) == 0 ? 0 : *guest_errno();
}
WINAPK_GUEST_ABI int g_usleep(uint32_t us) {
    sleep_ns(static_cast<int64_t>(us) * 1000);
    return 0;
}
WINAPK_GUEST_ABI unsigned g_sleep(unsigned s) {
    sleep_ns(static_cast<int64_t>(s) * 1000000000LL);
    return 0;
}

// struct tm is the same 9 ints on both sides, then Bionic adds tm_gmtoff (long)
// and tm_zone (const char*).
struct GuestTm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    int64_t tm_gmtoff;
    const char* tm_zone;
};
void to_guest_tm(const std::tm& t, GuestTm* g, int64_t gmtoff, const char* zone) {
    g->tm_sec = t.tm_sec;
    g->tm_min = t.tm_min;
    g->tm_hour = t.tm_hour;
    g->tm_mday = t.tm_mday;
    g->tm_mon = t.tm_mon;
    g->tm_year = t.tm_year;
    g->tm_wday = t.tm_wday;
    g->tm_yday = t.tm_yday;
    g->tm_isdst = t.tm_isdst;
    g->tm_gmtoff = gmtoff;
    g->tm_zone = zone;
}
std::tm from_guest_tm(const GuestTm* g) {
    std::tm t{};
    t.tm_sec = g->tm_sec;
    t.tm_min = g->tm_min;
    t.tm_hour = g->tm_hour;
    t.tm_mday = g->tm_mday;
    t.tm_mon = g->tm_mon;
    t.tm_year = g->tm_year;
    t.tm_wday = g->tm_wday;
    t.tm_yday = g->tm_yday;
    t.tm_isdst = g->tm_isdst;
    return t;
}
WINAPK_GUEST_ABI GuestTm* g_gmtime_r(const int64_t* t, GuestTm* out) {
    std::tm tm;
    const __time64_t v = *t;
    if (_gmtime64_s(&tm, &v)) return nullptr;
    to_guest_tm(tm, out, 0, "UTC");
    return out;
}
WINAPK_GUEST_ABI GuestTm* g_localtime_r(const int64_t* t, GuestTm* out) {
    std::tm tm;
    const __time64_t v = *t;
    if (_localtime64_s(&tm, &v)) return nullptr;
    // The UTC offset in effect: the local broken-down time read back as UTC.
    std::tm copy = tm;
    const int64_t gmtoff = _mkgmtime64(&copy) - v;
    to_guest_tm(tm, out, gmtoff, tm.tm_isdst > 0 ? "DST" : "STD");
    return out;
}
WINAPK_GUEST_ABI GuestTm* g_gmtime(const int64_t* t) {
    static thread_local GuestTm tm;
    return g_gmtime_r(t, &tm);
}
WINAPK_GUEST_ABI GuestTm* g_localtime(const int64_t* t) {
    static thread_local GuestTm tm;
    return g_localtime_r(t, &tm);
}
WINAPK_GUEST_ABI int64_t g_mktime(GuestTm* g) {
    std::tm t = from_guest_tm(g);
    const int64_t r = _mktime64(&t);
    if (r != -1) to_guest_tm(t, g, g->tm_gmtoff, g->tm_zone);
    return r;
}
WINAPK_GUEST_ABI int64_t g_timegm(GuestTm* g) {
    std::tm t = from_guest_tm(g);
    return _mkgmtime64(&t);
}
WINAPK_GUEST_ABI size_t g_strftime(char* s, size_t n, const char* fmt, const GuestTm* g) {
    const std::tm t = from_guest_tm(g);
    return std::strftime(s, n, fmt, &t);
}
// tzset fills Bionic's timezone (seconds west of UTC, a 64-bit long), daylight
// and tzname from the host's zone. Guest code may read them without calling
// tzset, so they are filled once at startup too.
glong g_timezone = 0;
int g_daylight = 0;
char g_tzname_std[64] = "GMT", g_tzname_dst[64] = "GMT";
char* g_tzname[2] = {g_tzname_std, g_tzname_dst};
WINAPK_GUEST_ABI void g_tzset() {
    _tzset();
    // From Win32 rather than the CRT's _get_timezone family, which not every
    // CRT this builds against exports. Bias is in minutes, UTC = local + Bias.
    TIME_ZONE_INFORMATION tzi;
    if (GetTimeZoneInformation(&tzi) == TIME_ZONE_ID_INVALID) return;
    g_timezone = static_cast<glong>(tzi.Bias) * 60;
    g_daylight = tzi.DaylightDate.wMonth != 0;
    WideCharToMultiByte(CP_UTF8, 0, tzi.StandardName, -1, g_tzname_std, sizeof(g_tzname_std), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, tzi.DaylightName, -1, g_tzname_dst, sizeof(g_tzname_dst), nullptr, nullptr);
}
const bool g_tz_ready = [] {
    g_tzset();
    return true;
}();
// <netinet/in.h>'s constants: :: and ::1.
unsigned char g_in6addr_any[16] = {};
unsigned char g_in6addr_loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

// ---- memory mapping -----------------------------------------------------------------
// Linux values: PROT_READ 1, WRITE 2, EXEC 4; MAP_SHARED 1, PRIVATE 2, FIXED
// 0x10, ANONYMOUS 0x20.
DWORD win_protect(int prot) {
    const bool r = prot & 1, w = prot & 2, x = prot & 4;
    if (x) return w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

std::mutex g_maps_lock;
std::map<uintptr_t, size_t> g_maps;  // reservations made here, by base

WINAPK_GUEST_ABI void* g_mmap(void* addr, size_t len, int prot, int flags, int fd, int64_t off) {
    void* const kFailed = reinterpret_cast<void*>(-1);
    if (len == 0) {
        *guest_errno() = kEINVAL;
        return kFailed;
    }
    void* want = (flags & 0x10) ? addr : nullptr;
    const bool anonymous = (flags & 0x20) || fd < 0;
    if (prot == 0 && anonymous) {
        // An address-space reservation (allocators reserve gigabytes this way
        // and commit pieces with mprotect). Only reserving keeps it off the
        // commit charge, as Linux's overcommit does.
        void* r = VirtualAlloc(want, len, MEM_RESERVE, PAGE_NOACCESS);
        if (!r && want) r = VirtualAlloc(nullptr, len, MEM_RESERVE, PAGE_NOACCESS);
        if (!r) {
            *guest_errno() = kENOMEM;
            return kFailed;
        }
        std::lock_guard<std::mutex> hold(g_maps_lock);
        g_maps[reinterpret_cast<uintptr_t>(r)] = len;
        return r;
    }
    // The pages are committed read-write first so a file's contents can be
    // copied in; the requested protection follows.
    void* p = VirtualAlloc(want, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p && want) p = VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        *guest_errno() = kENOMEM;
        return kFailed;
    }
    if (!(flags & 0x20) && fd >= 0) {
        // A file mapping: a private copy of the file's bytes. Writes are not
        // carried back to the file (MAP_SHARED writes need a real view).
        const int64_t at = _lseeki64(fd, 0, SEEK_CUR);
        if (_lseeki64(fd, off, SEEK_SET) >= 0) {
            size_t done = 0;
            while (done < len) {
                const int r = _read(fd, static_cast<char*>(p) + done,
                                    static_cast<unsigned>((len - done) > 0x40000000 ? 0x40000000 : len - done));
                if (r <= 0) break;
                done += static_cast<size_t>(r);
            }
        }
        if (at >= 0) _lseeki64(fd, at, SEEK_SET);
    }
    DWORD old;
    VirtualProtect(p, len, win_protect(prot), &old);
    std::lock_guard<std::mutex> hold(g_maps_lock);
    g_maps[reinterpret_cast<uintptr_t>(p)] = len;
    return p;
}
WINAPK_GUEST_ABI int g_munmap(void* addr, size_t len) {
    std::lock_guard<std::mutex> hold(g_maps_lock);
    auto it = g_maps.find(reinterpret_cast<uintptr_t>(addr));
    if (it != g_maps.end() && len >= it->second) {
        VirtualFree(addr, 0, MEM_RELEASE);
        g_maps.erase(it);
        return 0;
    }
    // Part of a mapping: decommit those pages, keep the reservation.
    VirtualFree(addr, len, MEM_DECOMMIT);
    return 0;
}
WINAPK_GUEST_ABI int g_mprotect(void* addr, size_t len, int prot) {
    DWORD old;
    // Opening up part of a reservation (see g_mmap) commits it; committing
    // pages that already are is allowed and only sets the protection.
    if (prot != 0 && VirtualAlloc(addr, len, MEM_COMMIT, win_protect(prot))) return 0;
    if (VirtualProtect(addr, len, win_protect(prot), &old)) return 0;
    set_errno_from_win32(GetLastError());
    return -1;
}
WINAPK_GUEST_ABI int g_madvise(void* addr, size_t len, int advice) {
    if (advice == 4) {  // MADV_DONTNEED: the pages read back as zeros
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
            std::memset(addr, 0, len);
        }
    }
    return 0;
}
WINAPK_GUEST_ABI int g_msync(void*, size_t, int) { return 0; }
WINAPK_GUEST_ABI int g_mlock(const void*, size_t) { return 0; }
WINAPK_GUEST_ABI int g_munlock(const void*, size_t) { return 0; }

// ---- system properties ---------------------------------------------------------------
std::mutex g_props_lock;
std::map<std::string, std::string>& props() {
    // What a device has before the framework pushes its own values.
    static std::map<std::string, std::string> m{
        {"ro.build.version.sdk", "29"},
        {"ro.build.version.release", "10"},
        {"ro.product.cpu.abi", "x86_64"},
        {"ro.product.cpu.abilist", "x86_64"},
        {"ro.product.cpu.abilist64", "x86_64"},
        {"ro.product.cpu.abilist32", ""},
        {"ro.kernel.qemu", "0"},
        {"ro.debuggable", "0"},
        {"ro.secure", "1"},
    };
    return m;
}

constexpr size_t kPropValueMax = 92;  // PROP_VALUE_MAX

// __system_property_get: the value's length, 0 (and "") when unset.
WINAPK_GUEST_ABI int g_property_get(const char* name, char* value) {
    std::lock_guard<std::mutex> hold(g_props_lock);
    auto it = props().find(name ? name : "");
    if (it == props().end()) {
        value[0] = '\0';
        return 0;
    }
    const size_t n = it->second.size() < kPropValueMax - 1 ? it->second.size() : kPropValueMax - 1;
    std::memcpy(value, it->second.data(), n);
    value[n] = '\0';
    return static_cast<int>(n);
}
// __system_property_find hands out an opaque prop_info: here the map entry.
WINAPK_GUEST_ABI const void* g_property_find(const char* name) {
    std::lock_guard<std::mutex> hold(g_props_lock);
    auto it = props().find(name ? name : "");
    return it == props().end() ? nullptr : &*it;
}
WINAPK_GUEST_ABI int g_property_read(const void* pi, char* name, char* value) {
    const auto* e = static_cast<const std::pair<const std::string, std::string>*>(pi);
    if (name) {
        std::strncpy(name, e->first.c_str(), 31);
        name[31] = '\0';
    }
    std::strncpy(value, e->second.c_str(), kPropValueMax - 1);
    value[kPropValueMax - 1] = '\0';
    return static_cast<int>(std::strlen(value));
}
using PropCallback = void(WINAPK_GUEST_ABI*)(void*, const char*, const char*, uint32_t);
WINAPK_GUEST_ABI void g_property_read_callback(const void* pi, PropCallback cb, void* cookie) {
    const auto* e = static_cast<const std::pair<const std::string, std::string>*>(pi);
    cb(cookie, e->first.c_str(), e->second.c_str(), 0);
}
WINAPK_GUEST_ABI int g_property_set(const char* name, const char* value) {
    property_set(name, value);
    return 0;
}
WINAPK_GUEST_ABI int g_get_device_api_level() {
    char v[kPropValueMax];
    return g_property_get("ro.build.version.sdk", v) ? std::atoi(v) : -1;
}

// ---- environment -------------------------------------------------------------------------
std::mutex g_env_lock;
std::map<std::string, std::string>& env() {
    // A zygote-forked app's environment (upstream's bionic_shim.cpp seeds the
    // full device list; these are the variables native code reads).
    static std::map<std::string, std::string> m{
        {"PATH", "/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:/system_ext/bin:"
                 "/system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin"},
        {"ANDROID_ROOT", "/system"},
        {"ANDROID_DATA", "/data"},
        {"ANDROID_STORAGE", "/storage"},
        {"EXTERNAL_STORAGE", "/sdcard"},
        {"ANDROID_ART_ROOT", "/apex/com.android.art"},
        {"ANDROID_I18N_ROOT", "/apex/com.android.i18n"},
        {"ANDROID_TZDATA_ROOT", "/apex/com.android.tzdata"},
    };
    return m;
}
// getenv's results must stay valid; values are interned and never freed.
const char* intern(const std::string& s) {
    static std::mutex lock;
    static std::map<std::string, const char*> pool;
    std::lock_guard<std::mutex> hold(lock);
    auto it = pool.find(s);
    if (it != pool.end()) return it->second;
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(p, s.c_str(), s.size() + 1);
    pool.emplace(s, p);
    return p;
}
// The guest's `environ`: a data symbol, so guest code reads the array behind
// it directly (SDL walks it). Rebuilt on every change, under g_env_lock; an
// old array is leaked, since another thread may still be walking it.
char** g_environ = nullptr;
void rebuild_environ() {
    auto& e = env();
    char** a = static_cast<char**>(std::calloc(e.size() + 1, sizeof(char*)));
    size_t i = 0;
    for (const auto& [k, v] : e) a[i++] = const_cast<char*>(intern(k + "=" + v));
    g_environ = a;
}
const bool g_environ_ready = [] {
    std::lock_guard<std::mutex> hold(g_env_lock);
    rebuild_environ();
    return true;
}();

WINAPK_GUEST_ABI char* g_getenv(const char* name) {
    const char* v = winapk::bionic_getenv(name);
    return const_cast<char*>(v);
}
WINAPK_GUEST_ABI int g_setenv(const char* name, const char* value, int overwrite) {
    const int r = winapk::bionic_setenv(name, value, overwrite);
    if (r < 0) {
        *guest_errno() = -r;
        return -1;
    }
    return 0;
}
WINAPK_GUEST_ABI int g_unsetenv(const char* name) {
    const int r = winapk::bionic_unsetenv(name);
    if (r < 0) {
        *guest_errno() = -r;
        return -1;
    }
    return 0;
}

// ---- process identity and limits ------------------------------------------------------------
WINAPK_GUEST_ABI int g_getpid() { return static_cast<int>(GetCurrentProcessId()); }
WINAPK_GUEST_ABI int g_getppid() { return 1; }
// An app's uid: AID_APP_START (10000) and up; the same for every id here.
WINAPK_GUEST_ABI uint32_t g_getuid() { return 10100; }
WINAPK_GUEST_ABI int g_kill(int pid, int sig) {
    if (pid == g_getpid() && (sig == 9 || sig == 15)) TerminateProcess(GetCurrentProcess(), 128 + sig);
    return 0;
}

struct GuestRlimit {
    uint64_t cur, max;
};
// RLIMIT_STACK 3, RLIMIT_NOFILE 7; anything else is unlimited.
WINAPK_GUEST_ABI int g_getrlimit(int resource, GuestRlimit* r) {
    const uint64_t kInfinity = ~uint64_t{0};
    r->cur = r->max = kInfinity;
    if (resource == 3) r->cur = r->max = 8 * 1024 * 1024;
    if (resource == 7) r->cur = r->max = 8192;
    return 0;
}
WINAPK_GUEST_ABI int g_setrlimit(int, const GuestRlimit*) { return 0; }
WINAPK_GUEST_ABI int g_prlimit(int, int resource, const GuestRlimit*, GuestRlimit* old) {
    if (old) g_getrlimit(resource, old);
    return 0;
}

// sysconf: the values Bionic answers on an x86_64 device.
WINAPK_GUEST_ABI glong g_sysconf(int name) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (name) {
        case 0x27: return 4096;                                // _SC_PAGESIZE
        case 0x60: case 0x61: return si.dwNumberOfProcessors;  // _SC_NPROCESSORS_CONF/ONLN
        case 0x6: return 100;                                  // _SC_CLK_TCK
        case 0x5: return 8192;                                 // _SC_OPEN_MAX
        case 0x62: case 0x63: {                                // _SC_PHYS_PAGES, _SC_AVPHYS_PAGES
            MEMORYSTATUSEX ms{};
            ms.dwLength = sizeof(ms);
            GlobalMemoryStatusEx(&ms);
            return static_cast<glong>((name == 0x62 ? ms.ullTotalPhys : ms.ullAvailPhys) / 4096);
        }
        default: *guest_errno() = kEINVAL; return -1;
    }
}
WINAPK_GUEST_ABI int g_getpagesize() { return 4096; }

// uname: what an x86_64 Android device reports (upstream reports the same
// release, so Java's os.version agrees).
struct GuestUtsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
WINAPK_GUEST_ABI int g_uname(GuestUtsname* u) {
    std::memset(u, 0, sizeof(*u));
    std::strcpy(u->sysname, "Linux");
    std::strcpy(u->nodename, "localhost");
    std::strcpy(u->release, "5.10.0");
    std::strcpy(u->version, "#1 SMP PREEMPT");
    std::strcpy(u->machine, "x86_64");
    std::strcpy(u->domainname, "(none)");
    return 0;
}
// getauxval: AT_PAGESZ 6, AT_HWCAP 16, AT_HWCAP2 26; the rest is 0.
WINAPK_GUEST_ABI uint64_t g_getauxval(uint64_t type) { return type == 6 ? 4096 : 0; }

// ---- randomness ---------------------------------------------------------------------------
WINAPK_GUEST_ABI void g_arc4random_buf(void* buf, size_t n) {
    BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}
WINAPK_GUEST_ABI uint32_t g_arc4random() {
    uint32_t v;
    g_arc4random_buf(&v, sizeof(v));
    return v;
}
WINAPK_GUEST_ABI uint32_t g_arc4random_uniform(uint32_t bound) {
    if (bound < 2) return 0;
    const uint32_t floor = (0u - bound) % bound;  // reject the biased low range
    for (;;) {
        const uint32_t v = g_arc4random();
        if (v >= floor) return v % bound;
    }
}
WINAPK_GUEST_ABI int64_t g_getrandom(void* buf, size_t n, unsigned) {
    g_arc4random_buf(buf, n);
    return static_cast<int64_t>(n);
}
// ---- syscall(2) ------------------------------------------------------------------------
// Code that bypasses libc (Chromium's and PartitionAlloc's locks, Rust and Go
// runtimes, abseil) calls syscall() with x86_64 Linux numbers. The common ones
// map onto the shim; the rest fail with ENOSYS, which callers expect from an
// older kernel.
// futex(2): FUTEX_WAIT and WAIT_BITSET sleep while *uaddr == val (WaitOnAddress
// wakes on any write to the address, the caller re-checks, as with a spurious
// futex wakeup); WAKE and WAKE_BITSET wake waiters.
int64_t guest_futex(uint32_t* uaddr, int op, uint32_t val, const GuestTimespec* ts) {
    const int cmd = op & 0x7f;  // drop FUTEX_PRIVATE_FLAG and FUTEX_CLOCK_REALTIME
    if (cmd == 0 || cmd == 9) {  // FUTEX_WAIT, FUTEX_WAIT_BITSET
        if (*reinterpret_cast<volatile uint32_t*>(uaddr) != val) {
            *guest_errno() = kEAGAIN;
            return -1;
        }
        DWORD ms = INFINITE;
        if (ts) {
            int64_t ns = ts->tv_sec * 1000000000LL + ts->tv_nsec;
            if (cmd == 9) {  // an absolute deadline on CLOCK_MONOTONIC (or REALTIME with the flag)
                GuestTimespec now;
                g_clock_gettime((op & 256) ? 0 : 1, &now);
                ns -= now.tv_sec * 1000000000LL + now.tv_nsec;
            }
            ms = ns <= 0 ? 0 : static_cast<DWORD>((ns + 999999) / 1000000);
        }
        if (!WaitOnAddress(uaddr, &val, sizeof(val), ms) && GetLastError() == ERROR_TIMEOUT) {
            *guest_errno() = kETIMEDOUT;
            return -1;
        }
        return 0;
    }
    if (cmd == 1 || cmd == 10) {  // FUTEX_WAKE, FUTEX_WAKE_BITSET
        if (val == 1) WakeByAddressSingle(uaddr);
        else WakeByAddressAll(uaddr);
        return 0;  // the number woken is unknown; callers treat it as a hint
    }
    *guest_errno() = kENOSYS;
    return -1;
}
WINAPK_GUEST_ABI int64_t g_syscall(int64_t nr, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) {
    switch (nr) {
        case 202: return guest_futex(reinterpret_cast<uint32_t*>(a), static_cast<int>(b), static_cast<uint32_t>(c),
                                     reinterpret_cast<const GuestTimespec*>(d));
        case 186: return static_cast<int64_t>(GetCurrentThreadId());  // gettid
        case 39: return g_getpid();
        case 24: SwitchToThread(); return 0;  // sched_yield
        case 318: return g_getrandom(reinterpret_cast<void*>(a), static_cast<size_t>(b), 0);
        case 228: return g_clock_gettime(static_cast<int>(a), reinterpret_cast<GuestTimespec*>(b));
        case 28: return g_madvise(reinterpret_cast<void*>(a), static_cast<size_t>(b), static_cast<int>(c));
        case 324:  // membarrier: MEMBARRIER_CMD_QUERY answers "no commands", so callers use fences
            if (a == 0) return 0;
            *guest_errno() = kENOSYS;
            return -1;
        default:
            (void)e;
            *guest_errno() = kENOSYS;
            return -1;
    }
}

// rand/random: Bionic's RAND_MAX is 0x7fffffff, the UCRT's 0x7fff.
uint64_t g_rand_state = 1;
WINAPK_GUEST_ABI void g_srand(unsigned seed) { g_rand_state = seed; }
WINAPK_GUEST_ABI int g_rand() {
    g_rand_state = g_rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<int>((g_rand_state >> 33) & 0x7fffffff);
}

// The rand48 family: POSIX's 48-bit linear congruential generator,
// X' = (a * X + c) mod 2^48 with a = 0x5DEECE66D and c = 0xB unless lcong48
// changed them. X, a and c are kept as 16-bit words, low word first, the way
// Bionic's (NetBSD's) rand48 keeps them, and start from the same seed.
// The e/n/j forms step the caller's X with the shared a and c.
std::mutex g_rand48_lock;
uint16_t g_rand48_seed[3] = {0x330e, 0xabcd, 0x1234};
uint16_t g_rand48_mult[3] = {0xe66d, 0xdeec, 0x0005};
uint16_t g_rand48_add = 0x000b;

uint64_t rand48_value(const uint16_t x[3]) { return x[0] | uint64_t{x[1]} << 16 | uint64_t{x[2]} << 32; }

// One step of `x`, returning the new 48-bit X.
uint64_t rand48_step(uint16_t x[3]) {
    std::lock_guard<std::mutex> hold(g_rand48_lock);
    const uint64_t next = (rand48_value(g_rand48_mult) * rand48_value(x) + g_rand48_add) & ((uint64_t{1} << 48) - 1);
    x[0] = static_cast<uint16_t>(next);
    x[1] = static_cast<uint16_t>(next >> 16);
    x[2] = static_cast<uint16_t>(next >> 32);
    return next;
}

void rand48_reset_params() {
    g_rand48_mult[0] = 0xe66d;
    g_rand48_mult[1] = 0xdeec;
    g_rand48_mult[2] = 0x0005;
    g_rand48_add = 0x000b;
}

// [0, 1): X / 2^48.
double rand48_double(uint64_t x) { return std::ldexp(static_cast<double>(x), -48); }
// [0, 2^31): the top 31 bits.
glong rand48_nonnegative(uint64_t x) { return static_cast<glong>(x >> 17); }
// [-2^31, 2^31): the top 32 bits, signed.
glong rand48_signed(uint64_t x) { return static_cast<int32_t>(static_cast<uint32_t>(x >> 16)); }

WINAPK_GUEST_ABI double g_drand48() { return rand48_double(rand48_step(g_rand48_seed)); }
WINAPK_GUEST_ABI double g_erand48(uint16_t x[3]) { return rand48_double(rand48_step(x)); }
WINAPK_GUEST_ABI glong g_lrand48() { return rand48_nonnegative(rand48_step(g_rand48_seed)); }
WINAPK_GUEST_ABI glong g_nrand48(uint16_t x[3]) { return rand48_nonnegative(rand48_step(x)); }
WINAPK_GUEST_ABI glong g_mrand48() { return rand48_signed(rand48_step(g_rand48_seed)); }
WINAPK_GUEST_ABI glong g_jrand48(uint16_t x[3]) { return rand48_signed(rand48_step(x)); }

WINAPK_GUEST_ABI void g_srand48(glong seed) {
    std::lock_guard<std::mutex> hold(g_rand48_lock);
    g_rand48_seed[0] = 0x330e;
    g_rand48_seed[1] = static_cast<uint16_t>(seed);
    g_rand48_seed[2] = static_cast<uint16_t>(seed >> 16);
    rand48_reset_params();
}

WINAPK_GUEST_ABI uint16_t* g_seed48(const uint16_t x[3]) {
    static uint16_t previous[3];
    std::lock_guard<std::mutex> hold(g_rand48_lock);
    std::memcpy(previous, g_rand48_seed, sizeof(previous));
    std::memcpy(g_rand48_seed, x, sizeof(g_rand48_seed));
    rand48_reset_params();
    return previous;
}

WINAPK_GUEST_ABI void g_lcong48(const uint16_t p[7]) {
    std::lock_guard<std::mutex> hold(g_rand48_lock);
    std::memcpy(g_rand48_seed, p, sizeof(g_rand48_seed));
    std::memcpy(g_rand48_mult, p + 3, sizeof(g_rand48_mult));
    g_rand48_add = p[6];
}

// ---- getopt ------------------------------------------------------------------------------
// MinGW's getopt, getopt_long and getopt_long_only are OpenBSD's
// getopt_long.c, which is Bionic's too, and struct option has one layout on
// both sides (name 0, has_arg 8, flag 16, val 24). So the guest's getopt is
// the CRT's, and the guest's optarg, optind, opterr, optopt and optreset are
// the CRT's own variables: a guest that sets optind back to 1 restarts the
// parser it calls.
WINAPK_GUEST_ABI int g_getopt(int argc, char* const* argv, const char* opts) { return ::getopt(argc, argv, opts); }
WINAPK_GUEST_ABI int g_getopt_long(int argc, char* const* argv, const char* opts, const struct option* longopts,
                                   int* index) {
    return ::getopt_long(argc, argv, opts, longopts, index);
}
WINAPK_GUEST_ABI int g_getopt_long_only(int argc, char* const* argv, const char* opts,
                                        const struct option* longopts, int* index) {
    return ::getopt_long_only(argc, argv, opts, longopts, index);
}

// ---- process exit -------------------------------------------------------------------------
[[noreturn]] WINAPK_GUEST_ABI void g_exit(int code) {
    mini_bionic_run_atexit();
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    std::abort();
}
[[noreturn]] WINAPK_GUEST_ABI void g__exit(int code) {
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    std::abort();
}
WINAPK_GUEST_ABI int g_atexit(void (*)()) { return 0; }  // never run: an app's process never exits normally

// ---- libm -----------------------------------------------------------------------------------
#define WINAPK_M1(name) WINAPK_GUEST_ABI double m_##name(double x) { return std::name(x); } \
                        WINAPK_GUEST_ABI float m_##name##f(float x) { return std::name(x); }
#define WINAPK_M2(name) WINAPK_GUEST_ABI double m_##name(double x, double y) { return std::name(x, y); } \
                        WINAPK_GUEST_ABI float m_##name##f(float x, float y) { return std::name(x, y); }
WINAPK_M1(sqrt) WINAPK_M1(cbrt) WINAPK_M1(sin) WINAPK_M1(cos) WINAPK_M1(tan) WINAPK_M1(asin) WINAPK_M1(acos)
WINAPK_M1(atan) WINAPK_M1(sinh) WINAPK_M1(cosh) WINAPK_M1(tanh) WINAPK_M1(asinh) WINAPK_M1(acosh) WINAPK_M1(atanh)
WINAPK_M1(exp) WINAPK_M1(exp2) WINAPK_M1(expm1) WINAPK_M1(log) WINAPK_M1(log2) WINAPK_M1(log10) WINAPK_M1(log1p)
WINAPK_M1(fabs) WINAPK_M1(floor) WINAPK_M1(ceil) WINAPK_M1(round) WINAPK_M1(trunc) WINAPK_M1(rint)
WINAPK_M1(nearbyint) WINAPK_M1(erf) WINAPK_M1(erfc) WINAPK_M1(tgamma) WINAPK_M1(lgamma) WINAPK_M1(logb)
WINAPK_M2(pow) WINAPK_M2(atan2) WINAPK_M2(fmod) WINAPK_M2(hypot) WINAPK_M2(fmin) WINAPK_M2(fmax) WINAPK_M2(copysign)
WINAPK_M2(remainder) WINAPK_M2(fdim) WINAPK_M2(nextafter)
#undef WINAPK_M1
#undef WINAPK_M2
WINAPK_GUEST_ABI double m_ldexp(double x, int e) { return std::ldexp(x, e); }
WINAPK_GUEST_ABI float m_ldexpf(float x, int e) { return std::ldexp(x, e); }
WINAPK_GUEST_ABI double m_scalbn(double x, int e) { return std::scalbn(x, e); }
WINAPK_GUEST_ABI float m_scalbnf(float x, int e) { return std::scalbn(x, e); }
WINAPK_GUEST_ABI double m_frexp(double x, int* e) { return std::frexp(x, e); }
WINAPK_GUEST_ABI float m_frexpf(float x, int* e) { return std::frexp(x, e); }
WINAPK_GUEST_ABI double m_modf(double x, double* i) { return std::modf(x, i); }
WINAPK_GUEST_ABI float m_modff(float x, float* i) { return std::modf(x, i); }
WINAPK_GUEST_ABI double m_fma(double x, double y, double z) { return std::fma(x, y, z); }
WINAPK_GUEST_ABI float m_fmaf(float x, float y, float z) { return std::fma(x, y, z); }
WINAPK_GUEST_ABI glong m_lround(double x) { return std::llround(x); }
WINAPK_GUEST_ABI glong m_lroundf(float x) { return std::llround(x); }
WINAPK_GUEST_ABI glong m_lrint(double x) { return std::llrint(x); }
WINAPK_GUEST_ABI glong m_lrintf(float x) { return std::llrint(x); }
WINAPK_GUEST_ABI long long m_llround(double x) { return std::llround(x); }
WINAPK_GUEST_ABI long long m_llrint(double x) { return std::llrint(x); }
WINAPK_GUEST_ABI int m_ilogb(double x) { return std::ilogb(x); }
WINAPK_GUEST_ABI void m_sincos(double x, double* s, double* c) {
    *s = std::sin(x);
    *c = std::cos(x);
}
WINAPK_GUEST_ABI void m_sincosf(float x, float* s, float* c) {
    *s = std::sin(x);
    *c = std::cos(x);
}
WINAPK_GUEST_ABI double m_nan(const char*) { return std::nan(""); }
WINAPK_GUEST_ABI float m_nanf(const char*) { return std::nanf(""); }
// Bionic's isinf/isnan/finite exist as functions, too.
WINAPK_GUEST_ABI int m_isinf(double x) { return std::isinf(x); }
WINAPK_GUEST_ABI int m_isnan(double x) { return std::isnan(x); }
WINAPK_GUEST_ABI int m_finite(double x) { return std::isfinite(x); }
WINAPK_GUEST_ABI int m_isinff(float x) { return std::isinf(x); }
WINAPK_GUEST_ABI int m_isnanf(float x) { return std::isnan(x); }

#define M(name) WINAPK_SHIM(#name, m_##name), WINAPK_SHIM(#name "f", m_##name##f)

const ShimSymbol kSymbols[] = {
    WINAPK_SHIM("clock_gettime", g_clock_gettime),
    WINAPK_SHIM("clock_getres", g_clock_getres),
    WINAPK_SHIM("gettimeofday", g_gettimeofday),
    WINAPK_SHIM("time", g_time),
    WINAPK_SHIM("clock", g_clock),
    WINAPK_SHIM("nanosleep", g_nanosleep),
    WINAPK_SHIM("clock_nanosleep", g_clock_nanosleep),
    WINAPK_SHIM("usleep", g_usleep),
    WINAPK_SHIM("sleep", g_sleep),
    WINAPK_SHIM("gmtime_r", g_gmtime_r),
    WINAPK_SHIM("localtime_r", g_localtime_r),
    WINAPK_SHIM("gmtime", g_gmtime),
    WINAPK_SHIM("localtime", g_localtime),
    WINAPK_SHIM("mktime", g_mktime),
    WINAPK_SHIM("timegm", g_timegm),
    WINAPK_SHIM("strftime", g_strftime),
    WINAPK_SHIM("tzset", g_tzset),
    WINAPK_SHIM("mmap", g_mmap),
    WINAPK_SHIM("mmap64", g_mmap),
    WINAPK_SHIM("munmap", g_munmap),
    WINAPK_SHIM("mprotect", g_mprotect),
    WINAPK_SHIM("madvise", g_madvise),
    WINAPK_SHIM("msync", g_msync),
    WINAPK_SHIM("mlock", g_mlock),
    WINAPK_SHIM("munlock", g_munlock),
    WINAPK_SHIM("__system_property_get", g_property_get),
    WINAPK_SHIM("__system_property_find", g_property_find),
    WINAPK_SHIM("__system_property_read", g_property_read),
    WINAPK_SHIM("__system_property_read_callback", g_property_read_callback),
    WINAPK_SHIM("__system_property_set", g_property_set),
    WINAPK_SHIM("android_get_device_api_level", g_get_device_api_level),
    WINAPK_SHIM("syscall", g_syscall),
    WINAPK_SHIM("strftime_l", g_strftime),
    {"timezone", &g_timezone},
    {"daylight", &g_daylight},
    {"tzname", g_tzname},
    {"in6addr_any", g_in6addr_any},
    {"in6addr_loopback", g_in6addr_loopback},
    WINAPK_SHIM("getenv", g_getenv),
    {"environ", &g_environ},
    WINAPK_SHIM("secure_getenv", g_getenv),
    WINAPK_SHIM("setenv", g_setenv),
    WINAPK_SHIM("unsetenv", g_unsetenv),
    WINAPK_SHIM("getpid", g_getpid),
    WINAPK_SHIM("getppid", g_getppid),
    WINAPK_SHIM("getuid", g_getuid),
    WINAPK_SHIM("geteuid", g_getuid),
    WINAPK_SHIM("getgid", g_getuid),
    WINAPK_SHIM("getegid", g_getuid),
    WINAPK_SHIM("kill", g_kill),
    WINAPK_SHIM("getrlimit", g_getrlimit),
    WINAPK_SHIM("getrlimit64", g_getrlimit),
    WINAPK_SHIM("setrlimit", g_setrlimit),
    WINAPK_SHIM("setrlimit64", g_setrlimit),
    WINAPK_SHIM("prlimit", g_prlimit),
    WINAPK_SHIM("prlimit64", g_prlimit),
    WINAPK_SHIM("sysconf", g_sysconf),
    WINAPK_SHIM("getpagesize", g_getpagesize),
    WINAPK_SHIM("uname", g_uname),
    WINAPK_SHIM("getauxval", g_getauxval),
    WINAPK_SHIM("arc4random", g_arc4random),
    WINAPK_SHIM("arc4random_buf", g_arc4random_buf),
    WINAPK_SHIM("arc4random_uniform", g_arc4random_uniform),
    WINAPK_SHIM("getrandom", g_getrandom),
    WINAPK_SHIM("srand", g_srand),
    WINAPK_SHIM("rand", g_rand),
    WINAPK_SHIM("srandom", g_srand),
    WINAPK_SHIM("random", g_rand),
    WINAPK_SHIM("drand48", g_drand48),
    WINAPK_SHIM("erand48", g_erand48),
    WINAPK_SHIM("lrand48", g_lrand48),
    WINAPK_SHIM("nrand48", g_nrand48),
    WINAPK_SHIM("mrand48", g_mrand48),
    WINAPK_SHIM("jrand48", g_jrand48),
    WINAPK_SHIM("srand48", g_srand48),
    WINAPK_SHIM("seed48", g_seed48),
    WINAPK_SHIM("lcong48", g_lcong48),
    WINAPK_SHIM("getopt", g_getopt),
    WINAPK_SHIM("getopt_long", g_getopt_long),
    WINAPK_SHIM("getopt_long_only", g_getopt_long_only),
    WINAPK_SHIM("optarg", optarg),
    WINAPK_SHIM("optind", optind),
    WINAPK_SHIM("opterr", opterr),
    WINAPK_SHIM("optopt", optopt),
    WINAPK_SHIM("optreset", __mingw_optreset),
    WINAPK_SHIM("exit", g_exit),
    WINAPK_SHIM("_exit", g__exit),
    WINAPK_SHIM("_Exit", g__exit),
    WINAPK_SHIM("atexit", g_atexit),
    M(sqrt), M(cbrt), M(sin), M(cos), M(tan), M(asin), M(acos), M(atan), M(sinh), M(cosh), M(tanh), M(asinh),
    M(acosh), M(atanh), M(exp), M(exp2), M(expm1), M(log), M(log2), M(log10), M(log1p), M(fabs), M(floor),
    M(ceil), M(round), M(trunc), M(rint), M(nearbyint), M(erf), M(erfc), M(tgamma), M(lgamma), M(logb),
    M(pow), M(atan2), M(fmod), M(hypot), M(fmin), M(fmax), M(copysign), M(remainder), M(fdim), M(nextafter),
    M(ldexp), M(scalbn), M(frexp), M(modf), M(fma), M(lround), M(lrint), M(sincos), M(nan), M(isinf), M(isnan),
    WINAPK_SHIM("llround", m_llround),
    WINAPK_SHIM("llrint", m_llrint),
    WINAPK_SHIM("ilogb", m_ilogb),
    WINAPK_SHIM("finite", m_finite),
    WINAPK_SHIM("__isinf", m_isinf),
    WINAPK_SHIM("__isnan", m_isnan),
    WINAPK_SHIM("__isfinite", m_finite),
};

#undef M

}  // namespace

}  // namespace winapk::bionic

namespace winapk {

void property_set(const char* name, const char* value) {
    if (!name) return;
    std::lock_guard<std::mutex> hold(bionic::g_props_lock);
    if (value) bionic::props()[name] = value;
    else bionic::props().erase(name);
}

const char* bionic_getenv(const char* name) {
    if (!name) return nullptr;
    std::lock_guard<std::mutex> hold(bionic::g_env_lock);
    auto it = bionic::env().find(name);
    return it == bionic::env().end() ? nullptr : bionic::intern(it->second);
}

int bionic_setenv(const char* name, const char* value, int overwrite) {
    if (!name || !*name || std::strchr(name, '=') || !value) return -bionic::kEINVAL;
    std::lock_guard<std::mutex> hold(bionic::g_env_lock);
    auto& e = bionic::env();
    if (overwrite || e.find(name) == e.end()) {
        e[name] = value;
        bionic::rebuild_environ();
    }
    return 0;
}

int bionic_unsetenv(const char* name) {
    if (!name || !*name || std::strchr(name, '=')) return -bionic::kEINVAL;
    std::lock_guard<std::mutex> hold(bionic::g_env_lock);
    if (bionic::env().erase(name)) bionic::rebuild_environ();
    return 0;
}

}  // namespace winapk

namespace winapk::bionic {
ShimTable misc_symbols() { return WINAPK_SHIM_TABLE(kSymbols); }
}  // namespace winapk::bionic
