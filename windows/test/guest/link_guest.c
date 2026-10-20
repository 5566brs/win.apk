// link_guest.c -- the Android x86_64 libraries for the guest linker test
// (test/linker_test.cpp). One source, four libraries, picked with -D:
//
//   WINAPK_LINK_ROOT    libwinapk_link_root.so    needs mid, libc.so, libdl.so
//   WINAPK_LINK_MID     libwinapk_link_mid.so     needs base, liblog.so
//   WINAPK_LINK_BASE    libwinapk_link_base.so    needs mid (a DT_NEEDED cycle)
//   WINAPK_LINK_PLUGIN  libwinapk_link_plugin.so  no dependencies; dlopen'ed by root
//
// build_guest.sh links them against stub system libraries so the DT_NEEDED
// entries look like an NDK build's. The cycle is real: base is linked against
// a placeholder with mid's soname, then mid is linked against base.
//
// Constructors call an external function on purpose. LLVM evaluates a
// constructor that only stores a constant at compile time and deletes it.

typedef unsigned long size_t;

size_t strlen(const char*);
int strcmp(const char*, const char*);

#define EXPORT __attribute__((visibility("default")))

#if defined(WINAPK_LINK_BASE)

int link_mid_twice(int v);  // in mid, which needs base: the cycle

static int g_base_value;

__attribute__((constructor)) static void base_init(void) { g_base_value = 40 + (int)strlen("ok"); }

EXPORT int link_base_value(void) { return g_base_value; }

EXPORT int link_base_calls_mid(int v) { return link_mid_twice(v); }

// Found through dlsym on a library that depends on this one, the way a JNI
// method in a dependency is.
EXPORT int Java_WinApkLink_fromBase(void) { return 7; }

#elif defined(WINAPK_LINK_MID)

int link_base_value(void);

static int g_mid_seen;

// base is a dependency of mid, so its constructor has run by now: 42.
__attribute__((constructor)) static void mid_init(void) { g_mid_seen = link_base_value() + (int)strlen(""); }

EXPORT int link_mid_twice(int v) { return 2 * v; }

EXPORT int link_mid_seen(void) { return g_mid_seen; }

#elif defined(WINAPK_LINK_ROOT)

typedef struct {
    const char* dli_fname;
    void* dli_fbase;
    const char* dli_sname;
    void* dli_saddr;
} Dl_info;

void* dlopen(const char*, int);
void* dlsym(void*, const char*);
int dlclose(void*);
char* dlerror(void);
int dladdr(const void*, Dl_info*);

#define RTLD_NOW 2
#define RTLD_DEFAULT ((void*)0)

int link_mid_seen(void);
int link_base_calls_mid(int);  // defined two levels down, in base

static int g_root_seen;

__attribute__((constructor)) static void root_init(void) { g_root_seen = link_mid_seen() + (int)strlen(""); }

// 42 only if base's and mid's constructors ran before root's.
EXPORT int link_root_init_order(void) { return g_root_seen; }

// An undefined symbol bound through the local group, two levels down, into a
// library that itself calls back up the cycle: 2 * 5 = 10.
EXPORT int link_root_indirect(void) { return link_base_calls_mid(5); }

// dlopen by bare name (found in the search path) and dlsym on the handle: 81.
EXPORT int link_root_dlopen(void) {
    void* h = dlopen("libwinapk_link_plugin.so", RTLD_NOW);
    if (!h) return -1;
    int (*square)(int) = (int (*)(int))dlsym(h, "link_plugin_square");
    if (!square) return -2;
    int r = square(9);
    dlclose(h);
    return r;
}

// A failed dlopen sets dlerror, which reads back once.
EXPORT int link_root_dlerror(void) {
    if (dlopen("libwinapk_not_there.so", RTLD_NOW)) return -1;
    const char* e = dlerror();
    if (!e || strlen(e) == 0) return -2;
    if (dlerror()) return -3;
    return 1;
}

// dladdr names this very function.
EXPORT int link_root_dladdr(void) {
    Dl_info info;
    if (!dladdr((const void*)&link_root_dladdr, &info)) return -1;
    if (!info.dli_sname || strcmp(info.dli_sname, "link_root_dladdr") != 0) return -2;
    if (info.dli_saddr != (void*)&link_root_dladdr) return -3;
    if (!info.dli_fname || !info.dli_fbase) return -4;
    return 1;
}

// RTLD_DEFAULT sees libc, but not libraries loaded without RTLD_GLOBAL.
EXPORT int link_root_rtld_default(void) {
    if (!dlsym(RTLD_DEFAULT, "malloc")) return -1;
    if (dlsym(RTLD_DEFAULT, "link_base_value")) return -2;
    return 1;
}

#elif defined(WINAPK_LINK_PLUGIN)

EXPORT int link_plugin_square(int v) { return v * v; }

#endif
