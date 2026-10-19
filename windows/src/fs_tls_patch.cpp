// fs_tls_patch.cpp -- see fs_tls_patch.h for why the FS prefix becomes GS.

#include "winapk/fs_tls_patch.h"

#include <cstring>

#include <intrin.h>
#include <windows.h>

namespace winapk {

namespace {

constexpr uint8_t kPrefixFs = 0x64;
constexpr uint8_t kPrefixGs = 0x65;
constexpr uint32_t kStackGuardOffset = 0x28;  // TLS_SLOT_STACK_GUARD * 8

// REX.W, optionally REX.R (destination register r8-r15). REX.X/REX.B must be
// clear: with the SIB byte below they would change the addressing mode.
bool is_rex_w(uint8_t b) { return b == 0x48 || b == 0x4c; }

// Read-only uses of the guard: mov r64, m64 / sub / xor / cmp (both operand
// orders). A write (89 /r) is never patched; only Bionic's libc init writes
// the guard, and that code does not run here.
bool is_guard_read_opcode(uint8_t op) {
    return op == 0x8b || op == 0x2b || op == 0x33 || op == 0x3b || op == 0x39;
}

}  // namespace

size_t patch_fs_stack_guard_sites(uint8_t* code, size_t len) {
    size_t patched = 0;
    // prefix, REX, opcode, ModRM, SIB, disp32
    constexpr size_t kInsnLen = 9;
    for (size_t i = 0; i + kInsnLen <= len; ++i) {
        if (code[i] != kPrefixFs) continue;
        if (!is_rex_w(code[i + 1])) continue;
        if (!is_guard_read_opcode(code[i + 2])) continue;
        // ModRM mod=00 rm=100 (SIB follows); SIB 0x25 = no base, no index,
        // disp32: an absolute segment-relative address.
        if ((code[i + 3] & 0xc7) != 0x04) continue;
        if (code[i + 4] != 0x25) continue;
        uint32_t disp;
        std::memcpy(&disp, code + i + 5, sizeof(disp));
        if (disp != kStackGuardOffset) continue;
        code[i] = kPrefixGs;
        ++patched;
        i += kInsnLen - 1;
    }
    return patched;
}

void set_guest_stack_guard_for_current_thread(uint64_t value) {
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    tib->ArbitraryUserPointer = reinterpret_cast<PVOID>(value);
}

uint64_t read_guest_stack_guard_slot() {
    return __readgsqword(kStackGuardOffset);
}

}  // namespace winapk
