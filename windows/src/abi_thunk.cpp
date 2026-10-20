// abi_thunk.cpp -- see abi_thunk.h.
//
// The emitter writes only the handful of x86-64 instruction forms below, each
// with a fixed encoding (disp32 addressing everywhere, so no short/long form
// choices). Register numbers are the hardware ones: rax 0, rcx 1, rdx 2,
// rbx 3, rsp 4, rbp 5, rsi 6, rdi 7, r8-r15 8-15; xmmN is N.

#include "winapk/abi_thunk.h"

#include <cstring>
#include <mutex>
#include <vector>

#include <windows.h>

namespace winapk {

namespace {

constexpr int RAX = 0, RCX = 1, RDX = 2, RBP = 5, RSI = 6, RDI = 7, R8 = 8, R9 = 9, R11 = 11;

// System V integer argument registers, in order.
constexpr int kSysVIntRegs[6] = {RDI, RSI, RDX, RCX, R8, R9};
// Microsoft x64 integer argument registers, in order.
constexpr int kMsIntRegs[4] = {RCX, RDX, R8, R9};

class Emitter {
public:
    const std::vector<uint8_t>& bytes() const { return b_; }

    void u8(uint8_t v) { b_.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i))); }

    void push(int r) { if (r >= 8) u8(0x41); u8(0x50 + (r & 7)); }
    void pop(int r) { if (r >= 8) u8(0x41); u8(0x58 + (r & 7)); }
    void mov_rbp_rsp() { u8(0x48); u8(0x89); u8(0xe5); }
    void leave() { u8(0xc9); }
    void ret() { u8(0xc3); }
    void sub_rsp(uint32_t n) { u8(0x48); u8(0x81); u8(0xec); u32(n); }
    void add_rsp(uint32_t n) { u8(0x48); u8(0x81); u8(0xc4); u32(n); }

    // mov r64, imm64 / call r64
    void mov_imm64(int r, uint64_t v) { u8(0x48 | (r >= 8 ? 1 : 0)); u8(0xb8 + (r & 7)); u64(v); }
    void call(int r) { if (r >= 8) u8(0x41); u8(0xff); u8(0xd0 + (r & 7)); }
    void mov_eax_imm32(uint32_t v) { u8(0xb8); u32(v); }

    // [rbp + disp32] and [rsp + disp32] operands.
    void store_rbp(int r, int32_t disp) { rex_w(r); u8(0x89); modrm_rbp(r, disp); }
    void load_rbp(int r, int32_t disp) { rex_w(r); u8(0x8b); modrm_rbp(r, disp); }
    void store_rsp(int r, int32_t disp) { rex_w(r); u8(0x89); modrm_rsp(r, disp); }
    void lea_rbp(int r, int32_t disp) { rex_w(r); u8(0x8d); modrm_rbp(r, disp); }
    void lea_rsp(int r, int32_t disp) { rex_w(r); u8(0x8d); modrm_rsp(r, disp); }
    void store_imm32_rsp(int32_t disp, uint32_t v) { u8(0xc7); modrm_rsp(0, disp); u32(v); }

    // Widening 32-bit loads: movzx/movsx r32, byte/word [rbp + disp32].
    void load_ext_rbp(int r, int32_t disp, ArgKind k) {
        if (r >= 8) u8(0x44);
        u8(0x0f);
        switch (k) {
            case ArgKind::Bool: u8(0xb6); break;   // movzx r32, m8
            case ArgKind::Byte: u8(0xbe); break;   // movsx r32, m8
            case ArgKind::Char: u8(0xb7); break;   // movzx r32, m16
            case ArgKind::Short: u8(0xbf); break;  // movsx r32, m16
            default: break;
        }
        modrm_rbp(r, disp);
    }

    // movsd xmm <-> [rbp + disp32] (F2 0F 10 / 11), movdqu xmm <-> [rsp + disp32] (F3 0F 6F / 7F).
    void movsd_load_rbp(int x, int32_t disp) { u8(0xf2); rex_r(x); u8(0x0f); u8(0x10); modrm_rbp(x, disp); }
    void movsd_store_rbp(int x, int32_t disp) { u8(0xf2); rex_r(x); u8(0x0f); u8(0x11); modrm_rbp(x, disp); }
    void movdqu_load_rsp(int x, int32_t disp) { u8(0xf3); rex_r(x); u8(0x0f); u8(0x6f); modrm_rsp(x, disp); }
    void movdqu_store_rsp(int x, int32_t disp) { u8(0xf3); rex_r(x); u8(0x0f); u8(0x7f); modrm_rsp(x, disp); }

private:
    void rex_w(int r) { u8(0x48 | (r >= 8 ? 4 : 0)); }
    void rex_r(int r) { if (r >= 8) u8(0x44); }
    void modrm_rbp(int r, int32_t disp) { u8(0x80 | ((r & 7) << 3) | 5); u32(static_cast<uint32_t>(disp)); }
    void modrm_rsp(int r, int32_t disp) { u8(0x80 | ((r & 7) << 3) | 4); u8(0x24); u32(static_cast<uint32_t>(disp)); }

    std::vector<uint8_t> b_;
};

// Bump allocator over RWX chunks. Thunks are never freed: they live as long
// as the native method or JNI table entry that points at them.
class ThunkPool {
public:
    void* place(const std::vector<uint8_t>& code) {
        std::lock_guard<std::mutex> lock(mu_);
        const size_t need = (code.size() + 15) & ~size_t{15};
        if (!cur_ || used_ + need > kChunk) {
            cur_ = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, kChunk, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            if (!cur_) return nullptr;
            used_ = 0;
        }
        uint8_t* p = cur_ + used_;
        std::memcpy(p, code.data(), code.size());
        used_ += need;
        FlushInstructionCache(GetCurrentProcess(), p, code.size());
        return p;
    }

private:
    static constexpr size_t kChunk = 64 * 1024;
    std::mutex mu_;
    uint8_t* cur_ = nullptr;
    size_t used_ = 0;
};

ThunkPool& pool() {
    static ThunkPool p;
    return p;
}

bool is_sse(ArgKind k) { return k == ArgKind::Float || k == ArgKind::Double; }
bool is_narrow(ArgKind k) {
    return k == ArgKind::Bool || k == ArgKind::Byte || k == ArgKind::Char || k == ArgKind::Short;
}

}  // namespace

// Frame of a native-method thunk (Microsoft x64 caller -> System V callee):
//
//   [rbp + 48 + 8*(i-4)]   caller's stack arguments 4..n-1
//   [rbp + 16 .. 48)       caller's home area: args 0..3 spilled here, so
//                          every incoming argument i sits at [rbp + 16 + 8*i]
//   [rbp + 8]              return address
//   [rbp]                  saved rbp
//   [rbp - 8], [rbp - 16]  saved rsi, rdi (callee-saved in Microsoft x64 only)
//   [rsp + out .. +160)    saved xmm6-xmm15 (callee-saved in Microsoft x64 only)
//   [rsp .. rsp + out)     outgoing System V stack arguments; also the 32-byte
//                          home area for the env_xlate call made first
void* make_jni_native_thunk(void* target, const ArgKind* args, size_t nargs, void* env_xlate) {
    if (nargs < 1) return nullptr;
    auto in_slot = [](size_t i) { return static_cast<int32_t>(16 + 8 * i); };

    // System V placement: integer class fills rdi..r9, SSE class xmm0..7,
    // the rest goes to the stack in argument order.
    struct Place { bool stack; int reg; int slot; };
    std::vector<Place> place(nargs);
    int nint = 0, nsse = 0, nstack = 0;
    for (size_t i = 0; i < nargs; ++i) {
        if (is_sse(args[i])) {
            place[i] = nsse < 8 ? Place{false, nsse++, 0} : Place{true, 0, nstack++};
        } else {
            place[i] = nint < 6 ? Place{false, kSysVIntRegs[nint++], 0} : Place{true, 0, nstack++};
        }
    }
    uint32_t out = static_cast<uint32_t>(8 * nstack);
    if (out < 32) out = 32;
    out = (out + 15) & ~15u;
    const uint32_t frame = out + 160;  // keeps rsp 16-aligned: entry 8, +rbp, +rsi, +rdi

    Emitter e;
    e.push(RBP);
    e.mov_rbp_rsp();
    for (size_t i = 0; i < nargs && i < 4; ++i) {
        if (is_sse(args[i])) {
            e.movsd_store_rbp(static_cast<int>(i), in_slot(i));
        } else {
            e.store_rbp(kMsIntRegs[i], in_slot(i));
        }
    }
    e.push(RSI);
    e.push(RDI);
    e.sub_rsp(frame);
    for (int k = 0; k < 10; ++k) e.movdqu_store_rsp(6 + k, static_cast<int32_t>(out + 16 * k));

    // args[0] = env_xlate(args[0]), a Microsoft x64 call.
    e.load_rbp(RCX, in_slot(0));
    e.mov_imm64(R11, reinterpret_cast<uint64_t>(env_xlate));
    e.call(R11);
    e.store_rbp(RAX, in_slot(0));

    // Stack arguments first: they go through rax, which is free until the call.
    for (size_t i = 0; i < nargs; ++i) {
        if (!place[i].stack) continue;
        if (is_narrow(args[i])) {
            e.load_ext_rbp(RAX, in_slot(i), args[i]);
        } else {
            e.load_rbp(RAX, in_slot(i));
        }
        e.store_rsp(RAX, 8 * place[i].slot);
    }
    for (size_t i = 0; i < nargs; ++i) {
        if (place[i].stack) continue;
        if (is_sse(args[i])) {
            e.movsd_load_rbp(place[i].reg, in_slot(i));
        } else if (is_narrow(args[i])) {
            e.load_ext_rbp(place[i].reg, in_slot(i), args[i]);
        } else {
            e.load_rbp(place[i].reg, in_slot(i));
        }
    }
    e.mov_eax_imm32(static_cast<uint32_t>(nsse));  // al = vector registers used
    e.mov_imm64(R11, reinterpret_cast<uint64_t>(target));
    e.call(R11);

    // rax / xmm0 carry the result unchanged: both ABIs return there.
    for (int k = 0; k < 10; ++k) e.movdqu_load_rsp(6 + k, static_cast<int32_t>(out + 16 * k));
    e.add_rsp(frame);
    e.pop(RDI);
    e.pop(RSI);
    e.pop(RBP);
    e.ret();
    return pool().place(e.bytes());
}

// Frame of a variadic entry (System V caller -> System V handler):
//
//   [rbp + 16 ...]         caller's stack arguments (overflow_arg_area)
//   [rsp + 192]            SysVVaList.reg_save_area   = rsp
//   [rsp + 184]            SysVVaList.overflow_arg_area = rbp + 16
//   [rsp + 180]            SysVVaList.fp_offset = 48 (no named SSE arguments)
//   [rsp + 176]            SysVVaList.gp_offset = 8 * named
//   [rsp + 48 .. 176)      xmm0..xmm7
//   [rsp + 0 .. 48)        rdi rsi rdx rcx r8 r9
//
// This is the block va_start would have built; the handler reads it with
// sysv_va_next_gp/fp.
void* make_sysv_variadic_entry(int named, void* handler) {
    if (named < 0 || named > 5) return nullptr;
    Emitter e;
    e.push(RBP);
    e.mov_rbp_rsp();
    e.sub_rsp(208);
    for (int i = 0; i < 6; ++i) e.store_rsp(kSysVIntRegs[i], 8 * i);
    for (int k = 0; k < 8; ++k) e.movdqu_store_rsp(k, 48 + 16 * k);
    e.store_imm32_rsp(176, static_cast<uint32_t>(8 * named));
    e.store_imm32_rsp(180, 48);
    e.lea_rbp(RAX, 16);
    e.store_rsp(RAX, 184);
    e.lea_rsp(RAX, 0);
    e.store_rsp(RAX, 192);
    // The fixed arguments are still in their registers; the va_list pointer
    // becomes the next integer argument.
    e.lea_rsp(kSysVIntRegs[named], 176);
    e.mov_imm64(R11, reinterpret_cast<uint64_t>(handler));
    e.call(R11);
    e.leave();
    e.ret();
    return pool().place(e.bytes());
}

uint64_t sysv_va_next_gp(SysVVaList* va) {
    uint64_t v;
    if (va->gp_offset < 48) {
        std::memcpy(&v, va->reg_save_area + va->gp_offset, sizeof(v));
        va->gp_offset += 8;
    } else {
        std::memcpy(&v, va->overflow_arg_area, sizeof(v));
        va->overflow_arg_area += 8;
    }
    return v;
}

double sysv_va_next_fp(SysVVaList* va) {
    double v;
    if (va->fp_offset < 176) {
        std::memcpy(&v, va->reg_save_area + va->fp_offset, sizeof(v));
        va->fp_offset += 16;
    } else {
        std::memcpy(&v, va->overflow_arg_area, sizeof(v));
        va->overflow_arg_area += 8;
    }
    return v;
}

}  // namespace winapk
