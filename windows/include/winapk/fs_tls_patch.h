// fs_tls_patch.h -- the Bionic x86_64 stack guard on Windows.
//
// Every Android x86_64 function built with a stack protector loads the guard
// from Bionic's TLS block through the FS segment:
//
//     mov rax, qword ptr fs:[0x28]      64 48 8b 04 25 28 00 00 00
//     ...
//     sub rax, qword ptr fs:[0x28]      64 48 2b 04 25 28 00 00 00
//
// 0x28 is TLS_SLOT_STACK_GUARD (slot 5 x 8 bytes). Bionic makes FS point at
// that block with arch_prctl(ARCH_SET_FS); 64-bit Windows offers user mode no
// way to set the FS base, so these loads would read through a zero base.
//
// Windows keeps the TEB at the GS base instead, and the TEB starts with an
// NT_TIB, whose layout on x64 is:
//
//     0x00 ExceptionList   0x08 StackBase   0x10 StackLimit   0x18 SubSystemTib
//     0x20 FiberData       0x28 ArbitraryUserPointer          0x30 Self
//
// So swapping the one-byte FS prefix (0x64) for the GS prefix (0x65) keeps the
// instruction length and points the load at NT_TIB.ArbitraryUserPointer, a
// per-thread field user code may use. The canary only has to stay constant on
// a thread between a function's prologue and epilogue, which holds even on a
// thread whose slot was never initialized.
//
// PoC SCOPE. The scanner matches the exact 9-byte stack-guard encodings, not
// arbitrary FS accesses. It does not decode instructions, so a data byte run
// that happens to match would be patched as well. Win.apk phase 3 replaces this
// with a decoder-driven rewrite (Zydis) that also covers %fs:0 (__get_tls)
// and ELF TLS accesses, and moves the guard into a slot Win.apk owns.

#pragma once

#include <cstddef>
#include <cstdint>

namespace winapk {

// Rewrites the stack-guard loads in [code, code + len). Returns sites patched.
size_t patch_fs_stack_guard_sites(uint8_t* code, size_t len);

// Sets the value guest code reads as its stack guard on the calling thread.
void set_guest_stack_guard_for_current_thread(uint64_t value);

// Reads gs:[0x28] directly: what a patched guest load sees on this thread.
uint64_t read_guest_stack_guard_slot();

}  // namespace winapk
