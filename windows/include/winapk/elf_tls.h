// elf_tls.h -- dynamic ELF TLS for guest libraries.
//
// Android libraries built for API 29 and later may use ELF TLS (`thread_local`,
// `__thread`) instead of emulated TLS. A shared library reaches its TLS through
// the general-dynamic model: the compiler emits `__tls_get_addr(&index)`, where
// `index` is a GOT pair the loader fills from R_X86_64_DTPMOD64 (the module id)
// and R_X86_64_DTPOFF64 (the offset inside the module's TLS block).
//
// Each library with a PT_TLS segment registers here and gets a module id. A
// thread's block for a module is allocated on its first access, from the
// module's initialization image (.tdata, then zeroed .tbss), and freed when
// the thread exits (an FLS callback, so it also covers threads the guest did
// not create through pthread_create). Module ids are never reused: a block of
// an unloaded module is simply not handed out again.
//
// Not supported: the static (initial-exec and local-exec) models, which read
// %fs:0 directly and need R_X86_64_TPOFF64. Bionic only allows those in the
// executable and libc itself, so app libraries do not use them.

#pragma once

#include <cstddef>
#include <cstdint>

#include "winapk/guest_abi.h"

namespace winapk {

// The GOT pair `__tls_get_addr` receives.
struct TlsIndex {
    uint64_t module;
    uint64_t offset;
};

// Registers a module's TLS template. `image` must stay valid (and hold the
// relocated .tdata) for as long as the module is loaded. Returns its id (>= 1).
size_t tls_register_module(const void* image, size_t filesz, size_t memsz, size_t align);
// Stops handing out blocks for `id`. Blocks threads already hold are freed
// when those threads exit.
void tls_unregister_module(size_t id);

// The guest's `__tls_get_addr`.
WINAPK_GUEST_ABI void* tls_get_addr(const TlsIndex* index);

}  // namespace winapk
