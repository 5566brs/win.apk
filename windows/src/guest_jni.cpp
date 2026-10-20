// guest_jni.cpp -- see guest_jni.h.
//
// The 235 JNINativeInterface_ slots fall in three groups (windows/src/jni_slots.inc):
//
//   * non-variadic (WINAPK_JNI_PLAIN): forwarded by a single template whose
//     signature is deduced from the JNINativeInterface_ field itself, so every
//     one of them is generated, not hand-written. RegisterNatives is the one
//     exception: it wraps the guest's function pointers first (jni_bind.h).
//   * the Call<T>Method(env, obj, mid, ...) families (WINAPK_JNI_CALL) and
//     their va_list variants (WINAPK_JNI_CALLV): bridged by jni_variadic.cpp
//     once a signature provider is set, a loud-abort stub until then.
//   * reserved: left null, as HotSpot leaves them.
//
// The guest table is shared by every thread (the forwarders are stateless).
// The per-thread part is the GuestEnv wrapper, which carries the host JNIEnv*
// this thread is attached to.

#include "winapk/guest_jni.h"

#include "winapk/guest_abi.h"
#include "winapk/jni_bind.h"

#include <cstdio>
#include <cstdlib>

namespace winapk {

namespace {

// A guest JNIEnv is this struct, handed to guest code as a JNIEnv*. Its first
// member matches JNIEnv_ (a lone `functions` pointer), so a GuestEnv* and a
// JNIEnv* are interconvertible by reinterpret_cast.
struct GuestEnv {
    const JNINativeInterface_* functions;  // -> the shared guest table
    JNIEnv* host;                          // HotSpot's env for this thread
    bool alive;
};

// A guest JavaVM, likewise interconvertible with JavaVM_.
struct GuestVM {
    const JNIInvokeInterface_* functions;  // -> the guest invoke table
    JavaVM* host;                          // HotSpot's JavaVM
};

JavaVM* g_host_vm = nullptr;
GuestVM g_guest_vm;
JNINativeInterface_ g_guest_table;      // filled by build_guest_table()
JNIInvokeInterface_ g_guest_invoke;     // filled by guest_jni_init()
bool g_table_built = false;             // init runs once at startup, before threads
SignatureProvider g_sig_provider = nullptr;
void* g_sig_ctx = nullptr;

// One guest env per thread. Its address is stable for the thread's life, which
// is what lets it be handed out as a JNIEnv*.
thread_local GuestEnv t_env;

GuestEnv* guest_env_from(JNIEnv* genv) {
    auto* g = reinterpret_cast<GuestEnv*>(genv);
    return (g && g->functions == &g_guest_table) ? g : nullptr;
}

// The generic forwarder. The partial specialization deduces (R, A...) from the
// JNINativeInterface_ field, so one template covers every non-variadic slot.
// The host env comes from the guest env the slot was called through; a later
// slice may instead take it from the calling thread (the upstream ARM32 model),
// which tolerates engines that cache one env globally.
template <auto Field>
struct Slot;

template <typename R, typename... A, R(JNICALL* JNINativeInterface_::*Field)(JNIEnv*, A...)>
struct Slot<Field> {
    static WINAPK_GUEST_ABI R fn(JNIEnv* genv, A... args) {
        GuestEnv* g = guest_env_from(genv);
        JNIEnv* host = g ? g->host : genv;
        return (host->functions->*Field)(host, args...);
    }
};

// Slot stub for the deferred variadic Call<T>Method families. Declared with
// fixed parameters (not `...`): a System V caller cleans up its own stack, so
// the ignored variadic arguments are harmless, and we never read them.
[[noreturn]] WINAPK_GUEST_ABI void unimplemented_variadic(JNIEnv*) {
    std::fprintf(stderr,
                 "[win.apk] a guest called a variadic JNI Call*Method, which Win.apk does not "
                 "bridge yet (next slice)\n");
    std::fflush(stderr);
    std::abort();
}

void build_guest_table() {
    JNINativeInterface_& t = g_guest_table;
#define WINAPK_JNI_RESERVED(i)
#define WINAPK_JNI_PLAIN(Name) \
    *reinterpret_cast<void**>(&t.Name) = reinterpret_cast<void*>(&Slot<&JNINativeInterface_::Name>::fn);
#define WINAPK_JNI_CALL(Name, Kind, Type) \
    *reinterpret_cast<void**>(&t.Name) = reinterpret_cast<void*>(&unimplemented_variadic);
#define WINAPK_JNI_CALLV(Name, Kind, Type) \
    *reinterpret_cast<void**>(&t.Name) = reinterpret_cast<void*>(&unimplemented_variadic);
#include "jni_slots.inc"
#undef WINAPK_JNI_RESERVED
#undef WINAPK_JNI_PLAIN
#undef WINAPK_JNI_CALL
#undef WINAPK_JNI_CALLV

    // The one plain slot that cannot just forward: the function pointers the
    // guest registers are System V and need a thunk each (jni_bind.h).
    *reinterpret_cast<void**>(&t.RegisterNatives) = reinterpret_cast<void*>(&guest_register_natives);
}

// Updates this thread's guest env to wrap `host` and returns it as a JNIEnv*.
JNIEnv* wrap_host_env(JNIEnv* host) {
    t_env.functions = &g_guest_table;
    t_env.host = host;
    t_env.alive = true;
    return reinterpret_cast<JNIEnv*>(&t_env);
}

// ---- guest JavaVM slots (System V) ---------------------------------------

JavaVM* host_vm_from(JavaVM* vm) {
    auto* g = reinterpret_cast<GuestVM*>(vm);
    return (g && g->functions == &g_guest_invoke) ? g->host : vm;
}

WINAPK_GUEST_ABI jint gv_DestroyJavaVM(JavaVM*) {
    // The host owns the VM lifetime; a guest DestroyJavaVM is a no-op, as the
    // upstream ARM32 proxy does.
    return JNI_OK;
}

WINAPK_GUEST_ABI jint gv_AttachCurrentThread(JavaVM* vm, void** penv, void* args) {
    JavaVM* host = host_vm_from(vm);
    JNIEnv* henv = nullptr;
    jint r = host->AttachCurrentThread(reinterpret_cast<void**>(&henv), args);
    if (r != JNI_OK || !henv) return r;
    *penv = wrap_host_env(henv);
    return JNI_OK;
}

WINAPK_GUEST_ABI jint gv_AttachCurrentThreadAsDaemon(JavaVM* vm, void** penv, void* args) {
    JavaVM* host = host_vm_from(vm);
    JNIEnv* henv = nullptr;
    jint r = host->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&henv), args);
    if (r != JNI_OK || !henv) return r;
    *penv = wrap_host_env(henv);
    return JNI_OK;
}

WINAPK_GUEST_ABI jint gv_DetachCurrentThread(JavaVM* vm) {
    JavaVM* host = host_vm_from(vm);
    jint r = host->DetachCurrentThread();
    t_env.alive = false;
    t_env.host = nullptr;
    return r;
}

WINAPK_GUEST_ABI jint gv_GetEnv(JavaVM* vm, void** penv, jint version) {
    JavaVM* host = host_vm_from(vm);
    JNIEnv* henv = nullptr;
    jint r = host->GetEnv(reinterpret_cast<void**>(&henv), version);
    if (r != JNI_OK || !henv) {
        *penv = nullptr;
        return r;
    }
    *penv = wrap_host_env(henv);
    return JNI_OK;
}

}  // namespace

JavaVM* guest_jni_init(JavaVM* host_vm) {
    g_host_vm = host_vm;
    if (!g_table_built) {
        build_guest_table();
        g_table_built = true;
    }
    if (g_sig_provider) install_variadic_slots(&g_guest_table, g_sig_provider, g_sig_ctx);

    JNIInvokeInterface_& iv = g_guest_invoke;
    iv.reserved0 = nullptr;
    iv.reserved1 = nullptr;
    iv.reserved2 = nullptr;
    *reinterpret_cast<void**>(&iv.DestroyJavaVM) = reinterpret_cast<void*>(&gv_DestroyJavaVM);
    *reinterpret_cast<void**>(&iv.AttachCurrentThread) = reinterpret_cast<void*>(&gv_AttachCurrentThread);
    *reinterpret_cast<void**>(&iv.DetachCurrentThread) = reinterpret_cast<void*>(&gv_DetachCurrentThread);
    *reinterpret_cast<void**>(&iv.GetEnv) = reinterpret_cast<void*>(&gv_GetEnv);
    *reinterpret_cast<void**>(&iv.AttachCurrentThreadAsDaemon) =
        reinterpret_cast<void*>(&gv_AttachCurrentThreadAsDaemon);

    g_guest_vm.functions = &g_guest_invoke;
    g_guest_vm.host = host_vm;
    return reinterpret_cast<JavaVM*>(&g_guest_vm);
}

JavaVM* guest_java_vm() { return g_host_vm ? reinterpret_cast<JavaVM*>(&g_guest_vm) : nullptr; }

JNIEnv* guest_env_for_current_thread() {
    if (t_env.alive && t_env.functions == &g_guest_table) return reinterpret_cast<JNIEnv*>(&t_env);
    if (!g_host_vm) return nullptr;
    JNIEnv* host = nullptr;
    jint r = g_host_vm->GetEnv(reinterpret_cast<void**>(&host), JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        r = g_host_vm->AttachCurrentThread(reinterpret_cast<void**>(&host), nullptr);
    }
    if (r != JNI_OK || !host) return nullptr;
    return wrap_host_env(host);
}

JNIEnv* host_env_of(JNIEnv* guest) {
    GuestEnv* g = guest_env_from(guest);
    return g ? g->host : nullptr;
}

JNIEnv* guest_env_wrapping(JNIEnv* host) { return host ? wrap_host_env(host) : nullptr; }

void guest_jni_set_signature_provider(SignatureProvider provider, void* ctx) {
    g_sig_provider = provider;
    g_sig_ctx = ctx;
    // If the table already exists, install the bridges now; otherwise
    // guest_jni_init will, once it has built the table.
    if (g_table_built) install_variadic_slots(&g_guest_table, provider, ctx);
}

}  // namespace winapk
