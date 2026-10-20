// mini_bionic.cpp -- see mini_bionic.h.

#include "winapk/mini_bionic.h"

#include "winapk/elf_tls.h"
#include "winapk/guest_abi.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "bionic/bionic_internal.h"

namespace winapk {

namespace {

// ---- memory ---------------------------------------------------------------
// Bionic and the UCRT both return 16-byte aligned blocks on x86_64, which is
// what guest code compiled for Android assumes.
WINAPK_GUEST_ABI void* g_malloc(size_t n) { return std::malloc(n); }
WINAPK_GUEST_ABI void* g_calloc(size_t n, size_t sz) { return std::calloc(n, sz); }
WINAPK_GUEST_ABI void* g_realloc(void* p, size_t n) { return std::realloc(p, n); }
WINAPK_GUEST_ABI void g_free(void* p) { std::free(p); }

// ---- string / memory routines --------------------------------------------
WINAPK_GUEST_ABI void* g_memcpy(void* d, const void* s, size_t n) { return std::memcpy(d, s, n); }
WINAPK_GUEST_ABI void* g_memmove(void* d, const void* s, size_t n) { return std::memmove(d, s, n); }
WINAPK_GUEST_ABI void* g_memset(void* d, int c, size_t n) { return std::memset(d, c, n); }
WINAPK_GUEST_ABI int g_memcmp(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n); }
WINAPK_GUEST_ABI size_t g_strlen(const char* s) { return std::strlen(s); }
WINAPK_GUEST_ABI int g_strcmp(const char* a, const char* b) { return std::strcmp(a, b); }
WINAPK_GUEST_ABI int g_strncmp(const char* a, const char* b, size_t n) { return std::strncmp(a, b, n); }
WINAPK_GUEST_ABI char* g_strcpy(char* d, const char* s) { return std::strcpy(d, s); }

// ---- stdio ----------------------------------------------------------------
WINAPK_GUEST_ABI int g_puts(const char* s) {
    int r = std::puts(s);
    std::fflush(stdout);
    return r;
}

// ---- liblog ---------------------------------------------------------------
WINAPK_GUEST_ABI int g_android_log_write(int prio, const char* tag, const char* text) {
    bionic::log_line(prio, tag, text);
    return 1;
}

// ---- stack protector / abort ----------------------------------------------
[[noreturn]] WINAPK_GUEST_ABI void g_stack_chk_fail() {
    std::fprintf(stderr, "[win.apk] guest stack corruption detected (__stack_chk_fail)\n");
    std::fflush(stderr);
    std::abort();
}

[[noreturn]] WINAPK_GUEST_ABI void g_abort() {
    std::fprintf(stderr, "[win.apk] guest called abort()\n");
    std::fflush(stderr);
    std::abort();
}

// ---- errno ----------------------------------------------------------------
// Bionic's errno is `*__errno()`, in Linux numbering; see bionic_internal.h.
WINAPK_GUEST_ABI int* g_errno() { return bionic::guest_errno(); }

// ---- C++ static destructors -----------------------------------------------
using GuestDtor = void(WINAPK_GUEST_ABI*)(void*);
struct AtexitEntry { GuestDtor fn; void* arg; void* dso; };
std::mutex g_atexit_lock;
std::vector<AtexitEntry> g_atexit;

WINAPK_GUEST_ABI int g_cxa_atexit(GuestDtor fn, void* arg, void* dso) {
    std::lock_guard<std::mutex> lock(g_atexit_lock);
    g_atexit.push_back({fn, arg, dso});
    return 0;
}

WINAPK_GUEST_ABI void g_cxa_finalize(void* dso) {
    std::vector<AtexitEntry> run;
    {
        std::lock_guard<std::mutex> lock(g_atexit_lock);
        for (size_t i = g_atexit.size(); i-- > 0;) {
            if (dso == nullptr || g_atexit[i].dso == dso) {
                run.push_back(g_atexit[i]);
                g_atexit.erase(g_atexit.begin() + static_cast<ptrdiff_t>(i));
            }
        }
    }
    for (const AtexitEntry& e : run) e.fn(e.arg);
}

// ---- qsort: a guest callback invoked from host code -------------------------
// The CRT's qsort calls its comparator with the Microsoft ABI. The guest's
// comparator expects System V. An ms_abi trampoline reads the guest pointer
// from a thread_local; saving and restoring it keeps nested qsort calls (a
// comparator that itself sorts) correct.
using GuestCompare = int(WINAPK_GUEST_ABI*)(const void*, const void*);
thread_local GuestCompare t_guest_compare = nullptr;

int host_compare_trampoline(const void* a, const void* b) { return t_guest_compare(a, b); }

WINAPK_GUEST_ABI void g_qsort(void* base, size_t n, size_t size, GuestCompare cmp) {
    GuestCompare saved = t_guest_compare;
    t_guest_compare = cmp;
    std::qsort(base, n, size, host_compare_trampoline);
    t_guest_compare = saved;
}

using bionic::ShimSymbol;
using bionic::ShimTable;

const ShimSymbol kSymbols[] = {
    WINAPK_SHIM("malloc", g_malloc),
    WINAPK_SHIM("calloc", g_calloc),
    WINAPK_SHIM("realloc", g_realloc),
    WINAPK_SHIM("free", g_free),
    WINAPK_SHIM("memcpy", g_memcpy),
    WINAPK_SHIM("memmove", g_memmove),
    WINAPK_SHIM("memset", g_memset),
    WINAPK_SHIM("memcmp", g_memcmp),
    WINAPK_SHIM("strlen", g_strlen),
    WINAPK_SHIM("strcmp", g_strcmp),
    WINAPK_SHIM("strncmp", g_strncmp),
    WINAPK_SHIM("strcpy", g_strcpy),
    WINAPK_SHIM("puts", g_puts),
    WINAPK_SHIM("__android_log_write", g_android_log_write),
    WINAPK_SHIM("__stack_chk_fail", g_stack_chk_fail),
    WINAPK_SHIM("abort", g_abort),
    WINAPK_SHIM("__errno", g_errno),
    WINAPK_SHIM("__cxa_atexit", g_cxa_atexit),
    WINAPK_SHIM("__cxa_finalize", g_cxa_finalize),
    WINAPK_SHIM("qsort", g_qsort),
    WINAPK_SHIM("__tls_get_addr", winapk::tls_get_addr),
};

}  // namespace

namespace bionic {

thread_local int t_guest_errno = 0;
int* guest_errno() { return &t_guest_errno; }

int linux_errno(int e) {
    // The UCRT agrees with Linux up to ERANGE (34), except where noted.
    switch (e) {
        case EDEADLK: return 35;        // UCRT 36
        case ENAMETOOLONG: return 36;   // 38
        case ENOLCK: return 37;         // 39
        case ENOSYS: return 38;         // 40
        case ENOTEMPTY: return 39;      // 41
        case EILSEQ: return 84;         // 42
        case EADDRINUSE: return 98;
        case EADDRNOTAVAIL: return 99;
        case EAFNOSUPPORT: return 97;
        case EALREADY: return 114;
        case EBADMSG: return 74;
        case ECANCELED: return 125;
        case ECONNABORTED: return 103;
        case ECONNREFUSED: return 111;
        case ECONNRESET: return 104;
        case EDESTADDRREQ: return 89;
        case EHOSTUNREACH: return 113;
        case EIDRM: return 43;
        case EINPROGRESS: return 115;
        case EISCONN: return 106;
        case ELOOP: return 40;
        case EMSGSIZE: return 90;
        case ENETDOWN: return 100;
        case ENETRESET: return 102;
        case ENETUNREACH: return 101;
        case ENOBUFS: return 105;
        case ENODATA: return 61;
        case ENOLINK: return 67;
        case ENOMSG: return 42;
        case ENOPROTOOPT: return 92;
        case ENOSR: return 63;
        case ENOSTR: return 60;
        case ENOTCONN: return 107;
        case ENOTRECOVERABLE: return 131;
        case ENOTSOCK: return 88;
        case ENOTSUP: return 95;
        case EOPNOTSUPP: return 95;
        case EOVERFLOW: return 75;
        case EOWNERDEAD: return 130;
        case EPROTO: return 71;
        case EPROTONOSUPPORT: return 93;
        case EPROTOTYPE: return 91;
        case ETIME: return 62;
        case ETIMEDOUT: return 110;
        case ETXTBSY: return 26;
        case EWOULDBLOCK: return 11;
        default: return (e > 0 && e <= 34) ? e : kEIO;
    }
}

void set_errno_from_crt(int crt_errno) { t_guest_errno = linux_errno(crt_errno); }

void set_errno_from_win32(unsigned long e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH: case ERROR_BAD_PATHNAME: t_guest_errno = kENOENT; break;
        case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: t_guest_errno = kEACCES; break;
        case ERROR_INVALID_HANDLE: t_guest_errno = kEBADF; break;
        case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: t_guest_errno = kENOMEM; break;
        case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: t_guest_errno = kEEXIST; break;
        case ERROR_DIR_NOT_EMPTY: t_guest_errno = kENOTEMPTY; break;
        case ERROR_INVALID_PARAMETER: case ERROR_INVALID_NAME: t_guest_errno = kEINVAL; break;
        case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: t_guest_errno = kENOSPC; break;
        case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: t_guest_errno = kEBUSY; break;
        case ERROR_DIRECTORY: t_guest_errno = kENOTDIR; break;
        case ERROR_TOO_MANY_OPEN_FILES: t_guest_errno = kEMFILE; break;
        case ERROR_FILENAME_EXCED_RANGE: t_guest_errno = kENAMETOOLONG; break;
        case ERROR_NOT_SUPPORTED: case ERROR_CALL_NOT_IMPLEMENTED: t_guest_errno = kENOSYS; break;
        case WAIT_TIMEOUT: t_guest_errno = kETIMEDOUT; break;
        default: t_guest_errno = kEIO; break;
    }
}

void log_line(int prio, const char* tag, const char* text) {
    // android_LogPriority: 2 VERBOSE .. 7 FATAL.
    static const char kLetters[] = "??VDIWEF";
    const char letter = (prio >= 0 && prio <= 7) ? kLetters[prio] : '?';
    std::fprintf(stderr, "%c/%s: %s\n", letter, tag ? tag : "", text ? text : "");
    std::fflush(stderr);
}

}  // namespace bionic

void* mini_bionic_resolve(const char* name) {
    // Every module's table, merged once; the core table above wins a clash.
    static const std::unordered_map<std::string, void*> all = [] {
        std::unordered_map<std::string, void*> m;
        auto add = [&m](const ShimSymbol* syms, size_t n) {
            for (size_t i = 0; i < n; ++i) m.emplace(syms[i].name, syms[i].addr);
        };
        add(kSymbols, sizeof(kSymbols) / sizeof(kSymbols[0]));
        for (ShimTable t : {bionic::string_symbols(), bionic::printf_symbols(), bionic::pthread_symbols(),
                            bionic::stdio_symbols(), bionic::misc_symbols(),
                            bionic::locale_symbols(), bionic::fd_symbols(),
                            bionic::socket_symbols(), bionic::looper_symbols()}) {
            add(t.symbols, t.count);
        }
        return m;
    }();
    auto it = all.find(name);
    return it == all.end() ? nullptr : it->second;
}

void mini_bionic_run_atexit() { g_cxa_finalize(nullptr); }

}  // namespace winapk
