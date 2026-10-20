// linker.cpp -- see linker.h.

#include "winapk/linker.h"

#include "winapk/guest_abi.h"

#include <cstring>
#include <unordered_set>

namespace winapk {

struct GuestLibrary {
    std::unique_ptr<ElfImage> image;  // null for the system and main handles
    std::string name;                 // soname, else the file name
    std::string file;                 // UTF-8 path, for dladdr and error messages
    std::filesystem::path path;
    std::vector<GuestLibrary*> needed;  // direct DT_NEEDED, in order
    bool global = false;
    bool init_started = false;  // guards constructor recursion on cycles
};

namespace {

GuestLinker* g_process_linker = nullptr;

// The libraries an app can link against without shipping them: the NDK's
// stable APIs plus the rest of /system/etc/public.libraries.txt. Same list as
// upstream's is_system_lib (runtime/translator/src/bionic_shim_arm32_link.cpp).
bool is_system_library(const std::string& file_name) {
    static const std::unordered_set<std::string> k = {
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so",
        "libandroid.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so",
        "libGLESv3.so", "libOpenSLES.so", "libOpenMAXAL.so", "libmediandk.so",
        "libjnigraphics.so", "libvulkan.so", "libaaudio.so", "libamidi.so",
        "libnativewindow.so", "libstdc++.so", "libcamera2ndk.so",
        "libneuralnetworks.so", "libsync.so", "libbinder_ndk.so", "libicu.so",
    };
    return k.count(file_name) != 0;
}

bool has_separator(const std::string& name) {
    return name.find('/') != std::string::npos || name.find('\\') != std::string::npos;
}

std::string file_name_of(const std::string& name) {
    const size_t slash = name.find_last_of("/\\");
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

// Paths from guest code are UTF-8. A plain std::string would be read in the
// ANSI code page on Windows.
std::filesystem::path utf8_path(const std::string& s) { return std::filesystem::u8path(s); }

std::string normalized(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
    return (ec ? p : c).lexically_normal().u8string();
}

}  // namespace

GuestLinker::GuestLinker(LinkerConfig config)
    : config_(std::move(config)), system_(new GuestLibrary()), main_(new GuestLibrary()) {
    system_->name = "<system>";
    main_->name = "<main>";
    g_process_linker = this;
}

GuestLinker::~GuestLinker() {
    std::lock_guard<std::recursive_mutex> hold(lock_);
    for (auto it = init_order_.rbegin(); it != init_order_.rend(); ++it) (*it)->image.reset();
    libs_.clear();
    if (g_process_linker == this) g_process_linker = nullptr;
}

GuestLinker* GuestLinker::process() { return g_process_linker; }

const ElfImage* GuestLinker::image_of(const GuestLibrary* lib) { return lib ? lib->image.get() : nullptr; }

GuestLibrary* GuestLinker::find_loaded(const std::string& name) const {
    if (has_separator(name)) {
        const std::string want = normalized(utf8_path(name));
        for (const auto& l : libs_) {
            if (normalized(l->path) == want) return l.get();
        }
        return nullptr;
    }
    for (const auto& l : libs_) {
        if (l->name == name || file_name_of(l->file) == name) return l.get();
    }
    return nullptr;
}

std::filesystem::path GuestLinker::locate(const std::string& name, const std::filesystem::path& near) const {
    std::error_code ec;
    if (has_separator(name)) {
        std::filesystem::path p = utf8_path(name);
        return std::filesystem::is_regular_file(p, ec) ? p : std::filesystem::path();
    }
    const std::filesystem::path file = utf8_path(name);
    for (const auto& dir : config_.search_paths) {
        if (std::filesystem::is_regular_file(dir / file, ec)) return dir / file;
    }
    if (!near.empty() && std::filesystem::is_regular_file(near / file, ec)) return near / file;
    return {};
}

std::vector<GuestLibrary*> GuestLinker::local_group(GuestLibrary* root) const {
    std::vector<GuestLibrary*> order{root};
    std::unordered_set<GuestLibrary*> seen{root};
    for (size_t i = 0; i < order.size(); ++i) {
        for (GuestLibrary* dep : order[i]->needed) {
            if (seen.insert(dep).second) order.push_back(dep);
        }
    }
    return order;
}

void* GuestLinker::resolve_host(const char* name) const {
    if (void* p = guest_libdl_resolve(name)) return p;
    return config_.host ? config_.host(name) : nullptr;
}

// A symbol as seen from inside one library of a tree: the system node stands
// for every system library, so it answers from the host.
void* GuestLinker::find_in(GuestLibrary* lib, const char* name) const {
    if (lib == system_.get()) return resolve_host(name);
    return lib->image ? lib->image->dlsym(name) : nullptr;
}

GuestLibrary* GuestLinker::open(const std::string& name, int flags, std::string* error) {
    std::lock_guard<std::recursive_mutex> hold(lock_);

    if (GuestLibrary* l = find_loaded(name)) {
        if (flags & kRtldGlobal) l->global = true;
        return l;
    }
    if (is_system_library(file_name_of(name))) return system_.get();
    if (flags & kRtldNoLoad) {
        *error = "library \"" + name + "\" wasn't loaded and RTLD_NOLOAD prevented it";
        return nullptr;
    }

    // Phase 1: map the new part of the tree, breadth-first. Nothing is linked
    // yet, so on failure the new images are simply dropped.
    std::vector<std::unique_ptr<GuestLibrary>> fresh;
    auto map_one = [&](const std::string& want, const GuestLibrary* needed_by) -> GuestLibrary* {
        const std::filesystem::path near = needed_by ? needed_by->path.parent_path() : std::filesystem::path();
        const std::filesystem::path path = locate(want, near);
        if (path.empty()) {
            *error = "library \"" + want + "\" not found";
            if (needed_by) *error += ": needed by " + needed_by->file;
            return nullptr;
        }
        std::string why;
        std::unique_ptr<ElfImage> img = ElfImage::map(path, &why, config_.load);
        if (!img) {
            *error = "cannot load \"" + path.u8string() + "\": " + why;
            return nullptr;
        }
        auto lib = std::make_unique<GuestLibrary>();
        lib->path = path;
        lib->file = path.u8string();
        lib->name = img->soname().empty() ? file_name_of(lib->file) : img->soname();
        lib->image = std::move(img);
        fresh.push_back(std::move(lib));
        return fresh.back().get();
    };
    auto find_fresh = [&](const std::string& want) -> GuestLibrary* {
        for (const auto& l : fresh) {
            if (l->name == want || file_name_of(l->file) == file_name_of(want)) return l.get();
        }
        return nullptr;
    };

    GuestLibrary* root = map_one(name, nullptr);
    if (!root) return nullptr;
    for (size_t i = 0; i < fresh.size(); ++i) {
        GuestLibrary* lib = fresh[i].get();
        for (const std::string& dep : lib->image->needed()) {
            GuestLibrary* d = nullptr;
            if (is_system_library(file_name_of(dep))) {
                d = system_.get();
            } else if (!(d = find_loaded(dep)) && !(d = find_fresh(dep))) {
                d = map_one(dep, lib);
                if (!d) return nullptr;
            }
            lib->needed.push_back(d);
        }
    }

    // Phase 2: link. Every new library resolves against the same scope: the
    // global group, then the root's tree.
    std::vector<GuestLibrary*> globals;
    for (const auto& l : libs_) {
        if (l->global) globals.push_back(l.get());
    }
    const std::vector<GuestLibrary*> group = local_group(root);
    SymbolResolver scope = [&](const char* sym) -> void* {
        if (void* p = resolve_host(sym)) return p;
        for (GuestLibrary* l : globals) {
            if (void* p = l->image->dlsym(sym)) return p;
        }
        for (GuestLibrary* l : group) {
            if (l != system_.get()) {
                if (void* p = l->image->dlsym(sym)) return p;
            }
        }
        return nullptr;
    };
    for (const auto& l : fresh) {
        std::string why;
        if (!l->image->link(scope, &why)) {
            *error = "cannot link \"" + l->file + "\": " + why;
            return nullptr;
        }
    }

    // Phase 3: register, then run constructors. Registration comes first so a
    // constructor that calls dlopen or dladdr on its own library sees it.
    if (flags & kRtldGlobal) root->global = true;
    for (auto& l : fresh) libs_.push_back(std::move(l));
    run_constructors(root);
    return root;
}

void GuestLinker::run_constructors(GuestLibrary* lib) {
    if (!lib->image || lib->init_started) return;
    lib->init_started = true;
    for (GuestLibrary* dep : lib->needed) run_constructors(dep);
    if (!lib->image->initialized()) {
        lib->image->initialize();
        init_order_.push_back(lib);
    }
}

bool GuestLinker::is_handle(const void* handle) const {
    if (handle == system_.get() || handle == main_.get()) return true;
    for (const auto& l : libs_) {
        if (l.get() == handle) return true;
    }
    return false;
}

void* GuestLinker::lookup(void* handle, const char* name, const void* caller, std::string* error) {
    std::lock_guard<std::recursive_mutex> hold(lock_);
    void* found = nullptr;
    if (handle == nullptr || handle == main_.get()) {
        // RTLD_DEFAULT and the main-program handle: the global group.
        found = resolve_host(name);
        for (size_t i = 0; !found && i < libs_.size(); ++i) {
            if (libs_[i]->global) found = libs_[i]->image->dlsym(name);
        }
    } else if (handle == reinterpret_cast<void*>(-1)) {
        // RTLD_NEXT: libraries loaded after the caller's, then the host. The
        // usual user is a wrapper that wants the libc function it shadows.
        size_t i = 0;
        while (i < libs_.size() && !libs_[i]->image->contains(caller)) ++i;
        for (++i; !found && i < libs_.size(); ++i) found = libs_[i]->image->dlsym(name);
        if (!found) found = resolve_host(name);
    } else if (!is_handle(handle)) {
        *error = "invalid handle";
        return nullptr;
    } else {
        for (GuestLibrary* l : local_group(static_cast<GuestLibrary*>(handle))) {
            if ((found = find_in(l, name))) break;
        }
    }
    if (!found) *error = std::string("undefined symbol: ") + name;
    return found;
}

const ElfImage* GuestLinker::image_containing(const void* addr) const {
    std::lock_guard<std::recursive_mutex> hold(lock_);
    for (const auto& l : libs_) {
        if (l->image->contains(addr)) return l->image.get();
    }
    return nullptr;
}

std::vector<const ElfImage*> GuestLinker::images() const {
    std::lock_guard<std::recursive_mutex> hold(lock_);
    std::vector<const ElfImage*> out;
    for (const auto& l : libs_) out.push_back(l->image.get());
    return out;
}

bool GuestLinker::describe(const void* addr, AddrInfo* info) const {
    std::lock_guard<std::recursive_mutex> hold(lock_);
    for (const auto& l : libs_) {
        if (!l->image->contains(addr)) continue;
        info->file = l->file.c_str();
        info->base = l->image->base();
        info->symbol = nullptr;
        info->symbol_addr = nullptr;
        l->image->symbol_at(addr, &info->symbol, &info->symbol_addr);
        return true;
    }
    return false;
}

// ---- the guest's libdl ------------------------------------------------------

namespace {

// dlerror() returns the last error once, then nullptr, per thread.
thread_local std::string t_dlerror;
thread_local bool t_dlerror_set = false;

void set_dlerror(const std::string& what, const std::string& detail) {
    t_dlerror = what + " failed: " + detail;
    t_dlerror_set = true;
}

// Bionic's Dl_info; the same layout as glibc's.
struct GuestDlInfo {
    const char* dli_fname;
    void* dli_fbase;
    const char* dli_sname;
    void* dli_saddr;
};

WINAPK_GUEST_ABI void* g_dlopen(const char* name, int flags) {
    GuestLinker* linker = GuestLinker::process();
    if (!linker) {
        set_dlerror("dlopen", "no guest linker");
        return nullptr;
    }
    if (!name) return linker->main_handle();
    std::string error;
    GuestLibrary* lib = linker->open(name, flags, &error);
    if (!lib) set_dlerror("dlopen", error);
    return lib;
}

// The extinfo (namespaces, loading from a file descriptor) is not supported;
// the plain dlopen behavior is what the common callers need.
WINAPK_GUEST_ABI void* g_android_dlopen_ext(const char* name, int flags, const void* /*extinfo*/) {
    return g_dlopen(name, flags);
}

WINAPK_GUEST_ABI void* g_dlsym(void* handle, const char* name) {
    GuestLinker* linker = GuestLinker::process();
    if (!linker || !name) {
        set_dlerror("dlsym", linker ? "symbol name is null" : "no guest linker");
        return nullptr;
    }
    std::string error;
    void* p = linker->lookup(handle, name, __builtin_return_address(0), &error);
    if (!p) set_dlerror("dlsym", error);
    return p;
}

// Libraries are never unloaded (see linker.h), so there is nothing to undo.
WINAPK_GUEST_ABI int g_dlclose(void* /*handle*/) { return 0; }

WINAPK_GUEST_ABI char* g_dlerror() {
    if (!t_dlerror_set) return nullptr;
    t_dlerror_set = false;
    return &t_dlerror[0];
}

WINAPK_GUEST_ABI int g_dladdr(const void* addr, GuestDlInfo* info) {
    GuestLinker* linker = GuestLinker::process();
    GuestLinker::AddrInfo a;
    if (!linker || !info || !linker->describe(addr, &a)) return 0;
    info->dli_fname = a.file;
    info->dli_fbase = const_cast<void*>(a.base);
    info->dli_sname = a.symbol;
    info->dli_saddr = a.symbol_addr;
    return 1;
}

struct LibdlSymbol {
    const char* name;
    void* addr;
};

#define WINAPK_LIBDL(name, fn) {name, reinterpret_cast<void*>(&fn)}

const LibdlSymbol kLibdl[] = {
    WINAPK_LIBDL("dlopen", g_dlopen),
    WINAPK_LIBDL("android_dlopen_ext", g_android_dlopen_ext),
    WINAPK_LIBDL("dlsym", g_dlsym),
    WINAPK_LIBDL("dlclose", g_dlclose),
    WINAPK_LIBDL("dlerror", g_dlerror),
    WINAPK_LIBDL("dladdr", g_dladdr),
};

#undef WINAPK_LIBDL

}  // namespace

void* guest_libdl_resolve(const char* name) {
    for (const LibdlSymbol& s : kLibdl) {
        if (std::strcmp(s.name, name) == 0) return s.addr;
    }
    return nullptr;
}

}  // namespace winapk
