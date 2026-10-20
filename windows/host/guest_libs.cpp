// guest_libs.cpp -- loads an app's x86_64 native libraries; see guest_libs.h.

#include "guest_libs.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <windows.h>

#include "winapk/abi_thunk.h"
#include "winapk/guest_jni.h"
#include "winapk/jni_bind.h"
#include "winapk/linker.h"
#include "winapk/mini_bionic.h"

namespace {

std::mutex g_lock;
std::unique_ptr<winapk::GuestLinker> g_linker;
JavaVM* g_guest_vm = nullptr;
// The stub module of each app library System.load()'ed, and its guest library.
std::unordered_map<jlong, winapk::GuestLibrary*> g_by_module;

using FindEntry = jlong(JNICALL*)(JNIEnv*, jclass, jlong, jstring);
FindEntry g_jdk_find_entry = nullptr;

std::string utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

bool is_guest_code(const void* addr) {
    winapk::GuestLinker* linker = winapk::GuestLinker::process();
    return linker && linker->image_containing(addr);
}

// ---- system functions the shim does not have yet ----------------------------

struct MissingTag {
    std::string name;
    bool reported = false;
};

// Called (Microsoft x64, from the tagged stub) by the guest; see
// make_tagged_zero_stub. The guest gets 0.
void report_missing(void*, const void* tag) {
    auto* t = static_cast<MissingTag*>(const_cast<void*>(tag));
    if (t->reported) return;
    t->reported = true;
    std::fprintf(stderr, "[win.apk] guest called a system function not implemented on Windows yet, returning 0: %s\n",
                 t->name.c_str());
    std::fflush(stderr);
}

void* missing_stub(const char* name) {
    static std::mutex lock;
    static std::unordered_map<std::string, void*> stubs;
    std::lock_guard<std::mutex> hold(lock);
    auto it = stubs.find(name);
    if (it != stubs.end()) return it->second;
    auto* tag = new MissingTag{name};
    void* stub = winapk::make_tagged_zero_stub(reinterpret_cast<void*>(&report_missing), tag);
    stubs.emplace(name, stub);
    return stub;
}

// A system variable the shim does not have: zeroed memory, large enough for
// the objects libraries import (FILE arrays, vtables, interface ids).
void* missing_object(const char* name) {
    static std::mutex lock;
    static std::unordered_map<std::string, void*> blocks;
    std::lock_guard<std::mutex> hold(lock);
    auto it = blocks.find(name);
    if (it != blocks.end()) return it->second;
    void* block = std::calloc(1, 4096);
    blocks.emplace(name, block);
    std::fprintf(stderr, "[win.apk] guest uses a system variable not implemented on Windows yet, bound to zeroed memory: %s\n",
                 name);
    return block;
}

// ---- the app library behind a stub module -----------------------------------

// The .so a stub copy stands for: the translator writes its path, UTF-8, to
// <stub>.source.
std::wstring source_of(const std::wstring& stub_path) {
    std::wstring so;
    FILE* f = _wfopen((stub_path + L".source").c_str(), L"rb");
    if (!f) return so;
    std::string bytes;
    char buf[1024];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) bytes.append(buf, n);
    std::fclose(f);
    while (!bytes.empty() && (bytes.back() == '\n' || bytes.back() == '\r')) bytes.pop_back();
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    so.resize(static_cast<size_t>(n > 0 ? n : 0));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &so[0], n);
    return so;
}

// Loads (or finds) the guest library a stub copy stands for, with its
// dependencies and constructors, as the dynamic linker does.
winapk::GuestLibrary* open_for_stub(const std::wstring& stub_path) {
    const std::wstring so = source_of(stub_path);
    if (so.empty()) {
        std::fprintf(stderr, "[win.apk] %s names no .so (no .source file)\n", utf8(stub_path).c_str());
        return nullptr;
    }
    std::string error;
    winapk::GuestLibrary* lib =
        g_linker->open(utf8(so), winapk::GuestLinker::kRtldNow, &error);
    if (!lib) std::fprintf(stderr, "[win.apk] cannot load %s: %s\n", utf8(so).c_str(), error.c_str());
    return lib;
}

// jdk.internal.loader.NativeLibrary.findEntry0(long handle, String name): for
// a stub module, the guest library's symbol (its dependency tree included, as
// dlsym on an Android handle); the NativeMethodBind hook wraps it before
// HotSpot calls it. For any other module, the JDK's own lookup.
jlong JNICALL find_entry(JNIEnv* env, jclass cls, jlong handle, jstring name) {
    winapk::GuestLibrary* lib = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_by_module.find(handle);
        if (it != g_by_module.end()) lib = it->second;
    }
    if (!lib) return g_jdk_find_entry ? g_jdk_find_entry(env, cls, handle, name) : 0;
    const char* n = env->GetStringUTFChars(name, nullptr);
    if (!n) return 0;
    std::string error;
    void* p = g_linker->lookup(lib, n, nullptr, &error);
    env->ReleaseStringUTFChars(name, n);
    return reinterpret_cast<jlong>(p);
}

// com.macapk.loader.NativeLinker.nativeLinkOnly(String path): null when mapped,
// else the error.
jstring JNICALL link_only(JNIEnv* env, jclass, jstring path) {
    if (!path) return env->NewStringUTF("null path");
    const jchar* chars = env->GetStringChars(path, nullptr);
    const std::wstring stub(reinterpret_cast<const wchar_t*>(chars), static_cast<size_t>(env->GetStringLength(path)));
    env->ReleaseStringChars(path, chars);
    return open_for_stub(stub) ? nullptr : env->NewStringUTF("cannot load the library (see the log)");
}

const winapk::host::HostMethod kLinkerMethods[] = {
    WINAPK_HOST_NATIVE("com.macapk.loader.NativeLinker", "nativeLinkOnly", "(Ljava/lang/String;)Ljava/lang/String;",
                       link_only),
};

}  // namespace

namespace winapk::host {

bool guest_libs_init(JNIEnv* env, jvmtiEnv* jvmti) {
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    // Upstream's order: the guest JNI view and the bind hook exist before the
    // first guest library is loaded.
    winapk::guest_jni_set_signature_provider(winapk::jvmti_method_signature, jvmti);
    g_guest_vm = winapk::guest_jni_init(vm);
    if (winapk::install_native_method_bind(vm, is_guest_code) != JNI_OK) {
        std::fprintf(stderr, "[win.apk] cannot install the NativeMethodBind hook\n");
        return false;
    }

    winapk::LinkerConfig config;
    config.host = winapk::mini_bionic_resolve;
    config.load.unresolved = missing_stub;
    config.load.unresolved_object = missing_object;
    g_linker = std::make_unique<winapk::GuestLinker>(config);

    // HotSpot's lookup of a native method by name ends in this static native.
    HMODULE java_dll = GetModuleHandleW(L"java.dll");
    g_jdk_find_entry = java_dll ? reinterpret_cast<FindEntry>(GetProcAddress(
                                      java_dll, "Java_jdk_internal_loader_NativeLibrary_findEntry0"))
                                : nullptr;
    jclass cls = env->FindClass("jdk/internal/loader/NativeLibrary");
    JNINativeMethod m{const_cast<char*>("findEntry0"), const_cast<char*>("(JLjava/lang/String;)J"),
                      reinterpret_cast<void*>(&find_entry)};
    if (!g_jdk_find_entry || !cls || env->RegisterNatives(cls, &m, 1) != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        std::fprintf(stderr, "[win.apk] cannot hook NativeLibrary.findEntry0; app natives bound by name will not resolve\n");
        return false;
    }
    return true;
}

HostMethods native_linker_methods() {
    return {kLinkerMethods, sizeof(kLinkerMethods) / sizeof(kLinkerMethods[0])};
}

}  // namespace winapk::host

extern "C" jint winapk_guest_library_loaded(JavaVM*, void* module, const wchar_t* stub_path) {
    if (!g_linker) return JNI_ERR;
    winapk::GuestLibrary* lib = open_for_stub(stub_path);
    if (!lib) return JNI_ERR;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_by_module[reinterpret_cast<jlong>(module)] = lib;
    }
    // System.loadLibrary's second half on Android: the library's own
    // JNI_OnLoad, found with dlsym on its handle, called with the JavaVM.
    using OnLoad = jint(WINAPK_GUEST_ABI*)(JavaVM*, void*);
    std::string error;
    auto on_load = reinterpret_cast<OnLoad>(g_linker->lookup(lib, "JNI_OnLoad", nullptr, &error));
    if (!on_load) return JNI_VERSION_1_6;
    const jint version = on_load(g_guest_vm, nullptr);
    // JNI_OnLoad's address lets a guest crash address in a log be mapped
    // back to the library (its st_value gives the load bias).
    std::fprintf(stderr, "[win.apk] %s: JNI_OnLoad (at %p) returned 0x%x\n", utf8(source_of(stub_path)).c_str(),
                 reinterpret_cast<void*>(on_load), static_cast<unsigned>(version));
    return version;
}
