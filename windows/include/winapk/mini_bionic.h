// mini_bionic.h -- the first slice of the Windows Bionic shim.
//
// The macOS shim (runtime/translator/src/bionic_shim.cpp) exports Bionic's
// names straight out of a dylib, because there an Android arm64 caller and a
// macOS callee agree on the calling convention. On Windows x64 they do not,
// so every export here is a WINAPK_GUEST_ABI wrapper around the Windows CRT,
// handed to the ELF loader through a name table instead of the PE export table.
//
// mini_bionic.cpp holds the core (allocation, errno, the C++ atexit pair,
// qsort's guest comparator) and the lookup; the rest lives in src/bionic/,
// one module per area, each with its own table (bionic_internal.h).

#pragma once

#include <cstddef>

namespace winapk {

// Host address of a shim export, or nullptr if the shim does not have it.
void* mini_bionic_resolve(const char* name);

// Runs the destructors guest code registered with __cxa_atexit, newest first.
void mini_bionic_run_atexit();

// The guest's system properties (__system_property_get). The Java framework
// pushes its values here (NativeCocoa.setSystemProperty); null erases.
void property_set(const char* name, const char* value);

// The guest's environment (getenv/setenv), apart from the host's. setenv and
// unsetenv return 0 or -errno (Linux numbering), as NativeOs expects.
const char* bionic_getenv(const char* name);
int bionic_setenv(const char* name, const char* value, int overwrite);
int bionic_unsetenv(const char* name);

}  // namespace winapk
