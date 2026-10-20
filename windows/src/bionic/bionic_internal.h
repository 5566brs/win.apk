// bionic_internal.h -- shared pieces of the Windows Bionic shim (src/bionic/).
//
// The shim is Android's libc, libm, libdl and liblog for x86_64 guest code, on
// the Windows CRT and Win32. Each module (string.cpp, printf.cpp, ...) exports
// one table of names; mini_bionic_resolve looks a name up in all of them.
// Every function a guest can call is WINAPK_GUEST_ABI (System V), and every
// value that crosses the boundary uses Bionic's layout and Linux's numbers.
// Upstream's runtime/translator/src/bionic_shim.cpp is the behavioural
// reference for each function.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "winapk/guest_abi.h"

namespace winapk::bionic {

// Guest C types whose size differs on Windows: Android's `long` is 64 bits
// (LP64), Windows' is 32 (LLP64). Never write `long` in a guest signature.
using glong = int64_t;
using gulong = uint64_t;

struct ShimSymbol {
    const char* name;
    void* addr;
};

struct ShimTable {
    const ShimSymbol* symbols;
    size_t count;
};

#define WINAPK_SHIM(name, fn) {name, reinterpret_cast<void*>(&fn)}
#define WINAPK_SHIM_TABLE(array) ShimTable{array, sizeof(array) / sizeof(array[0])}

// The module tables.
ShimTable string_symbols();   // string.cpp: <string.h>, <stdlib.h> conversions, <ctype.h>, _chk
ShimTable printf_symbols();   // printf.cpp: the printf family and liblog's formatted entry points
ShimTable pthread_symbols();  // pthread.cpp: threads, mutexes, conditions, keys, once
ShimTable stdio_symbols();    // stdio.cpp: FILE, file descriptors, directories, stat
ShimTable misc_symbols();     // misc.cpp: time, sysconf, system properties, environment, math
ShimTable fd_symbols();       // fd.cpp: pipe, eventfd, poll, select, epoll
ShimTable socket_symbols();
ShimTable looper_symbols();   // looper.cpp: the NDK's ALooper   // socket.cpp: BSD sockets on Winsock, getaddrinfo, inet_*
ShimTable locale_symbols();   // locale.cpp: <locale.h>, UTF-8 conversion, <wctype.h>, more <wchar.h>

// ---- errno ------------------------------------------------------------------

// The guest's errno (`*__errno()`), per thread, in Linux numbering.
int* guest_errno();

// Sets the guest's errno from a UCRT errno value, translated to Linux's.
void set_errno_from_crt(int crt_errno);
// Sets the guest's errno from a Win32 error code.
void set_errno_from_win32(unsigned long win32_error);
// Linux's number for a UCRT errno value.
int linux_errno(int crt_errno);

// Linux errno values the shim sets directly.
constexpr int kEPERM = 1, kENOENT = 2, kEINTR = 4, kEIO = 5, kEBADF = 9, kEAGAIN = 11, kENOMEM = 12,
              kEACCES = 13, kEFAULT = 14, kEBUSY = 16, kEEXIST = 17, kENOTDIR = 20, kEISDIR = 21,
              kEINVAL = 22, kEMFILE = 24, kENOSPC = 28, kESPIPE = 29, kERANGE = 34, kEDEADLK = 35,
              kENAMETOOLONG = 36, kENOSYS = 38, kENOTEMPTY = 39, kETIMEDOUT = 110, kENOTSUP = 95;

// ---- shared between modules ---------------------------------------------------

// strerror's text for a Linux errno value (string.cpp).
const char* errno_text(int linux_errno);

// stdio.cpp: write to a guest FILE* (returns the bytes written) or to a guest
// file descriptor (returns -1 and sets errno on failure), and the guest's
// stdout and stderr.
size_t file_write(void* guest_file, const char* data, size_t n);
int64_t fd_write(int fd, const void* data, size_t n);
void* guest_stdout();
void* guest_stderr();

// paths.cpp: the host path for a guest path (Android's absolute paths are
// mapped under the android root; see paths.cpp).
std::wstring host_path(const char* guest_path);

// ---- descriptors that are not files (fd.cpp) -------------------------------------
// Pipes, eventfds, epoll instances and sockets. Each holds a CRT descriptor
// number of its own (open on NUL), so numbers never collide with files, and
// the descriptor functions in stdio.cpp hand calls on such a number to its
// object. dup shares the object; it is destroyed when its last number closes.
// Results follow the syscalls: >= 0, or -1 with the guest's errno set.
class FdObject {
public:
    virtual ~FdObject() = default;
    virtual int64_t read(void* buf, size_t n);
    virtual int64_t write(const void* buf, size_t n);
    // The subset of `events` (POLLIN 1, POLLOUT 4, ...) that is ready, plus
    // POLLERR 8 / POLLHUP 16 whatever was asked.
    virtual short ready(short events) = 0;
    // st_mode for fstat (S_IFIFO, S_IFSOCK, ...).
    virtual unsigned mode() const { return 0010000; }
    virtual int ioctl(int request, void* arg);
    // True when readiness changes without fd_notify() (sockets): waits then
    // re-check every few milliseconds.
    virtual bool needs_polling() const { return false; }
    // O_NONBLOCK (fcntl F_SETFL, ioctl FIONBIO, the *_NONBLOCK flags).
    virtual void set_nonblocking(bool on) { nonblocking = on; }
    bool nonblocking = false;
};
std::shared_ptr<FdObject> fd_object(int fd);
// A new descriptor number for `object`, or -1 (errno set).
int fd_install(std::shared_ptr<FdObject> object);
// Close/dup for object descriptors; false if `fd` is not one.
bool fd_close(int fd);
int fd_dup(int fd, int to);  // to < 0: lowest free number
// Something became readable/writable: wakes poll, select and epoll_wait.
void fd_notify();
// poll(2) over guest descriptors, for other modules (looper.cpp).
struct PollFd {
    int fd;
    short events, revents;
};
int fd_poll(PollFd* fds, uint64_t n, int timeout_ms);

// ---- small helpers ----------------------------------------------------------

// Writes a guest log line the way liblog does: "<prio letter>/<tag>: <text>".
void log_line(int prio, const char* tag, const char* text);

}  // namespace winapk::bionic
