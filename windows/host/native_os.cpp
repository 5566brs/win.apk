// native_os.cpp -- com.macapk.loader.NativeOs on Windows (so far: the
// environment).
//
// android.system.Os.setenv exists for native code: an app sets a variable so
// its own library reads it with getenv(3) (AnkiDroid's Rust backend is
// upstream's measured case). The calls therefore go to the guest's
// environment in the Bionic shim, the one guest getenv reads, as upstream's
// runtime/src/native_os.cpp sends them to its shim on the Mac.

#include "host_natives.h"

#include <string>

#include "winapk/mini_bionic.h"

namespace winapk::host {

namespace {

std::string utf8(JNIEnv* env, jstring s) {
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

constexpr jint kEINVAL = 22;

jint JNICALL Os_setenv(JNIEnv* env, jclass, jstring name, jstring value, jboolean overwrite) {
    if (!name || !value) return -kEINVAL;
    return winapk::bionic_setenv(utf8(env, name).c_str(), utf8(env, value).c_str(), overwrite ? 1 : 0);
}

jstring JNICALL Os_getenv(JNIEnv* env, jclass, jstring name) {
    if (!name) return nullptr;
    const char* v = winapk::bionic_getenv(utf8(env, name).c_str());
    return v ? env->NewStringUTF(v) : nullptr;
}

jint JNICALL Os_unsetenv(JNIEnv* env, jclass, jstring name) {
    if (!name) return -kEINVAL;
    return winapk::bionic_unsetenv(utf8(env, name).c_str());
}

constexpr const char* kCls = "com.macapk.loader.NativeOs";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "setenv", "(Ljava/lang/String;Ljava/lang/String;Z)I", Os_setenv),
    WINAPK_HOST_NATIVE(kCls, "getenv", "(Ljava/lang/String;)Ljava/lang/String;", Os_getenv),
    WINAPK_HOST_NATIVE(kCls, "unsetenv", "(Ljava/lang/String;)I", Os_unsetenv),
};

}  // namespace

HostMethods native_os_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
