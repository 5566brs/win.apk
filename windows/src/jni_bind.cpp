// jni_bind.cpp -- see jni_bind.h.

#include "winapk/jni_bind.h"

#include "winapk/guest_jni.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <jvmti.h>

namespace winapk {

namespace {

std::mutex g_thunk_lock;
std::map<std::pair<void*, std::string>, void*> g_thunks;

GuestCodePredicate g_is_guest = nullptr;

std::mutex g_sig_lock;
std::unordered_map<jmethodID, std::string> g_sigs;  // node-based: c_str() stays valid

// Skips one field descriptor at `p`; returns nullptr if it is malformed.
const char* skip_field(const char* p) {
    while (*p == '[') ++p;
    switch (*p) {
        case 'Z': case 'B': case 'C': case 'S': case 'I': case 'J': case 'F': case 'D':
            return p + 1;
        case 'L': {
            const char* end = p + 1;
            while (*end && *end != ';') ++end;
            return (*end == ';' && end > p + 1) ? end + 1 : nullptr;
        }
        default:
            return nullptr;
    }
}

ArgKind kind_of(const char* field) {
    switch (*field) {
        case 'Z': return ArgKind::Bool;
        case 'B': return ArgKind::Byte;
        case 'C': return ArgKind::Char;
        case 'S': return ArgKind::Short;
        case 'F': return ArgKind::Float;
        case 'D': return ArgKind::Double;
        default: return ArgKind::Word;  // I, J, objects and arrays
    }
}

void JNICALL on_native_method_bind(jvmtiEnv* jvmti, JNIEnv*, jthread, jmethodID method, void* address,
                                   void** new_address) {
    if (!g_is_guest || !g_is_guest(address)) return;
    const char* sig = jvmti_method_signature(jvmti, method);
    void* thunk = sig ? guest_native_thunk(address, sig) : nullptr;
    if (!thunk) {
        // Binding the raw System V address would crash on the first call, far
        // from the cause. Leave it, but say why.
        std::fprintf(stderr, "[win.apk] cannot bind guest native %p (signature %s)\n", address,
                     sig ? sig : "unknown");
        std::fflush(stderr);
        return;
    }
    *new_address = thunk;
}

}  // namespace

bool jni_arg_kinds(const char* sig, std::vector<ArgKind>* out) {
    out->assign({ArgKind::Word, ArgKind::Word});
    if (!sig || *sig != '(') return false;
    const char* p = sig + 1;
    while (*p != ')') {
        const char* next = skip_field(p);
        if (!next) return false;
        out->push_back(*p == '[' ? ArgKind::Word : kind_of(p));
        p = next;
    }
    ++p;
    return (*p == 'V' && p[1] == '\0') || ((p = skip_field(p)) && *p == '\0');
}

void* guest_native_thunk(void* fn, const char* sig) {
    std::vector<ArgKind> kinds;
    if (!fn || !jni_arg_kinds(sig, &kinds)) return nullptr;
    std::lock_guard<std::mutex> hold(g_thunk_lock);
    auto key = std::make_pair(fn, std::string(sig));
    auto it = g_thunks.find(key);
    if (it != g_thunks.end()) return it->second;
    void* thunk = make_jni_native_thunk(fn, kinds.data(), kinds.size(),
                                        reinterpret_cast<void*>(&guest_env_wrapping));
    if (thunk) g_thunks.emplace(std::move(key), thunk);
    return thunk;
}

WINAPK_GUEST_ABI jint guest_register_natives(JNIEnv* env, jclass clazz, const JNINativeMethod* methods,
                                             jint count) {
    JNIEnv* host = host_env_of(env);
    if (!host) host = env;
    if (count < 0 || (count > 0 && !methods)) return JNI_ERR;
    std::vector<JNINativeMethod> wrapped(methods, methods + count);
    for (JNINativeMethod& m : wrapped) {
        void* thunk = guest_native_thunk(m.fnPtr, m.signature);
        if (!thunk) {
            // HotSpot throws NoSuchMethodError for a method it cannot bind;
            // a descriptor we cannot read is the same failure to the caller.
            std::string what = std::string(m.name ? m.name : "?") + (m.signature ? m.signature : "");
            jclass err = host->FindClass("java/lang/NoSuchMethodError");
            if (err) host->ThrowNew(err, what.c_str());
            return JNI_ERR;
        }
        m.fnPtr = thunk;
    }
    return host->RegisterNatives(clazz, wrapped.data(), count);
}

jint install_native_method_bind(JavaVM* host_vm, GuestCodePredicate is_guest) {
    jvmtiEnv* jvmti = nullptr;
    if (host_vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK || !jvmti) {
        return JNI_ERR;
    }
    g_is_guest = is_guest;
    jvmtiCapabilities caps{};
    caps.can_generate_native_method_bind_events = 1;
    jvmtiError e = jvmti->AddCapabilities(&caps);
    if (e != JVMTI_ERROR_NONE) return e;
    jvmtiEventCallbacks callbacks{};
    callbacks.NativeMethodBind = &on_native_method_bind;
    e = jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks));
    if (e != JVMTI_ERROR_NONE) return e;
    e = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_NATIVE_METHOD_BIND, nullptr);
    return e == JVMTI_ERROR_NONE ? JNI_OK : e;
}

const char* jvmti_method_signature(void* ctx, jmethodID mid) {
    auto* jvmti = static_cast<jvmtiEnv*>(ctx);
    if (!jvmti || !mid) return nullptr;
    {
        std::lock_guard<std::mutex> hold(g_sig_lock);
        auto it = g_sigs.find(mid);
        if (it != g_sigs.end()) return it->second.c_str();
    }
    char* sig = nullptr;
    if (jvmti->GetMethodName(mid, nullptr, &sig, nullptr) != JVMTI_ERROR_NONE || !sig) return nullptr;
    std::lock_guard<std::mutex> hold(g_sig_lock);
    const char* out = g_sigs.emplace(mid, sig).first->second.c_str();
    jvmti->Deallocate(reinterpret_cast<unsigned char*>(sig));
    return out;
}

}  // namespace winapk
