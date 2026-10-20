// jvmrun_main.cpp -- winapk-jvm-run, the real-JVM end-to-end test.
//
// Creates a real HotSpot JVM in-process, loads the Android x86_64 test library
// (windows/test/guest/jni_guest.c) with the Win.apk ELF loader, binds its
// native method to WinApkJniTest.nativeAdd through a host->guest native-method
// thunk, and calls it from Java. The native implementation calls back into Java
// (WinApkJniTest.javaMultiply) through the guest JNIEnv, so nativeAdd(3, 4)
// returns (3 + 4) + (3 * 4) = 19 only if the whole stack works: loader,
// native-method thunk, guest JNIEnv, the variadic Call bridge, and env
// translation in both directions.
//
//   winapk-jvm-run <jni_guest.so> <classpath-dir>
//
// jvm.dll is found on PATH (CI adds the JDK; locally WINEPATH does).

#include "winapk/abi_thunk.h"
#include "winapk/elf_loader.h"
#include "winapk/guest_jni.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <jni.h>
#include <jvmti.h>
#include <windows.h>

#include "winapk/mini_bionic.h"

namespace {

jvmtiEnv* g_jvmti = nullptr;

// SignatureProvider: JVMTI knows a method's signature from its jmethodID.
const char* signature_of(void*, jmethodID mid) {
    if (!g_jvmti || !mid) return nullptr;
    char* name = nullptr;
    char* sig = nullptr;
    if (g_jvmti->GetMethodName(mid, &name, &sig, nullptr) != JVMTI_ERROR_NONE) return nullptr;
    // Leak name/sig for simplicity: the test calls a bounded number of methods.
    if (name) g_jvmti->Deallocate(reinterpret_cast<unsigned char*>(name));
    return sig;  // caller reads it synchronously, before the next lookup
}

// Builds the native-method thunk argument kinds for a JNI signature: the two
// leading pointers (JNIEnv*, jclass/jobject) then one per Java parameter.
std::vector<winapk::ArgKind> arg_kinds(const char* sig) {
    std::vector<winapk::ArgKind> k{winapk::ArgKind::Word, winapk::ArgKind::Word};
    if (!sig || sig[0] != '(') return k;
    const char* p = sig + 1;
    while (*p && *p != ')') {
        char c = *p++;
        bool is_array = false;
        while (c == '[') { is_array = true; c = *p++; }
        if (is_array || c == 'L') {
            if (c == 'L') { while (*p && *p != ';') ++p; if (*p == ';') ++p; }
            k.push_back(winapk::ArgKind::Word);
        } else {
            switch (c) {
                case 'Z': k.push_back(winapk::ArgKind::Bool); break;
                case 'B': k.push_back(winapk::ArgKind::Byte); break;
                case 'C': k.push_back(winapk::ArgKind::Char); break;
                case 'S': k.push_back(winapk::ArgKind::Short); break;
                case 'F': k.push_back(winapk::ArgKind::Float); break;
                case 'D': k.push_back(winapk::ArgKind::Double); break;
                case 'I': case 'J': default: k.push_back(winapk::ArgKind::Word); break;
            }
        }
    }
    return k;
}

int fail(const char* msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return fail("usage: winapk-jvm-run <guest.so> <classpath-dir>");
    const char* so_path = argv[1];
    const std::string cp = std::string("-Djava.class.path=") + argv[2];

    HMODULE jvm = LoadLibraryW(L"jvm.dll");
    if (!jvm) return fail("LoadLibrary jvm.dll (is the JDK on PATH?)");
    auto create = reinterpret_cast<jint(JNICALL*)(JavaVM**, void**, void*)>(
        GetProcAddress(jvm, "JNI_CreateJavaVM"));
    if (!create) return fail("no JNI_CreateJavaVM");

    JavaVMOption opt;
    opt.optionString = const_cast<char*>(cp.c_str());
    opt.extraInfo = nullptr;
    JavaVMInitArgs vm_args;
    vm_args.version = JNI_VERSION_1_8;
    vm_args.nOptions = 1;
    vm_args.options = &opt;
    vm_args.ignoreUnrecognized = JNI_FALSE;

    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    if (create(&vm, reinterpret_cast<void**>(&env), &vm_args) != JNI_OK)
        return fail("JNI_CreateJavaVM");
    if (vm->GetEnv(reinterpret_cast<void**>(&g_jvmti), JVMTI_VERSION_1_2) != JNI_OK)
        return fail("GetEnv(JVMTI)");

    // Wire up the Win.apk guest JNI view backed by this real VM.
    winapk::guest_jni_set_signature_provider(signature_of, nullptr);
    winapk::guest_jni_init(vm);

    // Load the Android x86_64 library.
    std::string err;
    auto img = winapk::ElfImage::load(so_path, winapk::mini_bionic_resolve, &err);
    if (!img) return fail(("load guest .so: " + err).c_str());
    void* native = img->dlsym("Java_WinApkJniTest_nativeAdd");
    if (!native) return fail("guest does not export Java_WinApkJniTest_nativeAdd");

    jclass klass = env->FindClass("WinApkJniTest");
    if (!klass) return fail("FindClass WinApkJniTest (classpath?)");

    // Bind the native through a host->guest thunk built from its signature.
    const char* sig = "(II)I";
    std::vector<winapk::ArgKind> kinds = arg_kinds(sig);
    void* thunk = winapk::make_jni_native_thunk(native, kinds.data(), kinds.size(),
                                                reinterpret_cast<void*>(&winapk::guest_env_wrapping));
    if (!thunk) return fail("make_jni_native_thunk");
    JNINativeMethod m{const_cast<char*>("nativeAdd"), const_cast<char*>(sig), thunk};
    if (env->RegisterNatives(klass, &m, 1) != JNI_OK) return fail("RegisterNatives");

    jmethodID add = env->GetStaticMethodID(klass, "nativeAdd", sig);
    if (!add) return fail("GetStaticMethodID nativeAdd");
    jint r = env->CallStaticIntMethod(klass, add, 3, 4);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); return fail("exception in nativeAdd"); }

    std::printf("Win.apk real-JVM end-to-end\n");
    std::printf("  nativeAdd(3, 4) = %d (expect 19 = (3+4) + 3*4)\n", r);
    int failures = (r == 19) ? 0 : 1;
    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);

    vm->DestroyJavaVM();
    return failures;
}
