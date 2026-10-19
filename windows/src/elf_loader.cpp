// elf_loader.cpp -- Android x86_64 ELF loader for Windows. See elf_loader.h.

#include "winapk/elf_loader.h"

#include "winapk/elf64_defs.h"
#include "winapk/fs_tls_patch.h"
#include "winapk/guest_abi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <windows.h>

namespace winapk {

namespace {

using namespace winapk::elf;

// x86_64 Windows pages are 4 KiB. ELF segments built for Android are aligned
// to 4 KiB or 16 KiB, both multiples, so page math in ELF vaddrs lines up.
constexpr uint64_t kPage = 0x1000;

uint64_t page_down(uint64_t v) { return v & ~(kPage - 1); }
uint64_t page_up(uint64_t v) { return (v + kPage - 1) & ~(kPage - 1); }

// Tags lld emits for RELR when targeting Android API < 30 (--use-android-relr-tags).
constexpr Sxword kDtAndroidRelr = 0x6fffe000;
constexpr Sxword kDtAndroidRelrSz = 0x6fffe001;

std::string hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

bool read_file(const std::filesystem::path& path, std::vector<uint8_t>* out, std::string* error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        *error = "cannot open " + path.string();
        return false;
    }
    std::streamsize n = f.tellg();
    if (n <= 0) {
        *error = "empty file " + path.string();
        return false;
    }
    out->resize(static_cast<size_t>(n));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(out->data()), n)) {
        *error = "short read on " + path.string();
        return false;
    }
    return true;
}

DWORD page_protection(uint32_t pf) {
    const bool r = pf & kPfR, w = pf & kPfW, x = pf & kPfX;
    if (x && w) return PAGE_EXECUTE_READWRITE;
    if (x) return PAGE_EXECUTE_READ;
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;  // gap between segments
}

// SLEB128 stream used by Android's packed relocations (APS2).
class Sleb128Reader {
public:
    Sleb128Reader(const uint8_t* p, const uint8_t* end) : p_(p), end_(end) {}
    bool ok() const { return ok_; }
    int64_t next() {
        int64_t value = 0;
        unsigned shift = 0;
        uint8_t byte;
        do {
            if (p_ >= end_) {
                ok_ = false;
                return 0;
            }
            byte = *p_++;
            value |= static_cast<int64_t>(byte & 0x7f) << shift;
            shift += 7;
        } while ((byte & 0x80) && shift < 64);
        if (shift < 64 && (byte & 0x40)) value |= -(static_cast<int64_t>(1) << shift);
        return value;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

}  // namespace

std::unique_ptr<ElfImage> ElfImage::load(const std::filesystem::path& path,
                                         const SymbolResolver& resolve,
                                         std::string* error,
                                         const LoadOptions& options) {
    std::vector<uint8_t> file;
    if (!read_file(path, &file, error)) return nullptr;

    if (file.size() < sizeof(Ehdr)) {
        *error = "not an ELF file (too small)";
        return nullptr;
    }
    Ehdr eh;
    std::memcpy(&eh, file.data(), sizeof(eh));
    if (eh.e_ident[0] != kMag0 || eh.e_ident[1] != kMag1 || eh.e_ident[2] != kMag2 ||
        eh.e_ident[3] != kMag3) {
        *error = "not an ELF file (bad magic)";
        return nullptr;
    }
    if (eh.e_ident[kEiClass] != kClass64 || eh.e_ident[kEiData] != kData2Lsb) {
        *error = "not a little-endian ELF64 file (32-bit x86 is not supported)";
        return nullptr;
    }
    if (eh.e_machine != kEmX86_64) {
        *error = eh.e_machine == kEmAarch64
                     ? std::string("this is an arm64 library; Win.apk on x64 loads only x86_64 "
                                   "libraries (ARM needs the translation track)")
                     : "unsupported e_machine " + std::to_string(eh.e_machine);
        return nullptr;
    }
    if (eh.e_type != kEtDyn) {
        *error = "not a shared object (e_type " + std::to_string(eh.e_type) + ")";
        return nullptr;
    }
    if (eh.e_phentsize != sizeof(Phdr) ||
        eh.e_phoff + static_cast<uint64_t>(eh.e_phnum) * sizeof(Phdr) > file.size()) {
        *error = "program header table out of bounds";
        return nullptr;
    }

    std::unique_ptr<ElfImage> img(new ElfImage());
    for (int i = 0; i < eh.e_phnum; ++i) {
        Phdr ph;
        std::memcpy(&ph, file.data() + eh.e_phoff + i * sizeof(Phdr), sizeof(ph));
        if (ph.p_type == kPtLoad) {
            if (ph.p_filesz > ph.p_memsz || ph.p_offset + ph.p_filesz > file.size()) {
                *error = "PT_LOAD " + std::to_string(i) + " out of bounds";
                return nullptr;
            }
            img->loads_.push_back({ph.p_vaddr, ph.p_memsz, ph.p_filesz, ph.p_offset, ph.p_flags});
        } else if (ph.p_type == kPtDynamic) {
            img->dynamic_vaddr_ = ph.p_vaddr;
        } else if (ph.p_type == kPtGnuRelro) {
            img->has_relro_ = true;
            img->relro_vaddr_ = ph.p_vaddr;
            img->relro_memsz_ = ph.p_memsz;
        } else if (ph.p_type == kPtTls) {
            *error = "the library uses ELF TLS (PT_TLS), which Win.apk does not support yet";
            return nullptr;
        }
    }
    if (img->loads_.empty()) {
        *error = "no PT_LOAD segments";
        return nullptr;
    }
    if (img->dynamic_vaddr_ == 0) {
        *error = "no PT_DYNAMIC segment";
        return nullptr;
    }

    if (!img->map_segments(file, error)) return nullptr;
    if (!img->read_dynamic(error)) return nullptr;

    if (options.patch_fs_stack_guard) {
        for (const Segment& s : img->loads_) {
            if (s.flags & kPfX) {
                img->fs_sites_patched_ +=
                    patch_fs_stack_guard_sites(img->at<uint8_t>(s.vaddr), s.filesz);
            }
        }
    }

    if (!img->apply_relocations(resolve, error)) return nullptr;
    if (!img->protect_segments(error)) return nullptr;
    if (!img->apply_irelative(error)) return nullptr;
    img->seal_relro();
    FlushInstructionCache(GetCurrentProcess(), img->mem_, img->size_);

    img->run_initializers();
    return img;
}

ElfImage::~ElfImage() {
    if (initialized_) run_finalizers();
    if (mem_) VirtualFree(mem_, 0, MEM_RELEASE);
}

bool ElfImage::in_image(uint64_t vaddr, size_t len) const {
    return vaddr >= lo_vaddr_ && vaddr <= hi_vaddr_ && len <= hi_vaddr_ - vaddr;
}

bool ElfImage::map_segments(const std::vector<uint8_t>& file, std::string* error) {
    lo_vaddr_ = UINT64_MAX;
    hi_vaddr_ = 0;
    for (const Segment& s : loads_) {
        lo_vaddr_ = std::min(lo_vaddr_, page_down(s.vaddr));
        hi_vaddr_ = std::max(hi_vaddr_, page_up(s.vaddr + s.memsz));
    }
    size_ = static_cast<size_t>(hi_vaddr_ - lo_vaddr_);

    // Reserve and commit the whole span read-write; protect_segments() narrows
    // it per page once relocation is done. Fresh pages are zero, which covers
    // every segment's .bss tail.
    mem_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!mem_) {
        *error = "VirtualAlloc(" + std::to_string(size_) + ") failed: " + std::to_string(GetLastError());
        return false;
    }
    bias_ = reinterpret_cast<uintptr_t>(mem_) - lo_vaddr_;

    for (const Segment& s : loads_) {
        std::memcpy(at<uint8_t>(s.vaddr), file.data() + s.offset, s.filesz);
    }
    return true;
}

bool ElfImage::read_dynamic(std::string* error) {
    std::vector<uint64_t> needed_offsets;
    uint64_t soname_offset = UINT64_MAX;
    for (const Dyn* d = at<Dyn>(dynamic_vaddr_);; ++d) {
        if (!in_image(reinterpret_cast<uintptr_t>(d) - bias_, sizeof(Dyn))) {
            *error = "PT_DYNAMIC runs past the image";
            return false;
        }
        if (d->d_tag == kDtNull) break;
        const uint64_t v = d->d_un.d_val;
        switch (d->d_tag) {
            case kDtNeeded: needed_offsets.push_back(v); break;
            case kDtSoname: soname_offset = v; break;
            case kDtStrTab: strtab_ = v; break;
            case kDtStrSz: strsz_ = v; break;
            case kDtSymTab: symtab_ = v; break;
            case kDtHash: hash_ = v; break;
            case kDtGnuHash: gnu_hash_ = v; break;
            case kDtRela: rela_ = v; break;
            case kDtRelaSz: relasz_ = v; break;
            case kDtRelaEnt:
                if (v != sizeof(Rela)) {
                    *error = "unexpected DT_RELAENT " + std::to_string(v);
                    return false;
                }
                break;
            case kDtJmpRel: jmprel_ = v; break;
            case kDtPltRelSz: pltrelsz_ = v; break;
            case kDtRelr: case kDtAndroidRelr: relr_ = v; break;
            case kDtRelrSz: case kDtAndroidRelrSz: relrsz_ = v; break;
            case kDtAndroidRela: android_rela_ = v; break;
            case kDtAndroidRelaSz: android_relasz_ = v; break;
            case kDtInit: init_ = v; break;
            case kDtFini: fini_ = v; break;
            case kDtInitArray: init_array_ = v; break;
            case kDtInitArraySz: init_arraysz_ = v; break;
            case kDtFiniArray: fini_array_ = v; break;
            case kDtFiniArraySz: fini_arraysz_ = v; break;
            default: break;
        }
    }
    if (!strtab_ || !symtab_ || !in_image(strtab_, strsz_)) {
        *error = "missing or out-of-range DT_STRTAB/DT_SYMTAB";
        return false;
    }
    const char* strtab = at<const char>(strtab_);
    auto str = [&](uint64_t off) -> std::string {
        return off < strsz_ ? std::string(strtab + off, strnlen(strtab + off, strsz_ - off)) : std::string();
    };
    for (uint64_t off : needed_offsets) needed_.push_back(str(off));
    if (soname_offset != UINT64_MAX) soname_ = str(soname_offset);

    // Symbol count: DT_HASH stores it (nchain); DT_GNU_HASH has to be walked
    // to the end of its last non-empty chain.
    size_t nsyms = 0;
    if (hash_) {
        nsyms = at<const uint32_t>(hash_)[1];
    } else if (gnu_hash_) {
        const uint32_t* gh = at<const uint32_t>(gnu_hash_);
        const uint32_t nbuckets = gh[0], symoffset = gh[1], bloom_words = gh[2];
        const uint32_t* buckets = reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint64_t*>(gh + 4) + bloom_words);
        const uint32_t* chain = buckets + nbuckets;
        uint32_t last = 0;
        for (uint32_t b = 0; b < nbuckets; ++b) last = std::max(last, buckets[b]);
        if (last >= symoffset) {
            while (!(chain[last - symoffset] & 1)) ++last;
            nsyms = last + 1;
        } else {
            nsyms = symoffset;
        }
    }
    const Sym* syms = at<const Sym>(symtab_);
    for (size_t i = 1; i < nsyms; ++i) {
        const Sym& s = syms[i];
        const uint8_t bind = st_bind(s.st_info);
        if (s.st_shndx == kShnUndef || (bind != kStbGlobal && bind != kStbWeak)) continue;
        if (st_type(s.st_info) == kSttTls) continue;
        exports_.emplace(str(s.st_name), bias_ + s.st_value);
    }
    return true;
}

bool ElfImage::apply_relocations(const SymbolResolver& resolve, std::string* error) {
    const Sym* syms = at<const Sym>(symtab_);
    const char* strtab = at<const char>(strtab_);
    std::vector<std::string> missing;
    std::string fatal;

    auto symbol_value = [&](uint32_t idx, uint64_t* out) -> bool {
        const Sym& s = syms[idx];
        const char* name = strtab + s.st_name;
        if (st_type(s.st_info) == kSttTls) {
            fatal = std::string("ELF TLS symbol ") + name + " is not supported yet";
            return false;
        }
        if (s.st_shndx != kShnUndef) {
            *out = bias_ + s.st_value;
            return true;
        }
        if (void* host = resolve(name)) {
            *out = reinterpret_cast<uint64_t>(host);
            return true;
        }
        if (st_bind(s.st_info) == kStbWeak) {
            *out = 0;
            return true;
        }
        if (std::find(missing.begin(), missing.end(), name) == missing.end()) missing.push_back(name);
        *out = 0;
        return true;  // keep going so every missing symbol is reported at once
    };

    auto apply = [&](const Rela& r) -> bool {
        const uint32_t type = r_type(r.r_info);
        if (type == kRX86_64_None) return true;
        if (!in_image(r.r_offset, sizeof(uint64_t))) {
            fatal = "relocation target " + hex(r.r_offset) + " outside the image";
            return false;
        }
        uint64_t* where = at<uint64_t>(r.r_offset);
        uint64_t s = 0;
        switch (type) {
            case kRX86_64_Relative:
                *where = bias_ + r.r_addend;
                break;
            case kRX86_64_64:
                if (!symbol_value(r_sym(r.r_info), &s)) return false;
                *where = s + r.r_addend;
                break;
            case kRX86_64_GlobDat:
            case kRX86_64_JumpSlot:
                if (!symbol_value(r_sym(r.r_info), &s)) return false;
                *where = s;
                break;
            case kRX86_64_IRelative:
                // The resolver is guest code; it can only run once the text
                // is executable, so this waits for apply_irelative().
                irelative_slots_.push_back(r.r_offset);
                irelative_addends_.push_back(r.r_addend);
                break;
            case kRX86_64_DtpMod64:
            case kRX86_64_DtpOff64:
            case kRX86_64_TpOff64:
                fatal = "ELF TLS relocation type " + std::to_string(type) + " is not supported yet";
                return false;
            default:
                fatal = "unsupported x86_64 relocation type " + std::to_string(type);
                return false;
        }
        ++relocs_applied_;
        return true;
    };

    auto apply_table = [&](uint64_t vaddr, uint64_t size) -> bool {
        if (!vaddr || !size) return true;
        if (!in_image(vaddr, size)) {
            fatal = "relocation table " + hex(vaddr) + " outside the image";
            return false;
        }
        const Rela* r = at<const Rela>(vaddr);
        for (uint64_t i = 0; i < size / sizeof(Rela); ++i) {
            if (!apply(r[i])) return false;
        }
        return true;
    };

    // DT_RELR: a bitmap encoding of R_X86_64_RELATIVE runs (gABI "RELR").
    auto apply_relr = [&]() -> bool {
        if (!relr_ || !relrsz_) return true;
        if (!in_image(relr_, relrsz_)) {
            fatal = "DT_RELR table outside the image";
            return false;
        }
        const uint64_t* e = at<const uint64_t>(relr_);
        uint64_t base = 0;  // vaddr of the next word an odd entry describes
        for (uint64_t i = 0; i < relrsz_ / 8; ++i) {
            if ((e[i] & 1) == 0) {
                if (!in_image(e[i], 8)) {
                    fatal = "RELR address outside the image";
                    return false;
                }
                *at<uint64_t>(e[i]) += bias_;
                base = e[i] + 8;
                ++relocs_applied_;
            } else {
                uint64_t bits = e[i] >> 1;
                for (uint64_t k = 0; bits; ++k, bits >>= 1) {
                    if (!(bits & 1)) continue;
                    if (!in_image(base + k * 8, 8)) {
                        fatal = "RELR bitmap address outside the image";
                        return false;
                    }
                    *at<uint64_t>(base + k * 8) += bias_;
                    ++relocs_applied_;
                }
                base += 63 * 8;
            }
        }
        return true;
    };

    // DT_ANDROID_RELA: "APS2" + SLEB128 groups, decoded exactly like bionic's
    // packed_reloc_iterator (linker/linker_reloc_iterators.h).
    auto apply_android_packed = [&]() -> bool {
        if (!android_rela_ || !android_relasz_) return true;
        if (!in_image(android_rela_, android_relasz_) || android_relasz_ < 4) {
            fatal = "DT_ANDROID_RELA outside the image";
            return false;
        }
        const uint8_t* p = at<const uint8_t>(android_rela_);
        if (std::memcmp(p, "APS2", 4) != 0) {
            fatal = "DT_ANDROID_RELA without the APS2 magic";
            return false;
        }
        constexpr int64_t kGroupedByInfo = 1, kGroupedByOffsetDelta = 2, kGroupedByAddend = 4,
                          kGroupHasAddend = 8;
        Sleb128Reader in(p + 4, p + android_relasz_);
        const int64_t count = in.next();
        Rela rel{};
        rel.r_offset = static_cast<uint64_t>(in.next());
        int64_t done = 0;
        while (done < count && in.ok()) {
            const int64_t group_size = in.next();
            const int64_t flags = in.next();
            int64_t offset_delta = 0;
            if (flags & kGroupedByOffsetDelta) offset_delta = in.next();
            if (flags & kGroupedByInfo) rel.r_info = static_cast<uint64_t>(in.next());
            if ((flags & kGroupHasAddend) && (flags & kGroupedByAddend)) {
                rel.r_addend += in.next();
            } else if (!(flags & kGroupHasAddend)) {
                rel.r_addend = 0;
            }
            for (int64_t i = 0; i < group_size && in.ok(); ++i) {
                rel.r_offset += (flags & kGroupedByOffsetDelta) ? offset_delta : in.next();
                if (!(flags & kGroupedByInfo)) rel.r_info = static_cast<uint64_t>(in.next());
                if ((flags & kGroupHasAddend) && !(flags & kGroupedByAddend)) rel.r_addend += in.next();
                if (!apply(rel)) return false;
                ++done;
            }
        }
        if (!in.ok()) {
            fatal = "truncated APS2 relocation stream";
            return false;
        }
        return true;
    };

    const bool ok = apply_relr() && apply_android_packed() && apply_table(rela_, relasz_) &&
                    apply_table(jmprel_, pltrelsz_);
    if (!ok) {
        *error = fatal;
        return false;
    }
    if (!missing.empty()) {
        *error = std::to_string(missing.size()) + " undefined symbol(s) the host does not provide:";
        for (size_t i = 0; i < missing.size() && i < 64; ++i) *error += "\n    " + missing[i];
        if (missing.size() > 64) *error += "\n    ...";
        return false;
    }
    return true;
}

bool ElfImage::protect_segments(std::string* error) {
    // Two segments can share a page (lld does this without -z separate-code);
    // the page gets the union of their permissions, as with mmap on Linux.
    const size_t npages = size_ / kPage;
    std::vector<uint32_t> flags(npages, 0);
    for (const Segment& s : loads_) {
        const size_t first = (page_down(s.vaddr) - lo_vaddr_) / kPage;
        const size_t last = (page_up(s.vaddr + s.memsz) - lo_vaddr_) / kPage;
        for (size_t p = first; p < last; ++p) flags[p] |= s.flags & (kPfR | kPfW | kPfX);
    }
    size_t run = 0;
    for (size_t p = 1; p <= npages; ++p) {
        if (p < npages && flags[p] == flags[run]) continue;
        DWORD old;
        if (!VirtualProtect(mem_ + run * kPage, (p - run) * kPage, page_protection(flags[run]), &old)) {
            *error = "VirtualProtect failed: " + std::to_string(GetLastError());
            return false;
        }
        run = p;
    }
    return true;
}

bool ElfImage::apply_irelative(std::string* error) {
    for (size_t i = 0; i < irelative_slots_.size(); ++i) {
        auto resolver = reinterpret_cast<GuestIRelativeResolver>(bias_ + irelative_addends_[i]);
        uint64_t* where = at<uint64_t>(irelative_slots_[i]);
        // The slot sits in a writable segment (GOT / data); RELRO is sealed later.
        *where = reinterpret_cast<uint64_t>(resolver());
        ++relocs_applied_;
    }
    (void)error;
    return true;
}

void ElfImage::seal_relro() {
    if (!has_relro_) return;
    // Same rounding as bionic's phdr_table_protect_gnu_relro.
    const uint64_t start = page_down(relro_vaddr_);
    const uint64_t end = page_up(relro_vaddr_ + relro_memsz_);
    if (end <= start || !in_image(start, end - start)) return;
    DWORD old;
    VirtualProtect(at<uint8_t>(start), end - start, PAGE_READONLY, &old);
}

void ElfImage::run_initializers() {
    static char* empty_argv[] = {nullptr};
    static char* empty_envp[] = {nullptr};
    if (init_) reinterpret_cast<GuestVoidFn>(bias_ + init_)();
    if (init_array_ && init_arraysz_ && in_image(init_array_, init_arraysz_)) {
        const uint64_t* fns = at<const uint64_t>(init_array_);
        for (uint64_t i = 0; i < init_arraysz_ / 8; ++i) {
            // 0 and -1 are placeholders some toolchains leave in the array.
            if (fns[i] == 0 || fns[i] == UINT64_MAX) continue;
            reinterpret_cast<GuestInitFn>(fns[i])(0, empty_argv, empty_envp);
        }
    }
    initialized_ = true;
}

void ElfImage::run_finalizers() {
    if (fini_array_ && fini_arraysz_ && in_image(fini_array_, fini_arraysz_)) {
        const uint64_t* fns = at<const uint64_t>(fini_array_);
        for (uint64_t i = fini_arraysz_ / 8; i-- > 0;) {
            if (fns[i] == 0 || fns[i] == UINT64_MAX) continue;
            reinterpret_cast<GuestVoidFn>(fns[i])();
        }
    }
    if (fini_) reinterpret_cast<GuestVoidFn>(bias_ + fini_)();
    initialized_ = false;
}

void* ElfImage::dlsym(const char* name) const {
    auto it = exports_.find(name);
    return it == exports_.end() ? nullptr : reinterpret_cast<void*>(it->second);
}

}  // namespace winapk
