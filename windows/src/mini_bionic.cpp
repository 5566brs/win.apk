// mini_bionic.cpp -- see mini_bionic.h.

#include "winapk/mini_bionic.h"

#include "winapk/guest_abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

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
// android_LogPriority: 2 VERBOSE .. 7 FATAL.
WINAPK_GUEST_ABI int g_android_log_write(int prio, const char* tag, const char* text) {
    static const char kLetters[] = "??VDIWEF";
    const char letter = (prio >= 0 && prio <= 7) ? kLetters[prio] : '?';
    std::fprintf(stderr, "%c/%s: %s\n", letter, tag ? tag : "", text ? text : "");
    std::fflush(stderr);
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
// Bionic's errno is `*__errno()`. Its values are Linux numbers, which differ
// from the UCRT's; the real shim translates at every call that sets it.
thread_local int t_guest_errno = 0;
WINAPK_GUEST_ABI int* g_errno() { return &t_guest_errno; }

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

struct ShimSymbol {
    const char* name;
    void* addr;
};

#define WINAPK_SHIM(name, fn) {name, reinterpret_cast<void*>(&fn)}

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
};

#undef WINAPK_SHIM

}  // namespace

void* mini_bionic_resolve(const char* name) {
    for (const ShimSymbol& s : kSymbols) {
        if (std::strcmp(s.name, name) == 0) return s.addr;
    }
    return nullptr;
}

void mini_bionic_run_atexit() { g_cxa_finalize(nullptr); }

}  // namespace winapk
