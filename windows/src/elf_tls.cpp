// elf_tls.cpp -- see elf_tls.h.

#include "winapk/elf_tls.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <malloc.h>
#include <windows.h>

namespace winapk {

namespace {

struct Module {
    const void* image = nullptr;
    size_t filesz = 0, memsz = 0, align = 1;
    bool live = false;
};

// Indexed by module id; slot 0 is unused so that a zero DTPMOD64 is invalid.
std::mutex g_lock;
std::vector<Module> g_modules(1);

// A thread's blocks, indexed by module id.
struct ThreadBlocks {
    std::vector<void*> blocks;
};

void WINAPI free_thread_blocks(void* p) {
    auto* t = static_cast<ThreadBlocks*>(p);
    if (!t) return;
    for (void* b : t->blocks) _aligned_free(b);
    delete t;
}

DWORD fls_slot() {
    static const DWORD slot = FlsAlloc(free_thread_blocks);
    return slot;
}

ThreadBlocks* thread_blocks() {
    const DWORD slot = fls_slot();
    auto* t = static_cast<ThreadBlocks*>(FlsGetValue(slot));
    if (!t) {
        t = new ThreadBlocks();
        FlsSetValue(slot, t);
    }
    return t;
}

void* allocate_block(size_t id) {
    Module m;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (id >= g_modules.size() || !g_modules[id].live) return nullptr;
        m = g_modules[id];
    }
    size_t align = m.align < sizeof(void*) ? sizeof(void*) : m.align;
    void* block = _aligned_malloc(m.memsz ? m.memsz : 1, align);
    if (!block) return nullptr;
    std::memcpy(block, m.image, m.filesz);
    std::memset(static_cast<char*>(block) + m.filesz, 0, m.memsz - m.filesz);
    return block;
}

}  // namespace

size_t tls_register_module(const void* image, size_t filesz, size_t memsz, size_t align) {
    fls_slot();
    std::lock_guard<std::mutex> hold(g_lock);
    Module m;
    m.image = image;
    m.filesz = filesz;
    m.memsz = memsz;
    // p_align is a power of two or 0/1 for "no constraint".
    m.align = align > 1 ? align : 1;
    m.live = true;
    g_modules.push_back(m);
    return g_modules.size() - 1;
}

void tls_unregister_module(size_t id) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (id < g_modules.size()) g_modules[id].live = false;
}

WINAPK_GUEST_ABI void* tls_get_addr(const TlsIndex* index) {
    ThreadBlocks* t = thread_blocks();
    const size_t id = static_cast<size_t>(index->module);
    if (id >= t->blocks.size()) t->blocks.resize(id + 1, nullptr);
    void*& block = t->blocks[id];
    if (!block) block = allocate_block(id);
    if (!block) {
        // Bionic aborts on a bad module id too; there is no address to return.
        std::fprintf(stderr, "winapk: __tls_get_addr for unknown TLS module %zu\n", id);
        std::abort();
    }
    return static_cast<char*>(block) + index->offset;
}

}  // namespace winapk
