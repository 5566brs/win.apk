// linker.h -- the guest dynamic linker: loads Android x86_64 libraries together
// with their DT_NEEDED dependencies, keeps a registry of what is loaded, and
// implements the guest's libdl (dlopen, dlsym, dladdr, ...).
//
// ElfImage (elf_loader.h) loads one library. GuestLinker does what bionic's
// linker does around that:
//
//   * load: maps the whole DT_NEEDED tree breadth-first, links every new
//     library, then runs constructors dependencies first. JNI_OnLoad is never
//     called here; on Android the dynamic linker does not call it either
//     (System.loadLibrary does, for the library it was asked for only).
//   * system libraries: a DT_NEEDED or dlopen name from the NDK's public set
//     (libc.so, liblog.so, libGLESv2.so, ...) is not a file. It is answered by
//     the host shim through LinkerConfig::host.
//   * symbol lookup when linking a new library, in bionic's order: the global
//     group first (the host shim, standing in for app_process and its libc,
//     then libraries opened with RTLD_GLOBAL), then the local group (the
//     dlopen'ed root and its dependency tree, breadth-first). A library's own
//     definitions are bound directly (ElfImage does not support interposition
//     of a symbol the library defines itself).
//   * dlsym(handle) walks the handle's dependency tree breadth-first, so a
//     dependency's Java_* symbols are found through the library that needs it.
//     The addresses it returns are raw System V addresses: a guest that calls
//     another library's JNI_OnLoad itself (Unity's libmain does) gets the real
//     function.
//
// Not done yet:
//   * dlclose never unloads. Libraries stay mapped until the process exits,
//     as if every library were RTLD_NODELETE.
//   * linker namespaces and android_dlopen_ext's extinfo (loading from an fd
//     or from inside the APK) are ignored.

#pragma once

#include "winapk/elf_loader.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace winapk {

struct LinkerConfig {
    // Where a bare library name ("libfoo.so") is looked for, in order: the
    // app's native library directory. A dependency is also looked for next to
    // the library that needs it.
    std::vector<std::filesystem::path> search_paths;
    // The host shim: libc, libm, liblog and the other system libraries. Must
    // return System V callable addresses (or data addresses).
    SymbolResolver host;
    LoadOptions load;
};

// One loaded library, or a system library answered by the host. Its address is
// the handle the guest's dlopen returns.
struct GuestLibrary;

class GuestLinker {
public:
    // Bionic's dlopen flags (LP64 values).
    static constexpr int kRtldLazy = 0x1;
    static constexpr int kRtldNow = 0x2;
    static constexpr int kRtldNoLoad = 0x4;
    static constexpr int kRtldGlobal = 0x100;
    static constexpr int kRtldNoDelete = 0x1000;

    // The guest's libdl functions act on the process linker. Constructing a
    // GuestLinker makes it the process linker; there can be one at a time.
    explicit GuestLinker(LinkerConfig config);
    ~GuestLinker();  // runs finalizers, last-initialized first

    GuestLinker(const GuestLinker&) = delete;
    GuestLinker& operator=(const GuestLinker&) = delete;

    static GuestLinker* process();

    // Loads `name` and its dependencies, or returns the library if it is
    // already loaded (matched by path, soname or file name). `name` is UTF-8:
    // a path if it contains a slash or a backslash, else a file name looked up
    // in the search paths. RTLD_NOLOAD and RTLD_GLOBAL are honored. On failure
    // returns nullptr and sets `error` to a bionic-style message.
    GuestLibrary* open(const std::string& name, int flags, std::string* error);

    // What dlopen(nullptr) returns: a handle that searches like RTLD_DEFAULT.
    void* main_handle() const { return main_.get(); }

    // dlsym semantics. `handle` is a GuestLibrary*, the dlopen(nullptr)
    // handle, RTLD_DEFAULT (nullptr) or RTLD_NEXT ((void*)-1, needs `caller`).
    void* lookup(void* handle, const char* name, const void* caller, std::string* error);

    // The image a guest address belongs to, or nullptr for any other address
    // (host code, the JVM, thunks). This is the registry the JNI binding code
    // uses to tell guest functions apart.
    const ElfImage* image_containing(const void* addr) const;

    // The image behind a handle from open(), or nullptr for a system library.
    static const ElfImage* image_of(const GuestLibrary* lib);

    // Loaded images in load order.
    std::vector<const ElfImage*> images() const;

    // What dladdr reports. Returns false for an address outside guest images.
    struct AddrInfo {
        const char* file;    // UTF-8 path; valid as long as the library is loaded
        const void* base;
        const char* symbol;  // nullptr if no symbol covers the address
        void* symbol_addr;
    };
    bool describe(const void* addr, AddrInfo* info) const;

    // Resolves a name for guest code being linked: the libdl functions first,
    // then the host shim. Exposed for loaders that do not go through open().
    void* resolve_host(const char* name) const;

private:
    GuestLibrary* find_loaded(const std::string& name) const;
    std::filesystem::path locate(const std::string& name, const std::filesystem::path& near) const;
    std::vector<GuestLibrary*> local_group(GuestLibrary* root) const;
    void* find_in(GuestLibrary* lib, const char* name) const;
    void run_constructors(GuestLibrary* lib);
    bool is_handle(const void* handle) const;

    LinkerConfig config_;
    mutable std::recursive_mutex lock_;  // constructors may call dlopen
    std::vector<std::unique_ptr<GuestLibrary>> libs_;  // load order
    std::vector<GuestLibrary*> init_order_;
    std::unique_ptr<GuestLibrary> system_;  // every system library
    std::unique_ptr<GuestLibrary> main_;    // dlopen(nullptr)
};

// Host address of a libdl function (dlopen, dlsym, dlclose, dlerror, dladdr,
// android_dlopen_ext) for guest code, or nullptr.
void* guest_libdl_resolve(const char* name);

}  // namespace winapk
