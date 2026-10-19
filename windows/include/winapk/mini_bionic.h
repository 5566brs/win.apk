// mini_bionic.h -- the first slice of the Windows Bionic shim.
//
// The macOS shim (runtime/translator/src/bionic_shim.cpp) exports Bionic's
// names straight out of a dylib, because there an Android arm64 caller and a
// macOS callee agree on the calling convention. On Windows x64 they do not,
// so every export here is a WINAPK_GUEST_ABI wrapper around the Windows CRT,
// handed to the ELF loader through a name table instead of the PE export table.
//
// This slice holds what the PoC guest needs: allocation, string/memory
// routines, stdout, liblog's non-variadic entry point, the stack-protector
// failure hook, errno and the C++ atexit pair. qsort shows the other direction
// of the boundary: a guest comparator called back from host code.

#pragma once

#include <cstddef>

namespace winapk {

// Host address of a shim export, or nullptr if the shim does not have it.
void* mini_bionic_resolve(const char* name);

// Runs the destructors guest code registered with __cxa_atexit, newest first.
void mini_bionic_run_atexit();

}  // namespace winapk
