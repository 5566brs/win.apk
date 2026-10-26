// stdio.cpp -- file descriptors, FILE streams, directories and stat, for guest
// code.
//
// Descriptors are the UCRT's own (_wopen and friends), so 0, 1 and 2 are the
// process's standard streams as on Android. What differs is translated here:
// Linux's open flags and errno numbers, UTF-8 paths (Windows' native APIs take
// UTF-16), and the layouts of Bionic's struct stat and struct dirent on
// x86_64. A guest FILE* is the CRT's FILE*; guest code never looks inside one.
// The standard streams are also exported the two ways Bionic does: the
// variables stdin/stdout/stderr (API 23+) and the older __sF array, whose
// elements stand for the same three streams.

#include "bionic_internal.h"
#include "winapk/linker.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#include <bcrypt.h>

namespace winapk::bionic {

namespace {

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

int fail_crt() {
    set_errno_from_crt(errno);
    return -1;
}

// ---- the standard streams ----------------------------------------------------
// Bionic's __sF: three FILE structs, 152 bytes each on LP64. Only their
// addresses matter; they are mapped back to the CRT's streams.
alignas(16) char g_sF[3 * 152];
FILE* g_stdin_var = stdin;
FILE* g_stdout_var = stdout;
FILE* g_stderr_var = stderr;

FILE* host_file(void* f) {
    const auto* p = static_cast<const char*>(f);
    if (p >= g_sF && p < g_sF + sizeof(g_sF)) {
        const ptrdiff_t i = (p - g_sF) / 152;
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    return static_cast<FILE*>(f);
}

// ---- file descriptors -----------------------------------------------------------
// Linux's open(2) flags (x86_64 values).
constexpr int kOWrOnly = 01, kORdWr = 02, kOCreat = 0100, kOExcl = 0200, kOTrunc = 01000, kOAppend = 02000,
              kODirectory = 0200000, kOCloexec = 02000000;

int crt_open_flags(int f) {
    int c = _O_BINARY;
    c |= (f & kORdWr) ? _O_RDWR : (f & kOWrOnly) ? _O_WRONLY : _O_RDONLY;
    if (f & kOCreat) c |= _O_CREAT;
    if (f & kOExcl) c |= _O_EXCL;
    if (f & kOTrunc) c |= _O_TRUNC;
    if (f & kOAppend) c |= _O_APPEND;
    if (f & kOCloexec) c |= _O_NOINHERIT;
    return c;
}

// /dev/urandom and /dev/random: a character device that reads as an endless
// stream from the system RNG. Windows has no such file, so open() failed and
// native code that CHECKs the descriptor (folly's Random.cpp, in WhatsApp
// right after registration) aborted the process.
class RandomDevice : public FdObject {
public:
    int64_t read(void* buf, size_t n) override {
        if (n == 0) return 0;
        if (n > 0x7fffffff) n = 0x7fffffff;
        if (BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(n),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            *guest_errno() = 5;  // EIO
            return -1;
        }
        return static_cast<int64_t>(n);
    }
    int64_t write(const void*, size_t n) override { return static_cast<int64_t>(n); }  // entropy is discarded
    short ready(short events) override { return static_cast<short>(events & (1 | 4)); }  // POLLIN | POLLOUT
    unsigned mode() const override { return 0020000; }  // S_IFCHR
};

// /proc/self/maps (and /proc/<our pid>/maps): synthesized from the guest
// linker on every open, as the kernel does, into a delete-on-close temporary
// file, so read, lseek, fstat and stdio all behave as on a regular file.
bool is_proc_maps(const char* path) {
    if (!path) return false;
    if (std::strcmp(path, "/proc/self/maps") == 0 || std::strcmp(path, "/proc/thread-self/maps") == 0) return true;
    char own[48];
    std::snprintf(own, sizeof own, "/proc/%lu/maps", static_cast<unsigned long>(GetCurrentProcessId()));
    return std::strcmp(path, own) == 0;
}

// A delete-on-close temporary file holding `content`, as a CRT descriptor
// positioned at 0; -1 with errno set.
int open_temp_with(const std::string& content) {
    wchar_t dir[MAX_PATH + 1], name[MAX_PATH + 1];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"prc", 0, name)) {
        *guest_errno() = 5;  // EIO
        return -1;
    }
    const int fd = _wopen(name, _O_RDWR | _O_BINARY | _O_TEMPORARY);
    if (fd < 0) {
        set_errno_from_crt(errno);
        DeleteFileW(name);
        return -1;
    }
    if (!content.empty() && _write(fd, content.data(), static_cast<unsigned>(content.size())) < 0) {
        set_errno_from_crt(errno);
        _close(fd);
        return -1;
    }
    _lseek(fd, 0, SEEK_SET);
    return fd;
}

std::string proc_maps_text() {
    GuestLinker* linker = GuestLinker::process();
    return linker ? linker->proc_maps() : std::string();
}

bool is_random_device(const char* path) {
    return path && (std::strcmp(path, "/dev/urandom") == 0 || std::strcmp(path, "/dev/random") == 0);
}

// open() is variadic (its mode is the third argument only with O_CREAT), but a
// variadic int travels in the same register as a fixed one.
WINAPK_GUEST_ABI int g_open(const char* path, int flags, unsigned mode) {
    if (is_random_device(path)) return fd_install(std::make_shared<RandomDevice>());
    if (is_proc_maps(path)) return open_temp_with(proc_maps_text());
    const std::wstring w = host_path(path);
    if (flags & kODirectory) {
        const DWORD a = GetFileAttributesW(w.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) {
            set_errno_from_win32(GetLastError());
            return -1;
        }
        if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
            *guest_errno() = kENOTDIR;
            return -1;
        }
        // A directory descriptor (for fsync or *at() calls): a handle wrapped
        // in a CRT descriptor.
        HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            set_errno_from_win32(GetLastError());
            return -1;
        }
        return _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY);
    }
    const int fd = _wopen(w.c_str(), crt_open_flags(flags), (mode & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD);
    return fd < 0 ? fail_crt() : fd;
}
WINAPK_GUEST_ABI int g_open_2(const char* path, int flags) { return g_open(path, flags, 0); }
WINAPK_GUEST_ABI int g_creat(const char* path, unsigned mode) { return g_open(path, kOWrOnly | kOCreat | kOTrunc, mode); }
// Pipes, eventfds, epoll and sockets are objects (fd.cpp); each descriptor
// call checks for one first.
WINAPK_GUEST_ABI int g_close(int fd) {
    if (fd_close(fd)) return 0;
    return _close(fd) < 0 ? fail_crt() : 0;
}
WINAPK_GUEST_ABI int64_t g_read(int fd, void* buf, size_t n) {
    if (auto o = fd_object(fd)) return o->read(buf, n);
    const int r = _read(fd, buf, static_cast<unsigned>(n > 0x7fffffff ? 0x7fffffff : n));
    return r < 0 ? fail_crt() : r;
}
WINAPK_GUEST_ABI int64_t g_write(int fd, const void* buf, size_t n) {
    if (auto o = fd_object(fd)) return o->write(buf, n);
    const int r = _write(fd, buf, static_cast<unsigned>(n > 0x7fffffff ? 0x7fffffff : n));
    return r < 0 ? fail_crt() : r;
}
WINAPK_GUEST_ABI int64_t g_lseek(int fd, int64_t off, int whence) {
    if (fd_object(fd)) {
        *guest_errno() = kESPIPE;
        return -1;
    }
    const int64_t r = _lseeki64(fd, off, whence);
    return r < 0 ? fail_crt() : r;
}
// pread/pwrite at an offset without moving the file position.
WINAPK_GUEST_ABI int64_t g_pread(int fd, void* buf, size_t n, int64_t off) {
    const int64_t at = _lseeki64(fd, 0, SEEK_CUR);
    if (at < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return fail_crt();
    const int r = _read(fd, buf, static_cast<unsigned>(n));
    const int e = errno;
    _lseeki64(fd, at, SEEK_SET);
    if (r < 0) {
        set_errno_from_crt(e);
        return -1;
    }
    return r;
}
WINAPK_GUEST_ABI int64_t g_pwrite(int fd, const void* buf, size_t n, int64_t off) {
    const int64_t at = _lseeki64(fd, 0, SEEK_CUR);
    if (at < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return fail_crt();
    const int r = _write(fd, buf, static_cast<unsigned>(n));
    const int e = errno;
    _lseeki64(fd, at, SEEK_SET);
    if (r < 0) {
        set_errno_from_crt(e);
        return -1;
    }
    return r;
}
WINAPK_GUEST_ABI int64_t g_read_chk(int fd, void* buf, size_t n, size_t bn) {
    if (n > bn) {
        std::fprintf(stderr, "[win.apk] guest FORTIFY: read: prevented buffer overflow\n");
        std::abort();
    }
    return g_read(fd, buf, n);
}
WINAPK_GUEST_ABI int64_t g_write_chk(int fd, const void* buf, size_t n, size_t bn) {
    if (n > bn) {
        std::fprintf(stderr, "[win.apk] guest FORTIFY: write: prevented buffer over-read\n");
        std::abort();
    }
    return g_write(fd, buf, n);
}
WINAPK_GUEST_ABI int g_dup(int fd) {
    const int o = fd_dup(fd, -1);
    if (o != -2) return o;
    const int r = _dup(fd);
    return r < 0 ? fail_crt() : r;
}
WINAPK_GUEST_ABI int g_dup2(int fd, int to) {
    const int o = fd_dup(fd, to);
    if (o != -2) return o;
    fd_close(to);  // a file replacing an object
    return _dup2(fd, to) < 0 ? fail_crt() : to;
}
WINAPK_GUEST_ABI int g_dup3(int fd, int to, int) { return g_dup2(fd, to); }
WINAPK_GUEST_ABI int g_fsync(int fd) { return _commit(fd) < 0 ? fail_crt() : 0; }
WINAPK_GUEST_ABI int g_ftruncate(int fd, int64_t len) {
    const errno_t e = _chsize_s(fd, len);
    if (e) {
        set_errno_from_crt(e);
        return -1;
    }
    return 0;
}
WINAPK_GUEST_ABI int g_isatty(int fd) { return !fd_object(fd) && _isatty(fd) ? 1 : 0; }
// fcntl: descriptor flags are accepted and ignored (no fork, so close-on-exec
// has nothing to protect); F_GETFL answers read-write.
WINAPK_GUEST_ABI int g_fcntl(int fd, int cmd, int64_t arg) {
    if (auto o = fd_object(fd)) {
        // O_NONBLOCK is the status flag that matters for objects.
        if (cmd == 3) return kORdWr | (o->nonblocking ? 04000 : 0);  // F_GETFL
        if (cmd == 4) {                                              // F_SETFL
            o->set_nonblocking((arg & 04000) != 0);
            return 0;
        }
    }
    switch (cmd) {
        case 0: return g_dup(fd);                      // F_DUPFD
        case 1030: return g_dup(fd);                   // F_DUPFD_CLOEXEC
        case 1: case 2: case 4: return 0;              // F_GETFD, F_SETFD, F_SETFL
        case 3: return kORdWr;                         // F_GETFL
        case 5: case 6: case 7: return 0;              // F_GETLK, F_SETLK, F_SETLKW
        default: (void)arg; *guest_errno() = kEINVAL; return -1;
    }
}
WINAPK_GUEST_ABI int g_ioctl(int fd, int request, void* arg) {
    if (auto o = fd_object(fd)) {
        if (request == 0x5421) {  // FIONBIO
            o->set_nonblocking(*static_cast<int*>(arg) != 0);
            return 0;
        }
        return o->ioctl(request, arg);
    }
    *guest_errno() = 25;  // ENOTTY
    return -1;
}

// ---- stat ----------------------------------------------------------------------------
// Bionic's struct stat on x86_64.
struct GuestStat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid;
    int32_t pad0;
    uint64_t st_rdev;
    int64_t st_size, st_blksize, st_blocks;
    int64_t st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec;
    int64_t unused[3];
};
static_assert(sizeof(GuestStat) == 144, "Bionic's struct stat on x86_64");

void fill_stat(const struct _stat64& s, GuestStat* g) {
    std::memset(g, 0, sizeof(*g));
    const bool dir = (s.st_mode & _S_IFMT) == _S_IFDIR;
    g->st_dev = s.st_dev;
    g->st_ino = s.st_ino;
    g->st_nlink = s.st_nlink ? s.st_nlink : 1;
    g->st_mode = dir ? (0040000 | 0771) : (0100000 | ((s.st_mode & _S_IWRITE) ? 0660 : 0440));
    g->st_size = s.st_size;
    g->st_blksize = 4096;
    g->st_blocks = (s.st_size + 511) / 512;
    g->st_atime_sec = s.st_atime;
    g->st_mtime_sec = s.st_mtime;
    g->st_ctime_sec = s.st_ctime;
}
WINAPK_GUEST_ABI int g_stat(const char* path, GuestStat* g) {
    struct _stat64 s;
    if (_wstat64(host_path(path).c_str(), &s) < 0) return fail_crt();
    fill_stat(s, g);
    return 0;
}
WINAPK_GUEST_ABI int g_fstat(int fd, GuestStat* g) {
    if (auto o = fd_object(fd)) {
        std::memset(g, 0, sizeof(*g));
        g->st_mode = o->mode() | 0600;
        g->st_nlink = 1;
        g->st_blksize = 4096;
        return 0;
    }
    struct _stat64 s;
    if (_fstat64(fd, &s) < 0) return fail_crt();
    fill_stat(s, g);
    return 0;
}
// memfd_create: an anonymous file. A temporary file deleted on close behaves
// the same for its uses (ftruncate, read/write, mmap of a private copy).
WINAPK_GUEST_ABI int g_memfd_create(const char*, unsigned) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"mfd", 0, path)) {
        set_errno_from_win32(GetLastError());
        return -1;
    }
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        set_errno_from_win32(GetLastError());
        return -1;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDWR | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        return fail_crt();
    }
    return fd;
}

// flock: LOCK_SH 1, LOCK_EX 2, LOCK_NB 4, LOCK_UN 8, on the whole file.
WINAPK_GUEST_ABI int g_flock(int fd, int op) {
    const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
        *guest_errno() = kEBADF;
        return -1;
    }
    OVERLAPPED at{};
    if (op & 8) {
        UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &at);
        return 0;
    }
    DWORD flags = (op & 2) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (op & 4) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &at)) return 0;
    const DWORD e = GetLastError();
    if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) {
        *guest_errno() = kEAGAIN;  // EWOULDBLOCK
        return -1;
    }
    set_errno_from_win32(e);
    return -1;
}

// Bionic's struct statvfs (x86_64): eleven unsigned longs, then spare ints.
struct GuestStatvfs {
    uint64_t f_bsize, f_frsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
    uint32_t spare[6];
};
static_assert(sizeof(GuestStatvfs) == 112, "Bionic's struct statvfs on x86_64");
int statvfs_of(const std::wstring& path, GuestStatvfs* out) {
    ULARGE_INTEGER avail, total, free_bytes;
    BOOL ok = GetDiskFreeSpaceExW(path.c_str(), &avail, &total, &free_bytes);
    DWORD error = ok ? 0 : GetLastError();
    // GetDiskFreeSpaceExW takes directories only; Linux answers for the
    // filesystem of any path, a file's included.
    wchar_t root[MAX_PATH];
    if (!ok && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetVolumePathNameW(path.c_str(), root, MAX_PATH)) {
        ok = GetDiskFreeSpaceExW(root, &avail, &total, &free_bytes);
        if (!ok) error = GetLastError();
    }
    if (!ok) {
        set_errno_from_win32(error);
        return -1;
    }
    std::memset(out, 0, sizeof(*out));
    out->f_bsize = out->f_frsize = 4096;
    out->f_blocks = total.QuadPart / 4096;
    out->f_bfree = free_bytes.QuadPart / 4096;
    out->f_bavail = avail.QuadPart / 4096;
    out->f_files = out->f_ffree = out->f_favail = 1u << 20;  // NTFS has no inode limit to report
    out->f_namemax = 255;
    return 0;
}
WINAPK_GUEST_ABI int g_statvfs(const char* path, GuestStatvfs* out) { return statvfs_of(host_path(path), out); }
WINAPK_GUEST_ABI int g_fstatvfs(int fd, GuestStatvfs* out) {
    const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    wchar_t path[MAX_PATH + 8];
    if (h == INVALID_HANDLE_VALUE || !GetFinalPathNameByHandleW(h, path, MAX_PATH, FILE_NAME_NORMALIZED)) {
        *guest_errno() = kEBADF;
        return -1;
    }
    // \\?\C:\dir\file -> its volume root.
    wchar_t root[MAX_PATH];
    if (!GetVolumePathNameW(path, root, MAX_PATH)) {
        set_errno_from_win32(GetLastError());
        return -1;
    }
    return statvfs_of(root, out);
}

// Bionic's struct statfs on x86_64, which is also its struct statfs64: the
// kernel's, where every field is a 64-bit __kernel_long_t except f_fsid (two
// ints). Windows has no statfs, so it is filled from statvfs_of's answer:
//
//   field       offset  size   from
//   f_type         0     8     ext4's magic (below)
//   f_bsize        8     8     f_bsize
//   f_blocks      16     8     f_blocks
//   f_bfree       24     8     f_bfree
//   f_bavail      32     8     f_bavail
//   f_files       40     8     f_files
//   f_ffree       48     8     f_ffree
//   f_fsid        56     8     0 (int[2])
//   f_namelen     64     8     f_namemax
//   f_frsize      72     8     f_frsize
//   f_flags       80     8     /data's mount flags (below)
//   f_spare       88    32     0 (long[4])
//   sizeof       120
//
// f_type and f_flags are what an Android /data reports, as upstream answers:
// EXT4_SUPER_MAGIC 0xef53, and ST_VALID | ST_NOSUID | ST_NODEV | ST_NOATIME.
struct GuestStatfs {
    uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
};
static_assert(sizeof(GuestStatfs) == 120 && offsetof(GuestStatfs, f_fsid) == 56 &&
                  offsetof(GuestStatfs, f_namelen) == 64 && offsetof(GuestStatfs, f_spare) == 88,
              "Bionic's struct statfs on x86_64");
int to_statfs(const GuestStatvfs& v, GuestStatfs* out) {
    std::memset(out, 0, sizeof(*out));
    out->f_type = 0xef53;
    out->f_bsize = v.f_bsize;
    out->f_blocks = v.f_blocks;
    out->f_bfree = v.f_bfree;
    out->f_bavail = v.f_bavail;
    out->f_files = v.f_files;
    out->f_ffree = v.f_ffree;
    out->f_namelen = v.f_namemax;
    out->f_frsize = v.f_frsize;
    out->f_flags = 0x0426;
    return 0;
}
WINAPK_GUEST_ABI int g_statfs(const char* path, GuestStatfs* out) {
    GuestStatvfs v;
    return statvfs_of(host_path(path), &v) < 0 ? -1 : to_statfs(v, out);
}
WINAPK_GUEST_ABI int g_fstatfs(int fd, GuestStatfs* out) {
    GuestStatvfs v;
    return g_fstatvfs(fd, &v) < 0 ? -1 : to_statfs(v, out);
}

// AT_FDCWD (-100) with a path, the common case of the *at() calls.
WINAPK_GUEST_ABI int g_fstatat(int dirfd, const char* path, GuestStat* g, int) {
    if (dirfd != -100 && path && path[0] != '/' && !(path[0] && path[1] == ':')) {
        *guest_errno() = kENOSYS;
        return -1;
    }
    return g_stat(path, g);
}
WINAPK_GUEST_ABI int g_access(const char* path, int mode) {
    // _waccess knows no X_OK (1); anything readable counts.
    return _waccess(host_path(path).c_str(), mode & 6) < 0 ? fail_crt() : 0;
}
WINAPK_GUEST_ABI int g_faccessat(int, const char* path, int mode, int) { return g_access(path, mode); }

// ---- the file system -------------------------------------------------------------------
WINAPK_GUEST_ABI int g_mkdir(const char* path, unsigned) { return _wmkdir(host_path(path).c_str()) < 0 ? fail_crt() : 0; }
WINAPK_GUEST_ABI int g_rmdir(const char* path) { return _wrmdir(host_path(path).c_str()) < 0 ? fail_crt() : 0; }
WINAPK_GUEST_ABI int g_unlink(const char* path) {
    // POSIX unlinks an open file; Windows needs it shared for delete, which
    // the CRT does not ask for, so fall back to delete-on-close semantics.
    const std::wstring w = host_path(path);
    if (DeleteFileW(w.c_str())) return 0;
    set_errno_from_win32(GetLastError());
    return -1;
}
WINAPK_GUEST_ABI int g_rename(const char* from, const char* to) {
    if (MoveFileExW(host_path(from).c_str(), host_path(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    set_errno_from_win32(GetLastError());
    return -1;
}
WINAPK_GUEST_ABI int g_remove(const char* path) {
    const std::wstring w = host_path(path);
    const DWORD a = GetFileAttributesW(w.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return g_rmdir(path);
    return g_unlink(path);
}
WINAPK_GUEST_ABI int g_chdir(const char* path) { return _wchdir(host_path(path).c_str()) < 0 ? fail_crt() : 0; }
WINAPK_GUEST_ABI char* g_getcwd(char* buf, size_t n) {
    wchar_t w[MAX_PATH * 4];
    if (!_wgetcwd(w, MAX_PATH * 4)) {
        fail_crt();
        return nullptr;
    }
    // POSIX code takes a directory that does not start with '/' for a relative
    // one and resolves it against getcwd() again: JUCE's File did that until
    // the stack overflowed (Spotify's libcomScore). So the directory is rooted,
    // "/C:/dir", which host_path() reads back as "C:/dir".
    std::string s = narrow(w);
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    if (s[0] != '/') s.insert(0, 1, '/');
    if (!buf) return _strdup(s.c_str());
    if (s.size() + 1 > n) {
        *guest_errno() = kERANGE;
        return nullptr;
    }
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return buf;
}
WINAPK_GUEST_ABI char* g_realpath(const char* path, char* out) {
    wchar_t full[MAX_PATH * 4];
    if (!GetFullPathNameW(host_path(path).c_str(), MAX_PATH * 4, full, nullptr)) {
        set_errno_from_win32(GetLastError());
        return nullptr;
    }
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        set_errno_from_win32(GetLastError());
        return nullptr;
    }
    const std::string s = narrow(full);
    if (!out) return _strdup(s.c_str());
    std::strncpy(out, s.c_str(), 4095);
    out[4095] = '\0';
    return out;
}
WINAPK_GUEST_ABI int g_chmod(const char*, unsigned) { return 0; }
WINAPK_GUEST_ABI int g_fchmod(int, unsigned) { return 0; }
WINAPK_GUEST_ABI unsigned g_umask(unsigned) { return 022; }

// ---- directories -------------------------------------------------------------------------
// Bionic's struct dirent on x86_64.
struct GuestDirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

struct GuestDir {
    HANDLE find = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW data;
    bool pending = false;  // `data` holds an entry not handed out yet
    int64_t offset = 0;
    GuestDirent entry;
};

WINAPK_GUEST_ABI GuestDir* g_opendir(const char* path) {
    std::wstring pattern = host_path(path);
    if (!pattern.empty() && pattern.back() != L'/' && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    auto* d = new GuestDir();
    d->find = FindFirstFileW(pattern.c_str(), &d->data);
    if (d->find == INVALID_HANDLE_VALUE) {
        set_errno_from_win32(GetLastError());
        delete d;
        return nullptr;
    }
    d->pending = true;
    return d;
}
WINAPK_GUEST_ABI GuestDirent* g_readdir(GuestDir* d) {
    if (!d) return nullptr;
    if (!d->pending) {
        if (!FindNextFileW(d->find, &d->data)) return nullptr;  // end: errno untouched
    }
    d->pending = false;
    std::memset(&d->entry, 0, sizeof(d->entry));
    const std::string name = narrow(d->data.cFileName);
    std::strncpy(d->entry.d_name, name.c_str(), sizeof(d->entry.d_name) - 1);
    d->entry.d_ino = static_cast<uint64_t>(++d->offset);
    d->entry.d_off = d->offset;
    d->entry.d_reclen = sizeof(GuestDirent);
    d->entry.d_type = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;  // DT_DIR, DT_REG
    return &d->entry;
}
WINAPK_GUEST_ABI int g_closedir(GuestDir* d) {
    if (!d) return -1;
    if (d->find != INVALID_HANDLE_VALUE) FindClose(d->find);
    delete d;
    return 0;
}
WINAPK_GUEST_ABI void g_rewinddir(GuestDir*) {}

// ---- FILE streams ----------------------------------------------------------------------------
// Bionic's mode letters, less "e" (O_CLOEXEC); the CRT stream is always binary,
// as Linux's are.
std::wstring crt_mode(const char* mode) {
    std::wstring m;
    for (const char* p = mode; p && *p; ++p) {
        if (*p == 'e' || *p == 'b') continue;
        m.push_back(static_cast<wchar_t>(*p));
    }
    m.push_back(L'b');
    return m;
}

// fopen("/dev/urandom"): a guest FILE* is the CRT's, which cannot wrap an
// FdObject, so hand out a delete-on-close temporary file pre-filled from the
// system RNG. Entropy readers (mbedtls' platform poll, in WhatsApp's msys
// networking) open, read a few dozen bytes and close; without this their DRBG
// seed failed (-52) and the process aborted.
FILE* open_random_stream() {
    wchar_t dir[MAX_PATH + 1], name[MAX_PATH + 1];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"rnd", 0, name)) {
        *guest_errno() = 5;  // EIO
        return nullptr;
    }
    FILE* f = _wfopen(name, L"w+bD");  // D: deleted when closed
    if (!f) {
        set_errno_from_crt(errno);
        DeleteFileW(name);
        return nullptr;
    }
    static constexpr size_t kBytes = 64 * 1024;
    std::unique_ptr<unsigned char[]> buf(new unsigned char[kBytes]);
    BCryptGenRandom(nullptr, buf.get(), static_cast<ULONG>(kBytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::fwrite(buf.get(), 1, kBytes, f);
    std::rewind(f);
    return f;
}
WINAPK_GUEST_ABI FILE* g_fopen(const char* path, const char* mode) {
    if (is_random_device(path)) return open_random_stream();
    if (is_proc_maps(path)) {
        const int fd = open_temp_with(proc_maps_text());
        if (fd < 0) return nullptr;
        FILE* f = _fdopen(fd, "rb");
        if (!f) {
            set_errno_from_crt(errno);
            _close(fd);
        }
        return f;
    }
    FILE* f = _wfopen(host_path(path).c_str(), crt_mode(mode).c_str());
    if (!f) set_errno_from_crt(errno);
    return f;
}
WINAPK_GUEST_ABI FILE* g_fdopen(int fd, const char* mode) {
    std::string m;
    for (const wchar_t c : crt_mode(mode)) m.push_back(static_cast<char>(c));
    FILE* f = _fdopen(fd, m.c_str());
    if (!f) set_errno_from_crt(errno);
    return f;
}
WINAPK_GUEST_ABI FILE* g_freopen(const char* path, const char* mode, void* f) {
    FILE* r = _wfreopen(host_path(path).c_str(), crt_mode(mode).c_str(), host_file(f));
    if (!r) set_errno_from_crt(errno);
    return r;
}
WINAPK_GUEST_ABI int g_fclose(void* f) { return std::fclose(host_file(f)) ? fail_crt() : 0; }
WINAPK_GUEST_ABI size_t g_fread(void* buf, size_t sz, size_t n, void* f) { return std::fread(buf, sz, n, host_file(f)); }
WINAPK_GUEST_ABI size_t g_fwrite(const void* buf, size_t sz, size_t n, void* f) {
    return std::fwrite(buf, sz, n, host_file(f));
}
WINAPK_GUEST_ABI int g_fputs(const char* s, void* f) { return std::fputs(s, host_file(f)); }
WINAPK_GUEST_ABI int g_fputc(int c, void* f) { return std::fputc(c, host_file(f)); }
WINAPK_GUEST_ABI int g_putchar(int c) { return std::fputc(c, stdout); }
WINAPK_GUEST_ABI char* g_fgets(char* s, int n, void* f) { return std::fgets(s, n, host_file(f)); }
WINAPK_GUEST_ABI char* g_fgets_chk(char* s, int n, void* f, size_t bn) {
    if (n < 0 || static_cast<size_t>(n) > bn) {
        std::fprintf(stderr, "[win.apk] guest FORTIFY: fgets: prevented buffer overflow\n");
        std::abort();
    }
    return g_fgets(s, n, f);
}
WINAPK_GUEST_ABI int g_fgetc(void* f) { return std::fgetc(host_file(f)); }
WINAPK_GUEST_ABI int g_ungetc(int c, void* f) { return std::ungetc(c, host_file(f)); }
WINAPK_GUEST_ABI int g_fflush(void* f) { return std::fflush(f ? host_file(f) : nullptr); }
WINAPK_GUEST_ABI int g_fseek(void* f, glong off, int whence) { return _fseeki64(host_file(f), off, whence) ? fail_crt() : 0; }
WINAPK_GUEST_ABI glong g_ftell(void* f) { return _ftelli64(host_file(f)); }
WINAPK_GUEST_ABI void g_rewind(void* f) { std::rewind(host_file(f)); }
WINAPK_GUEST_ABI int g_feof(void* f) { return std::feof(host_file(f)); }
WINAPK_GUEST_ABI int g_ferror(void* f) { return std::ferror(host_file(f)); }
WINAPK_GUEST_ABI void g_clearerr(void* f) { std::clearerr(host_file(f)); }
WINAPK_GUEST_ABI int g_fileno(void* f) { return _fileno(host_file(f)); }
WINAPK_GUEST_ABI int g_setvbuf(void* f, char* buf, int mode, size_t n) {
    // Linux: _IOFBF 0, _IOLBF 1, _IONBF 2.
    const int crt = mode == 2 ? _IONBF : mode == 1 ? _IOLBF : _IOFBF;
    return std::setvbuf(host_file(f), buf, crt, (crt == _IONBF || n >= 2) ? n : 2);
}
WINAPK_GUEST_ABI void g_setbuf(void* f, char* buf) { std::setbuf(host_file(f), buf); }
WINAPK_GUEST_ABI FILE* g_tmpfile() { return std::tmpfile(); }
WINAPK_GUEST_ABI void g_perror(const char* s) {
    std::fprintf(stderr, "%s%s%s\n", s ? s : "", s && *s ? ": " : "", errno_text(*guest_errno()));
}
// getline / getdelim: grow *line with the guest's allocator (the CRT's).
WINAPK_GUEST_ABI int64_t g_getdelim(char** line, size_t* cap, int delim, void* f) {
    FILE* h = host_file(f);
    size_t n = 0;
    for (;;) {
        const int c = std::fgetc(h);
        if (c == EOF) break;
        if (!*line || n + 2 > *cap) {
            const size_t grow = *cap ? *cap * 2 : 128;
            char* p = static_cast<char*>(std::realloc(*line, grow));
            if (!p) {
                *guest_errno() = kENOMEM;
                return -1;
            }
            *line = p;
            *cap = grow;
        }
        (*line)[n++] = static_cast<char>(c);
        if (c == delim) break;
    }
    if (n == 0) return -1;
    (*line)[n] = '\0';
    return static_cast<int64_t>(n);
}
WINAPK_GUEST_ABI int64_t g_getline(char** line, size_t* cap, void* f) { return g_getdelim(line, cap, '\n', f); }

const ShimSymbol kSymbols[] = {
    {"__sF", g_sF},
    {"stdin", &g_stdin_var},
    {"stdout", &g_stdout_var},
    {"stderr", &g_stderr_var},
    WINAPK_SHIM("open", g_open),
    WINAPK_SHIM("open64", g_open),
    WINAPK_SHIM("__open_2", g_open_2),
    WINAPK_SHIM("creat", g_creat),
    WINAPK_SHIM("close", g_close),
    WINAPK_SHIM("read", g_read),
    WINAPK_SHIM("__read_chk", g_read_chk),
    WINAPK_SHIM("__write_chk", g_write_chk),
    WINAPK_SHIM("write", g_write),
    WINAPK_SHIM("lseek", g_lseek),
    WINAPK_SHIM("lseek64", g_lseek),
    WINAPK_SHIM("pread", g_pread),
    WINAPK_SHIM("pread64", g_pread),
    WINAPK_SHIM("pwrite", g_pwrite),
    WINAPK_SHIM("pwrite64", g_pwrite),
    WINAPK_SHIM("dup", g_dup),
    WINAPK_SHIM("dup2", g_dup2),
    WINAPK_SHIM("dup3", g_dup3),
    WINAPK_SHIM("fsync", g_fsync),
    WINAPK_SHIM("fdatasync", g_fsync),
    WINAPK_SHIM("ftruncate", g_ftruncate),
    WINAPK_SHIM("ftruncate64", g_ftruncate),
    WINAPK_SHIM("isatty", g_isatty),
    WINAPK_SHIM("fcntl", g_fcntl),
    WINAPK_SHIM("ioctl", g_ioctl),
    WINAPK_SHIM("stat", g_stat),
    WINAPK_SHIM("stat64", g_stat),
    WINAPK_SHIM("lstat", g_stat),
    WINAPK_SHIM("lstat64", g_stat),
    WINAPK_SHIM("fstat", g_fstat),
    WINAPK_SHIM("fstat64", g_fstat),
    WINAPK_SHIM("fstatat", g_fstatat),
    WINAPK_SHIM("memfd_create", g_memfd_create),
    WINAPK_SHIM("flock", g_flock),
    WINAPK_SHIM("statvfs", g_statvfs),
    WINAPK_SHIM("fstatvfs", g_fstatvfs),
    WINAPK_SHIM("statfs", g_statfs),
    WINAPK_SHIM("statfs64", g_statfs),
    WINAPK_SHIM("fstatfs", g_fstatfs),
    WINAPK_SHIM("fstatfs64", g_fstatfs),
    WINAPK_SHIM("fstatat64", g_fstatat),
    WINAPK_SHIM("access", g_access),
    WINAPK_SHIM("faccessat", g_faccessat),
    WINAPK_SHIM("mkdir", g_mkdir),
    WINAPK_SHIM("rmdir", g_rmdir),
    WINAPK_SHIM("unlink", g_unlink),
    WINAPK_SHIM("rename", g_rename),
    WINAPK_SHIM("remove", g_remove),
    WINAPK_SHIM("chdir", g_chdir),
    WINAPK_SHIM("getcwd", g_getcwd),
    WINAPK_SHIM("realpath", g_realpath),
    WINAPK_SHIM("chmod", g_chmod),
    WINAPK_SHIM("fchmod", g_fchmod),
    WINAPK_SHIM("umask", g_umask),
    WINAPK_SHIM("opendir", g_opendir),
    WINAPK_SHIM("readdir", g_readdir),
    WINAPK_SHIM("readdir64", g_readdir),
    WINAPK_SHIM("closedir", g_closedir),
    WINAPK_SHIM("rewinddir", g_rewinddir),
    WINAPK_SHIM("fopen", g_fopen),
    WINAPK_SHIM("fopen64", g_fopen),
    WINAPK_SHIM("fdopen", g_fdopen),
    WINAPK_SHIM("freopen", g_freopen),
    WINAPK_SHIM("fclose", g_fclose),
    WINAPK_SHIM("fread", g_fread),
    WINAPK_SHIM("fwrite", g_fwrite),
    WINAPK_SHIM("fputs", g_fputs),
    WINAPK_SHIM("fputc", g_fputc),
    WINAPK_SHIM("putc", g_fputc),
    WINAPK_SHIM("putchar", g_putchar),
    WINAPK_SHIM("fgets", g_fgets),
    WINAPK_SHIM("__fgets_chk", g_fgets_chk),
    WINAPK_SHIM("fgetc", g_fgetc),
    WINAPK_SHIM("getc", g_fgetc),
    WINAPK_SHIM("ungetc", g_ungetc),
    WINAPK_SHIM("fflush", g_fflush),
    WINAPK_SHIM("fseek", g_fseek),
    WINAPK_SHIM("fseeko", g_fseek),
    WINAPK_SHIM("fseeko64", g_fseek),
    WINAPK_SHIM("ftell", g_ftell),
    WINAPK_SHIM("ftello", g_ftell),
    WINAPK_SHIM("ftello64", g_ftell),
    WINAPK_SHIM("rewind", g_rewind),
    WINAPK_SHIM("feof", g_feof),
    WINAPK_SHIM("ferror", g_ferror),
    WINAPK_SHIM("clearerr", g_clearerr),
    WINAPK_SHIM("fileno", g_fileno),
    WINAPK_SHIM("setvbuf", g_setvbuf),
    WINAPK_SHIM("setbuf", g_setbuf),
    WINAPK_SHIM("tmpfile", g_tmpfile),
    WINAPK_SHIM("perror", g_perror),
    WINAPK_SHIM("getdelim", g_getdelim),
    WINAPK_SHIM("getline", g_getline),
};

}  // namespace

size_t file_write(void* f, const char* data, size_t n) { return std::fwrite(data, 1, n, host_file(f)); }
int64_t fd_write(int fd, const void* data, size_t n) { return g_write(fd, data, n); }
void* guest_stdout() { return stdout; }
void* guest_stderr() { return stderr; }

ShimTable stdio_symbols() { return WINAPK_SHIM_TABLE(kSymbols); }

}  // namespace winapk::bionic
