// guest_abi.h -- the calling-convention boundary between Android x86_64 code
// and the Windows host.
//
// Android x86_64 code follows the System V AMD64 ABI. Windows code follows
// the Microsoft x64 ABI. They disagree on almost everything that matters at a
// call boundary:
//
//                       System V (Android guest)      Microsoft x64 (host)
//   integer args        rdi rsi rdx rcx r8 r9         rcx rdx r8 r9
//   float args          xmm0-xmm7                     xmm0-xmm3 (slot-shared)
//   shadow space        none                          32 bytes above the ret addr
//   callee-saved        rbx rbp r12-r15               rbx rbp rdi rsi r12-r15
//                                                     xmm6-xmm15
//   red zone            128 bytes below rsp           none
//   va_list             struct (gp/fp offsets)        char*
//
// clang accepts __attribute__((sysv_abi)) on Windows x64 targets and generates
// the correct prologue/epilogue for it, including saving rdi/rsi/xmm6-15 when
// a sysv function is called from ms_abi code. (GCC accepts the attribute too,
// but mingw GCC 13 emits broken SEH unwind data for such functions.) So the
// rule for the whole port is simple:
//
//   * every host function that guest code can call is declared WINAPK_GUEST_ABI;
//   * every guest function pointer the host calls is a WINAPK_GUEST_ABI type.
//
// Variadic functions (printf, __android_log_print, JNI Call*Method) are the
// exception: their va_list layouts differ, so they get dedicated bridges and
// are not covered by this header yet.

#pragma once

#if defined(__clang__)
#define WINAPK_GUEST_ABI __attribute__((sysv_abi))
#else
#error "Win.apk needs clang: MSVC cannot emit System V calls, mingw GCC miscompiles them."
#endif

namespace winapk {

using GuestVoidFn = void(WINAPK_GUEST_ABI*)();
// Bionic calls DT_INIT_ARRAY entries as fn(argc, argv, envp).
using GuestInitFn = void(WINAPK_GUEST_ABI*)(int, char**, char**);
using GuestIRelativeResolver = void*(WINAPK_GUEST_ABI*)();

}  // namespace winapk
