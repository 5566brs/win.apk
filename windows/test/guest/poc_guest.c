// poc_guest.c -- the Android x86_64 library the Win.apk loader PoC loads.
//
// Built with clang for x86_64-linux-android, with no NDK sysroot: the libc
// functions below are declared by hand and stay undefined in the .so, exactly
// as a real Android library imports them from libc.so. Each exported
// winapk_test_* function exercises one thing the loader or the ABI boundary
// has to get right; tools/elfrun_main.cpp calls them and checks the results.
//
// Build flags that matter (see build_guest.sh):
//   -fstack-protector-all   every function loads the guard from %fs:0x28
//   -fPIC -shared           RELATIVE / GLOB_DAT / JUMP_SLOT relocations

typedef unsigned long size_t;

void* malloc(size_t);
void free(void*);
void* memcpy(void*, const void*, size_t);
size_t strlen(const char*);
int strcmp(const char*, const char*);
int puts(const char*);
void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
int __android_log_write(int prio, const char* tag, const char* text);

#define EXPORT __attribute__((visibility("default")))

// ---- arguments and return values across the ABI boundary -------------------
EXPORT int winapk_test_add(int a, int b) { return a + b; }

EXPORT double winapk_test_scale(double x, int n) { return x * n; }

// Eight integer arguments: under System V the 7th and 8th go on the stack,
// plus four doubles in xmm0-xmm3.
EXPORT long winapk_test_many_args(long a, long b, long c, long d, long e, long f, long g,
                                  long h, double x, double y, double z, double w) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h +
           (long)(x * 10 + y * 100 + z * 1000 + w * 10000);
}

// ---- relocations ------------------------------------------------------------
// The tables are indexed with a runtime count so the compiler cannot fold them
// away: they must survive as data the loader relocates.
//
// Pointers to strings: R_X86_64_RELATIVE entries in .data.rel.ro.
static const char* const kWords[] = {"win", ".apk", "loads", "android", "x86_64"};

static int twice(int v) { return 2 * v; }
static int square(int v) { return v * v; }
// Function pointers: also RELATIVE, pointing into .text.
static int (*const kOps[])(int) = {twice, square};

// Writable pointer table (.data, RELATIVE) that the guest modifies at runtime.
static const char* g_mutable_words[] = {"a", "bb", "ccc"};

// The address of an exported, preemptible function stored in data: lld emits
// R_X86_64_64 against the symbol for this one.
EXPORT int winapk_test_add(int a, int b);
EXPORT int (*const winapk_test_add_ptr)(int, int) = winapk_test_add;

// n = 5: 25 + square(10) + twice(3) + ("a" 1 + "bb" 2 + "win" 3) + add(5, 1) = 143
EXPORT int winapk_test_relocs(int n) {
    int total = 0;
    for (int i = 0; i < n; ++i) total += (int)strlen(kWords[i % 5]);
    total += kOps[n & 1](10) + kOps[(n + 1) & 1](3);
    g_mutable_words[n % 3] = kWords[0];
    for (int i = 0; i < 3; ++i) total += (int)strlen(g_mutable_words[i]);
    return total + winapk_test_add_ptr(n, 1);
}

// ---- constructors -----------------------------------------------------------
// The external call keeps LLVM from evaluating the constructor at compile time
// and deleting it, so the .so really carries a DT_INIT_ARRAY entry.
static volatile int g_ctor_value;

__attribute__((constructor)) static void winapk_poc_ctor(void) {
    g_ctor_value = 0x1234;
    __android_log_write(3 /* ANDROID_LOG_DEBUG */, "WinApkPoc", "constructor ran");
}

EXPORT int winapk_test_ctor_ran(void) { return g_ctor_value; }

// ---- libc through the shim --------------------------------------------------
EXPORT int winapk_test_libc(void) {
    const char* src = "hello from an Android x86_64 library";
    size_t n = strlen(src) + 1;
    char* copy = (char*)malloc(n);
    if (!copy) return -1;
    memcpy(copy, src, n);
    int ok = strcmp(copy, src) == 0;
    puts(copy);
    free(copy);
    return ok ? (int)n : -2;
}

// ---- stack protector --------------------------------------------------------
// A local array guarantees a canary even without -fstack-protector-all.
EXPORT int winapk_test_stack_guard(int seed) {
    volatile char buf[64];
    for (int i = 0; i < 64; ++i) buf[i] = (char)(seed + i);
    int sum = 0;
    for (int i = 0; i < 64; ++i) sum += buf[i];
    return sum;
}

// ---- callbacks in both directions -------------------------------------------
static int compare_ints(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

// Guest comparator called by the host's qsort.
EXPORT int winapk_test_qsort(void) {
    int v[] = {42, 7, 19, 3, 88, 21, 1, 56};
    qsort(v, 8, sizeof(int), compare_ints);
    for (int i = 1; i < 8; ++i) {
        if (v[i - 1] > v[i]) return 0;
    }
    return v[0] * 100 + v[7];  // 1 * 100 + 88 = 188
}

// Host function pointer called by the guest.
EXPORT int winapk_test_callback(int (*host_fn)(int), int v) { return host_fn(v) + host_fn(v + 1); }

// ---- liblog -----------------------------------------------------------------
EXPORT int winapk_test_log(void) {
    return __android_log_write(4 /* ANDROID_LOG_INFO */, "WinApkPoc", "log line from the guest");
}
