// native_log.cpp -- com.macapk.loader.NativeLog on Windows.

#include "host_natives.h"

#include <cstdio>
#include <cstdlib>

#include <windows.h>

namespace winapk::host {

namespace {

// MACAPK_LOG_LEVEL gates the output the same way upstream's Log_print does:
// V=2 D=3 I=4 W=5 E=6, default everything.
int log_min_level() {
    static const int level = [] {
        const char* e = std::getenv("MACAPK_LOG_LEVEL");
        return (e && *e) ? std::atoi(e) : 0;
    }();
    return level;
}

void JNICALL Log_print(JNIEnv* env, jclass, jint level, jstring tag_j, jstring msg_j) {
    if (level < log_min_level()) return;
    const char* tag = tag_j ? env->GetStringUTFChars(tag_j, nullptr) : "";
    const char* msg = msg_j ? env->GetStringUTFChars(msg_j, nullptr) : "";
    const char* lv = level <= 2 ? "V" : level == 3 ? "D" : level == 4 ? "I" : level == 5 ? "W" : "E";
    std::fprintf(stderr, "[%s] %s: %s\n", lv, tag, msg);
    if (tag_j) env->ReleaseStringUTFChars(tag_j, tag);
    if (msg_j) env->ReleaseStringUTFChars(msg_j, msg);
}

// Nanoseconds since the host process started, for Boot's timing lines.
jlong JNICALL Log_bootTimeNanos(JNIEnv*, jclass) {
    static const ULONGLONG start = [] {
        FILETIME created, exited, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        return (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }();
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    const ULONGLONG t = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return static_cast<jlong>((t - start) * 100);  // FILETIME ticks are 100 ns
}

// Upstream raises the calling thread to the interactive QoS class, so that a
// busy app cannot starve the thread that drives frames and animations. The
// Windows lever is the thread priority. Called on the thread to boost.
void JNICALL Log_boostCurrentThreadQos(JNIEnv*, jclass) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

// Upstream's JNI-call tracer is a Mac debugging aid with no Windows
// counterpart; the call is a no-op there too unless MACAPK_TRACE_JNI is set.
void JNICALL Log_installJniTrace(JNIEnv*, jclass) {}

// The pairip integrity-gate emulation (MACAPK_PAIRIP_GATE_EMU) is not ported;
// false tells Boot the rebind did not happen, so the app's own VMRunner stays.
jboolean JNICALL Log_registerPairipGate(JNIEnv*, jclass, jclass) { return JNI_FALSE; }

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE("com.macapk.loader.NativeLog", "print", "(ILjava/lang/String;Ljava/lang/String;)V",
                       Log_print),
    WINAPK_HOST_NATIVE("com.macapk.loader.NativeLog", "nativeBootTimeNanos", "()J", Log_bootTimeNanos),
    WINAPK_HOST_NATIVE("com.macapk.loader.NativeLog", "nativeBoostCurrentThreadQos", "()V",
                       Log_boostCurrentThreadQos),
    WINAPK_HOST_NATIVE("com.macapk.loader.NativeLog", "nativeInstallJniTrace", "()V", Log_installJniTrace),
    WINAPK_HOST_NATIVE("com.macapk.loader.NativeLog", "nativeRegisterPairipGate", "(Ljava/lang/Class;)Z",
                       Log_registerPairipGate),
};

}  // namespace

HostMethods native_log_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
