// abi_thunk.h -- machine-code thunks for calls whose shape is only known at
// runtime.
//
// Most of the System V <-> Microsoft x64 boundary is handled by clang
// (WINAPK_GUEST_ABI, see guest_abi.h). Two kinds of call cannot be written as
// C++ ahead of time:
//
//   * HotSpot calling a guest JNI native method. Its argument list comes from
//     a Java method descriptor discovered at runtime, and the JNIEnv* HotSpot
//     passes has to be swapped for the guest's view of it. A thunk is
//     generated per (guest function, descriptor).
//
//   * Guest code calling a C-variadic JNI function (CallIntMethod(env, obj,
//     mid, ...)). clang refuses va_start in a sysv_abi function on Windows,
//     so an entry stub spills the System V argument registers into a
//     va_list-shaped block and hands it to a C++ decoder.
//
// Thunks live in a RWX pool. W^X (a dual-mapped section) and unwind tables
// (RtlAddFunctionTable) for the pool are later work.

#pragma once

#include <cstddef>
#include <cstdint>

namespace winapk {

// How one JNI argument travels. The small integer kinds are widened to 32
// bits by the thunk: clang-compiled guest code assumes its caller already
// sign/zero-extended them (the signext/zeroext convention), and HotSpot's
// native wrapper does not promise that.
enum class ArgKind : uint8_t {
    Word,    // pointers, jobject, jint, jlong: copied as 64 bits
    Bool,    // jboolean: zero-extend 8 -> 32
    Byte,    // jbyte: sign-extend 8 -> 32
    Char,    // jchar: zero-extend 16 -> 32
    Short,   // jshort: sign-extend 16 -> 32
    Float,   // jfloat: SSE class
    Double,  // jdouble: SSE class
};

// A Microsoft x64 callable that forwards to the System V function `target`.
// args[0] must be the JNIEnv*; it is replaced by env_xlate(args[0]) (an
// ordinary Microsoft x64 function) before the call. Returns nullptr on
// allocation failure.
void* make_jni_native_thunk(void* target, const ArgKind* args, size_t nargs, void* env_xlate);

// A System V callable for a C-variadic function with `named` integer-class
// fixed arguments (3 for CallIntMethod(env, obj, mid, ...), 4 for the
// Nonvirtual variants). It calls the System V function `handler` with the
// same fixed arguments plus a pointer to a SysVVaList over the variadic part.
void* make_sysv_variadic_entry(int named, void* handler);

// The x86-64 psABI va_list (one element of the __va_list_tag array).
struct SysVVaList {
    uint32_t gp_offset;        // next GP register slot in reg_save_area (< 48)
    uint32_t fp_offset;        // next XMM slot in reg_save_area (48..176)
    uint8_t* overflow_arg_area;  // next stack-passed argument
    uint8_t* reg_save_area;    // rdi rsi rdx rcx r8 r9, then xmm0..xmm7 (16 bytes each)
};

uint64_t sysv_va_next_gp(SysVVaList* va);
double sysv_va_next_fp(SysVVaList* va);

}  // namespace winapk
