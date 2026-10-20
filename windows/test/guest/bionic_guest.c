// bionic_guest.c -- guest-side test of the Windows Bionic shim (src/bionic/).
//
// Built like the other test guests (build_guest.sh): plain clang for
// x86_64-linux-android, no NDK, the libc imports declared by hand. Every check
// goes through the shim exactly as NDK code does, System V calls and Bionic
// layouts included. bionic_test(dir) returns the number of failed checks and
// prints one line per check; `dir` is a scratch directory.

typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;
typedef int wchar_t;  // 32-bit on Android

int printf(const char*, ...);
int snprintf(char*, size_t, const char*, ...);
int sprintf(char*, const char*, ...);
int asprintf(char**, const char*, ...);
int strcmp(const char*, const char*);
size_t strlen(const char*);
char* strchr(const char*, int);
char* strstr(const char*, const char*);
char* strdup(const char*);
size_t strlcpy(char*, const char*, size_t);
int strcasecmp(const char*, const char*);
long strtol(const char*, char**, int);
unsigned long strtoul(const char*, char**, int);
double strtod(const char*, char**);
int atoi(const char*);
void* malloc(size_t);
void free(void*);
void* memset(void*, int, size_t);
int* __errno(void);
char* strerror(int);
int isdigit(int);
int toupper(int);
size_t wcslen(const wchar_t*);

typedef struct { int __private[10]; } pthread_mutex_t;
typedef struct { int __private[12]; } pthread_cond_t;
typedef long pthread_t;
typedef int pthread_key_t;
typedef int pthread_once_t;
int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
int pthread_join(pthread_t, void**);
int pthread_mutex_lock(pthread_mutex_t*);
int pthread_mutex_unlock(pthread_mutex_t*);
int pthread_mutex_trylock(pthread_mutex_t*);
int pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
int pthread_cond_signal(pthread_cond_t*);
int pthread_key_create(pthread_key_t*, void (*)(void*));
int pthread_setspecific(pthread_key_t, const void*);
void* pthread_getspecific(pthread_key_t);
int pthread_once(pthread_once_t*, void (*)(void));
pthread_t pthread_self(void);

int open(const char*, int, ...);
int close(int);
ssize_t read(int, void*, size_t);
ssize_t write(int, const void*, size_t);
off_t lseek(int, off_t, int);
int unlink(const char*);
int mkdir(const char*, unsigned);
int rmdir(const char*);
struct stat { unsigned long dev, ino, nlink; unsigned mode, uid, gid; int pad; unsigned long rdev;
              long size, blksize, blocks, atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns, unused[3]; };
int stat(const char*, struct stat*);
typedef struct DIR DIR;
struct dirent { unsigned long d_ino; long d_off; unsigned short d_reclen; unsigned char d_type; char d_name[256]; };
DIR* opendir(const char*);
struct dirent* readdir(DIR*);
int closedir(DIR*);
typedef struct FILE FILE;
FILE* fopen(const char*, const char*);
int fprintf(FILE*, const char*, ...);
int fclose(FILE*);
char* fgets(char*, int, FILE*);

struct timespec { long tv_sec; long tv_nsec; };
int clock_gettime(int, struct timespec*);
int nanosleep(const struct timespec*, struct timespec*);
int __system_property_get(const char*, char*);
char* getenv(const char*);
int setenv(const char*, const char*, int);
void* mmap(void*, size_t, int, int, int, long);
int munmap(void*, size_t);
int mprotect(void*, size_t, int);
extern char** environ;
long sysconf(int);

#define EXPORT __attribute__((visibility("default")))

static int failures;

static void check(const char* what, int ok, const char* got) {
    printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, got ? ": " : "", got ? got : "");
    if (!ok) ++failures;
}

static void check_str(const char* what, const char* got, const char* want) {
    check(what, strcmp(got, want) == 0, got);
}

// ---- printf -----------------------------------------------------------------
static void test_printf(void) {
    char b[256];
    snprintf(b, sizeof b, "%d %u %x %ld %lld", -42, 3000000000u, 0xbeef, -9000000000L, 123456789012LL);
    check_str("snprintf integers", b, "-42 3000000000 beef -9000000000 123456789012");
    snprintf(b, sizeof b, "%.3f|%8.2f|%-6.1f|%e|%g", 3.14159, 2.5, 1.25, 12345.678, 0.0001);
    check_str("snprintf doubles", b, "3.142|    2.50|1.2   |1.234568e+04|0.0001");
    // More than eight doubles and six integers: the rest go on the stack.
    snprintf(b, sizeof b, "%d %d %d %d %d %d %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f", 1, 2, 3, 4, 5, 6,
             1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0);
    check_str("snprintf stack arguments", b, "1 2 3 4 5 6 1 2 3 4 5 6 7 8 9 10");
    snprintf(b, sizeof b, "[%s] [%5s] [%-5s] [%.2s] [%c] [%%] [%s]", "abc", "ab", "ab", "abcdef", 'z', (char*)0);
    check_str("snprintf strings", b, "[abc] [   ab] [ab   ] [ab] [z] [%] [(null)]");
    snprintf(b, sizeof b, "%*d|%-*d|%.*f", 5, 7, 4, 8, 2, 1.0);
    check_str("snprintf star width/precision", b, "    7|8   |1.00");
    static const wchar_t w[] = {'w', 'i', 'd', 'e', 0x5d0, 0};
    snprintf(b, sizeof b, "%ls", w);
    check_str("snprintf %ls (32-bit wchar_t)", b, "wide\xd7\x90");
    snprintf(b, sizeof b, "%p", (void*)0x1234);
    check_str("snprintf %p", b, "0x1234");
    int n = 0;
    snprintf(b, sizeof b, "abc%nde", &n);
    check("snprintf %n", n == 3, b);
    const int len = snprintf(b, 5, "123456789");
    check("snprintf truncates and counts", len == 9 && strcmp(b, "1234") == 0, b);
    char* a = 0;
    asprintf(&a, "%s-%d", "x", 99);
    check_str("asprintf", a ? a : "(null)", "x-99");
    free(a);
}

// ---- strings ------------------------------------------------------------------
static void test_strings(void) {
    char b[64];
    check("strtol 64-bit", strtol("-9000000000", 0, 10) == -9000000000L, 0);
    check("strtoul hex", strtoul("ffffffffff", 0, 16) == 0xffffffffffUL, 0);
    check("strtod", strtod("2.5", 0) == 2.5, 0);
    check("atoi", atoi("1234") == 1234, 0);
    check("strchr/strstr", strchr("hello", 'l') && strstr("hello", "llo"), 0);
    check("strcasecmp", strcasecmp("HeLLo", "hello") == 0, 0);
    char* d = strdup("dup");
    check_str("strdup", d, "dup");
    free(d);
    strlcpy(b, "truncated", 6);
    check_str("strlcpy", b, "trunc");
    check("ctype", isdigit('7') && !isdigit('x') && toupper('q') == 'Q', 0);
    static const wchar_t w[] = {'a', 'b', 'c', 0};
    check("wcslen (32-bit)", wcslen(w) == 3, 0);
    check_str("strerror(ENOENT), Linux numbering", strerror(2), "No such file or directory");
}

// ---- threads --------------------------------------------------------------------
static pthread_mutex_t g_mu;  // zero: PTHREAD_MUTEX_INITIALIZER
static pthread_cond_t g_cv;
static int g_counter, g_ready;
static pthread_key_t g_key;
static int g_dtor_runs;
static pthread_once_t g_once;
static int g_once_runs;

static void once_fn(void) { ++g_once_runs; }
static void key_dtor(void* v) { (void)v; ++g_dtor_runs; }

static void* worker(void* arg) {
    pthread_setspecific(g_key, arg);
    pthread_once(&g_once, once_fn);
    for (int i = 0; i < 10000; ++i) {
        pthread_mutex_lock(&g_mu);
        ++g_counter;
        pthread_mutex_unlock(&g_mu);
    }
    pthread_mutex_lock(&g_mu);
    ++g_ready;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return (void*)((long)arg * 2);
}

// ELF TLS (built with -fno-emulated-tls): a hidden variable reached through the
// local-dynamic model, an exported one through general-dynamic, and a .tbss
// array. Every thread must start from the initial values.
static __thread int t_hidden = 11;
__attribute__((visibility("default"))) __thread long t_exported = 22;
static __thread int t_zeroed[64];

static void* tls_worker(void* arg) {
    long id = (long)arg;
    int fresh = t_hidden == 11 && t_exported == 22 && t_zeroed[63] == 0;
    t_hidden += (int)id;
    t_exported *= id;
    t_zeroed[63] = (int)id;
    for (volatile int i = 0; i < 1000; ++i) {}
    int kept = t_hidden == 11 + id && t_exported == 22 * id && t_zeroed[63] == id;
    return (void*)(long)(fresh && kept);
}

static void test_tls(void) {
    check("TLS initial values on the main thread", t_hidden == 11 && t_exported == 22 && t_zeroed[0] == 0, 0);
    t_hidden = 100;
    pthread_t t[3];
    for (long i = 0; i < 3; ++i) pthread_create(&t[i], 0, tls_worker, (void*)(i + 2));
    long ok = 1;
    for (int i = 0; i < 3; ++i) {
        void* r = 0;
        pthread_join(t[i], &r);
        ok &= (long)r;
    }
    check("TLS is per thread (3 threads, .tdata and .tbss)", ok == 1, 0);
    check("TLS of the main thread is untouched by other threads", t_hidden == 100 && t_exported == 22, 0);
}

// UTF-8 conversion, locales and raw syscalls (libc++ and Chromium use them).
size_t mbrtowc(wchar_t*, const char*, size_t, void*);
size_t wcrtomb(char*, wchar_t, void*);
size_t mbstowcs(wchar_t*, const char*, size_t);
char* setlocale(int, const char*);
void* newlocale(int, const char*, void*);
void* uselocale(void*);
long syscall(long, ...);
int gettid(void);
struct futex_timespec { long tv_sec, tv_nsec; };

static void test_locale_and_syscall(void) {
    wchar_t c = 0;
    unsigned st = 0;
    check("mbrtowc decodes a 3-byte UTF-8 character", mbrtowc(&c, "\xe2\x82\xac!", 4, &st) == 3 && c == 0x20ac, 0);
    st = 0;
    int partial = mbrtowc(&c, "\xe2", 1, &st) == (size_t)-2;
    check("mbrtowc continues a split sequence", partial && mbrtowc(&c, "\x82\xac", 2, &st) == 2 && c == 0x20ac, 0);
    check("mbrtowc rejects an illegal byte", mbrtowc(&c, "\xff", 1, 0) == (size_t)-1 && *__errno() == 84, 0);
    char b[8] = {0};
    check("wcrtomb encodes U+1F600 as 4 bytes",
          wcrtomb(b, 0x1f600, 0) == 4 && (unsigned char)b[0] == 0xf0 && (unsigned char)b[3] == 0x80, 0);
    wchar_t w[8];
    check("mbstowcs converts a string", mbstowcs(w, "a\xc3\xa9z", 8) == 3 && w[1] == 0xe9 && w[3] == 0, 0);
    void* loc = newlocale(63, "C", 0);
    check("setlocale and newlocale/uselocale for C and C.UTF-8",
          setlocale(6, "") != 0 && loc != 0 && uselocale(loc) == (void*)-1 && uselocale(0) == loc, 0);
    check("syscall(gettid)", syscall(186) == gettid(), 0);
    unsigned word = 5;
    struct futex_timespec ts = {0, 2000000};
    check("futex WAIT on a changed value fails with EAGAIN", syscall(202, &word, 128, 4, 0) == -1 && *__errno() == 11, 0);
    check("futex WAIT times out with ETIMEDOUT", syscall(202, &word, 128, 5, &ts) == -1 && *__errno() == 110, 0);
    check("futex WAKE succeeds", syscall(202, &word, 129, 1) >= 0, 0);
    check("an unknown syscall fails with ENOSYS", syscall(9999) == -1 && *__errno() == 38, 0);
}

static void test_threads(void) {
    pthread_key_create(&g_key, key_dtor);
    pthread_t t[4];
    for (long i = 0; i < 4; ++i) pthread_create(&t[i], 0, worker, (void*)(i + 1));
    pthread_mutex_lock(&g_mu);
    while (g_ready < 4) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    long sum = 0;
    for (int i = 0; i < 4; ++i) {
        void* r = 0;
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    char b[96];
    snprintf(b, sizeof b, "counter=%d results=%ld once=%d dtors=%d", g_counter, sum, g_once_runs, g_dtor_runs);
    check("4 threads: mutex, condition, join, once, key destructors",
          g_counter == 40000 && sum == 20 && g_once_runs == 1 && g_dtor_runs == 4, b);
    check("trylock on a held mutex fails",
          pthread_mutex_lock(&g_mu) == 0 && pthread_mutex_trylock(&g_mu) != 0 && pthread_mutex_unlock(&g_mu) == 0, 0);
    check("pthread_self is stable", pthread_self() == pthread_self(), 0);
}

// ---- files --------------------------------------------------------------------------
static void test_files(const char* dir) {
    char sub[512], path[512], line[64];
    snprintf(sub, sizeof sub, "%s/bionic_test_dir", dir);
    mkdir(sub, 0755);
    snprintf(path, sizeof path, "%s/file.txt", sub);
    const int fd = open(path, 01 | 0100 | 01000, 0644);  // O_WRONLY | O_CREAT | O_TRUNC
    check("open(O_CREAT)", fd >= 0, 0);
    check("write", write(fd, "hello\n", 6) == 6, 0);
    close(fd);
    struct stat st;
    check("stat size and regular file", stat(path, &st) == 0 && st.size == 6 && (st.mode & 0170000) == 0100000, 0);
    const int rfd = open(path, 0);
    char buf[8] = {0};
    check("read back", rfd >= 0 && read(rfd, buf, 5) == 5 && strcmp(buf, "hello") == 0, buf);
    check("lseek", lseek(rfd, 0, 2) == 6, 0);
    close(rfd);
    FILE* f = fopen(path, "a");
    fprintf(f, "%s %d\n", "line", 2);
    fclose(f);
    f = fopen(path, "r");
    fgets(line, sizeof line, f);
    fgets(line, sizeof line, f);
    fclose(f);
    check_str("fopen/fprintf/fgets", line, "line 2\n");
    int found = 0;
    DIR* d = opendir(sub);
    for (struct dirent* e; d && (e = readdir(d));) found |= strcmp(e->d_name, "file.txt") == 0 && e->d_type == 8;
    if (d) closedir(d);
    check("opendir/readdir", found, 0);
    check("open a missing file sets ENOENT", open("Z:/no/such/file", 0) < 0 && *__errno() == 2, 0);
    unlink(path);
    rmdir(sub);
    check("unlink + rmdir", stat(sub, &st) != 0, 0);
}

// ---- the rest -------------------------------------------------------------------------
static void test_misc(void) {
    struct timespec a, b2;
    clock_gettime(1, &a);
    struct timespec ten_ms = {0, 10000000};
    nanosleep(&ten_ms, 0);
    clock_gettime(1, &b2);
    const long ns = (b2.tv_sec - a.tv_sec) * 1000000000L + (b2.tv_nsec - a.tv_nsec);
    check("CLOCK_MONOTONIC advances across nanosleep(10 ms)", ns >= 9000000 && ns < 500000000, 0);
    clock_gettime(0, &a);
    check("CLOCK_REALTIME is after 2020", a.tv_sec > 1577836800L, 0);
    char v[92];
    check("__system_property_get", __system_property_get("ro.product.cpu.abi", v) > 0 && strcmp(v, "x86_64") == 0, v);
    setenv("WINAPK_TEST", "yes", 1);
    check("setenv/getenv", getenv("WINAPK_TEST") && strcmp(getenv("WINAPK_TEST"), "yes") == 0, 0);
    char* m = mmap(0, 65536, 3, 0x22, -1, 0);  // PROT_READ|WRITE, MAP_PRIVATE|ANONYMOUS
    check("mmap anonymous is zeroed and writable", m != (char*)-1 && m[100] == 0 && (m[100] = 7) == 7, 0);
    munmap(m, 65536);
    // An allocator-style reservation: 8 GiB of PROT_NONE, then one piece
    // opened with mprotect.
    char* big = mmap(0, 1UL << 33, 0, 0x22, -1, 0);
    int opened = big != (char*)-1 && mprotect(big + (1UL << 32), 65536, 3) == 0;
    if (opened) big[(1UL << 32) + 10] = 5;
    check("mmap reserves 8 GiB with PROT_NONE; mprotect commits a piece", opened && big[(1UL << 32) + 10] == 5, 0);
    if (big != (char*)-1) munmap(big, 1UL << 33);
    int found = 0;
    for (char** e = environ; e && *e; ++e) found |= strcmp(*e, "WINAPK_TEST=yes") == 0;
    check("environ lists the guest environment", found, 0);
    check("sysconf(_SC_PAGESIZE)", sysconf(0x27) == 4096, 0);
}

EXPORT int bionic_test(const char* dir) {
    printf("Bionic shim, from guest code\n");
    test_printf();
    test_strings();
    test_threads();
    test_tls();
    test_locale_and_syscall();
    test_files(dir);
    test_misc();
    return failures;
}
