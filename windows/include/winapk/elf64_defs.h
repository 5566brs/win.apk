// elf64_defs.h -- the slice of the ELF64 format the Win.apk loader reads.
//
// Windows has no <elf.h>, so the layouts are spelled out here. Field names
// and constant values follow the System V gABI and the x86-64 psABI exactly,
// so anything here can be checked against `readelf` output one-to-one.

#pragma once

#include <cstdint>

namespace winapk::elf {

using Addr  = uint64_t;
using Off   = uint64_t;
using Half  = uint16_t;
using Word  = uint32_t;
using Sword = int32_t;
using Xword = uint64_t;
using Sxword = int64_t;

constexpr int kIdentSize = 16;
constexpr uint8_t kMag0 = 0x7f, kMag1 = 'E', kMag2 = 'L', kMag3 = 'F';
constexpr int kEiClass = 4;
constexpr int kEiData = 5;
constexpr uint8_t kClass64 = 2;
constexpr uint8_t kData2Lsb = 1;

constexpr Half kEtDyn = 3;
constexpr Half kEmX86_64 = 62;
constexpr Half kEmAarch64 = 183;

struct Ehdr {
    uint8_t e_ident[kIdentSize];
    Half e_type;
    Half e_machine;
    Word e_version;
    Addr e_entry;
    Off e_phoff;
    Off e_shoff;
    Word e_flags;
    Half e_ehsize;
    Half e_phentsize;
    Half e_phnum;
    Half e_shentsize;
    Half e_shnum;
    Half e_shstrndx;
};

struct Phdr {
    Word p_type;
    Word p_flags;
    Off p_offset;
    Addr p_vaddr;
    Addr p_paddr;
    Xword p_filesz;
    Xword p_memsz;
    Xword p_align;
};

constexpr Word kPtLoad = 1;
constexpr Word kPtDynamic = 2;
constexpr Word kPtTls = 7;
constexpr Word kPtGnuRelro = 0x6474e552;

constexpr Word kPfX = 1;
constexpr Word kPfW = 2;
constexpr Word kPfR = 4;

struct Dyn {
    Sxword d_tag;
    union {
        Xword d_val;
        Addr d_ptr;
    } d_un;
};

constexpr Sxword kDtNull = 0;
constexpr Sxword kDtNeeded = 1;
constexpr Sxword kDtPltRelSz = 2;
constexpr Sxword kDtHash = 4;
constexpr Sxword kDtStrTab = 5;
constexpr Sxword kDtSymTab = 6;
constexpr Sxword kDtRela = 7;
constexpr Sxword kDtRelaSz = 8;
constexpr Sxword kDtRelaEnt = 9;
constexpr Sxword kDtStrSz = 10;
constexpr Sxword kDtInit = 12;
constexpr Sxword kDtFini = 13;
constexpr Sxword kDtSoname = 14;
constexpr Sxword kDtJmpRel = 23;
constexpr Sxword kDtInitArray = 25;
constexpr Sxword kDtFiniArray = 26;
constexpr Sxword kDtInitArraySz = 27;
constexpr Sxword kDtFiniArraySz = 28;
constexpr Sxword kDtRelrSz = 35;
constexpr Sxword kDtRelr = 36;
constexpr Sxword kDtGnuHash = 0x6ffffef5;
// Android's packed relocations (APS2). Not decoded yet; the loader reports them.
constexpr Sxword kDtAndroidRela = 0x60000011;
constexpr Sxword kDtAndroidRelaSz = 0x60000012;

struct Sym {
    Word st_name;
    uint8_t st_info;
    uint8_t st_other;
    Half st_shndx;
    Addr st_value;
    Xword st_size;
};

constexpr Half kShnUndef = 0;
constexpr uint8_t kStbLocal = 0;
constexpr uint8_t kStbGlobal = 1;
constexpr uint8_t kStbWeak = 2;
constexpr uint8_t kSttTls = 6;

inline uint8_t st_bind(uint8_t info) { return info >> 4; }
inline uint8_t st_type(uint8_t info) { return info & 0xf; }

struct Rela {
    Addr r_offset;
    Xword r_info;
    Sxword r_addend;
};

inline uint32_t r_sym(Xword info) { return static_cast<uint32_t>(info >> 32); }
inline uint32_t r_type(Xword info) { return static_cast<uint32_t>(info & 0xffffffffu); }

// x86-64 psABI relocation types the loader handles (or names in its errors).
constexpr uint32_t kRX86_64_None = 0;
constexpr uint32_t kRX86_64_64 = 1;
constexpr uint32_t kRX86_64_GlobDat = 6;
constexpr uint32_t kRX86_64_JumpSlot = 7;
constexpr uint32_t kRX86_64_Relative = 8;
constexpr uint32_t kRX86_64_DtpMod64 = 16;
constexpr uint32_t kRX86_64_DtpOff64 = 17;
constexpr uint32_t kRX86_64_TpOff64 = 18;
constexpr uint32_t kRX86_64_IRelative = 37;

}  // namespace winapk::elf
