// elf_loader.h -- loads an Android x86_64 ELF shared object into a Windows
// process and makes it callable.
//
// This is the x86_64 counterpart of runtime/translator/src/arm64_native.cpp:
// it does what Android's dynamic linker would do at dlopen() time, then hands
// back an image whose symbols the host calls through WINAPK_GUEST_ABI pointers.
//
//   1. copy the PT_LOAD segments into one VirtualAlloc'd range.
//      (MapViewOfFile cannot be used: its file offsets must be multiples of the
//      64 KiB allocation granularity, ELF segments are 4 KiB or 16 KiB aligned)
//   2. patch the Bionic stack-guard loads (%fs:0x28) -- see fs_tls_patch.h.
//   3. apply relocations: RELA, RELR (DT_RELR and DT_ANDROID_RELR) and
//      Android's packed APS2 format (DT_ANDROID_RELA).
//   4. set per-page protections, resolve IRELATIVE, then seal PT_GNU_RELRO.
//   5. run DT_INIT and DT_INIT_ARRAY.
//
// Not handled yet (the loader fails with a clear message instead):
//   * ELF TLS relocations (R_X86_64_DTPMOD64/DTPOFF64/TPOFF64).
//   * DT_NEEDED between guest libraries is reported, not followed: the caller's
//     resolver decides what an undefined symbol binds to. GuestLinker
//     (linker.h) is the caller that follows it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace winapk {

// Returns the host address for an undefined guest symbol, or nullptr.
using SymbolResolver = std::function<void*(const char* name)>;

struct LoadOptions {
    // Rewrite %fs:0x28 stack-guard loads to %gs:0x28. Turning it off is only
    // useful to show that guest code built with a stack protector crashes
    // without it.
    bool patch_fs_stack_guard = true;
    // Called for an undefined, non-weak symbol nobody defines. A non-null
    // result is bound in its place (the runtime binds a stub that reports the
    // name when called); null keeps the default, failing the link and listing
    // every such symbol.
    SymbolResolver unresolved;
};

class ElfImage {
public:
    // map() + link() + initialize(), for a library with no guest dependencies.
    static std::unique_ptr<ElfImage> load(const std::filesystem::path& path,
                                          const SymbolResolver& resolve,
                                          std::string* error,
                                          const LoadOptions& options = {});

    // The three phases separately, for a linker that loads a whole DT_NEEDED
    // tree: map every library first, then link each one against the others,
    // then run constructors dependencies first. This is bionic's order
    // (find_libraries), and it is what lets two libraries refer to each other.
    //
    // map: steps 1 and 2. Exports and DT_NEEDED are readable afterwards, but
    // the image is not relocated and must not be called.
    static std::unique_ptr<ElfImage> map(const std::filesystem::path& path, std::string* error,
                                         const LoadOptions& options = {});
    // link: steps 3 and 4. IRELATIVE resolvers run here, so this is the first
    // point at which guest code executes. Calling it again is a no-op.
    bool link(const SymbolResolver& resolve, std::string* error);
    // initialize: step 5. Runs once; does nothing before link() succeeded.
    void initialize();
    bool linked() const { return linked_; }
    bool initialized() const { return initialized_; }

    ~ElfImage();

    ElfImage(const ElfImage&) = delete;
    ElfImage& operator=(const ElfImage&) = delete;

    // Address of an exported (defined, global or weak) symbol, or nullptr.
    void* dlsym(const char* name) const;
    // Every exported symbol and its address.
    const std::unordered_map<std::string, uintptr_t>& exports() const { return exports_; }

    // True if `p` lies inside the mapped image.
    bool contains(const void* p) const {
        const auto a = reinterpret_cast<uintptr_t>(p);
        const auto lo = reinterpret_cast<uintptr_t>(mem_);
        return a >= lo && a - lo < size_;
    }

    // The defined dynamic symbol whose [value, value + size) covers `p`, for
    // dladdr. Returns false if there is none.
    bool symbol_at(const void* p, const char** name, void** start) const;

    const std::filesystem::path& path() const { return path_; }
    const void* base() const { return mem_; }
    const std::string& soname() const { return soname_; }
    const std::vector<std::string>& needed() const { return needed_; }
    uintptr_t load_bias() const { return bias_; }
    size_t mapped_size() const { return size_; }
    size_t fs_sites_patched() const { return fs_sites_patched_; }
    size_t relocations_applied() const { return relocs_applied_; }

private:
    ElfImage() = default;

    bool map_segments(const std::vector<uint8_t>& file, std::string* error);
    bool read_dynamic(std::string* error);
    bool apply_relocations(const SymbolResolver& resolve, std::string* error);
    bool protect_segments(std::string* error);
    bool apply_irelative(std::string* error);
    void seal_relro();
    void run_initializers();
    void run_finalizers();

    bool in_image(uint64_t vaddr, size_t len) const;
    template <typename T> T* at(uint64_t vaddr) const {
        return reinterpret_cast<T*>(bias_ + vaddr);
    }

    uint8_t* mem_ = nullptr;
    size_t size_ = 0;
    uintptr_t bias_ = 0;     // runtime address = bias_ + ELF vaddr
    uint64_t lo_vaddr_ = 0;  // page-aligned lowest PT_LOAD vaddr
    uint64_t hi_vaddr_ = 0;  // page-aligned end of the highest PT_LOAD

    struct Segment { uint64_t vaddr, memsz, filesz, offset; uint32_t flags; };
    std::vector<Segment> loads_;
    bool has_relro_ = false;
    uint64_t relro_vaddr_ = 0, relro_memsz_ = 0;
    uint64_t dynamic_vaddr_ = 0;

    // From PT_DYNAMIC (vaddrs; 0 = absent).
    uint64_t strtab_ = 0, strsz_ = 0, symtab_ = 0, hash_ = 0, gnu_hash_ = 0;
    uint64_t rela_ = 0, relasz_ = 0, jmprel_ = 0, pltrelsz_ = 0;
    uint64_t relr_ = 0, relrsz_ = 0, android_rela_ = 0, android_relasz_ = 0;
    uint64_t init_ = 0, fini_ = 0;
    uint64_t init_array_ = 0, init_arraysz_ = 0, fini_array_ = 0, fini_arraysz_ = 0;

    std::filesystem::path path_;
    LoadOptions options_;
    std::string soname_;
    std::vector<std::string> needed_;
    size_t nsyms_ = 0;
    std::unordered_map<std::string, uintptr_t> exports_;
    std::vector<uint64_t> irelative_slots_;  // vaddrs; resolved after protect
    std::vector<int64_t> irelative_addends_;

    size_t fs_sites_patched_ = 0;
    size_t relocs_applied_ = 0;
    bool linked_ = false;
    bool initialized_ = false;
};

}  // namespace winapk
