// native_cocoa.cpp -- com.macapk.loader.NativeCocoa on Windows, headless.
//
// NativeCocoa is upstream's window system: the window, the surfaces the app
// draws into, the run loop and process exit. Milestone M2 puts a real Win32
// window behind it. Until then this is the headless version M1 needs: the
// screen metrics are fixed, windows and views are plain handle numbers, and
// the run loop blocks until the app exits.
//
// Drawing is driven from here, as on the Mac, where the display link calls the
// renderer the app's window registered (setRenderer) once per vsync: the
// headless version runs that tick on its own 60 Hz thread. Frames come back
// through presentBitmap. With WINAPK_HEADLESS_FRAME set to a
// file path, the latest one is written there as a BMP, which is how a headless
// run shows that the app really drew something. WINAPK_HEADLESS_EXIT_AFTER_FRAMES
// ends the process with exit code 0 once that many frames were presented,
// which is what the CI smoke test waits for.

#include "host_natives.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include <windows.h>

namespace winapk::host {

namespace {

// Upstream's default phone size (cocoa_bridge.mm), in points.
std::atomic<int> g_phone_w{360};
std::atomic<int> g_phone_h{640};
std::atomic<jlong> g_next_handle{1};
std::atomic<long> g_frames{0};

jlong new_handle() { return g_next_handle.fetch_add(1); }

jboolean JNICALL Cocoa_init(JNIEnv*, jclass) { return JNI_TRUE; }

void JNICALL Cocoa_setPhoneSize(JNIEnv*, jclass, jint w, jint h) {
    if (w > 0) g_phone_w = w;
    if (h > 0) g_phone_h = h;
}

jint JNICALL Cocoa_phoneWidth(JNIEnv*, jclass) { return g_phone_w; }
jint JNICALL Cocoa_phoneHeight(JNIEnv*, jclass) { return g_phone_h; }
jint JNICALL Cocoa_displayHz(JNIEnv*, jclass) { return 60; }
jfloat JNICALL Cocoa_backingScale(JNIEnv*, jclass) { return 1.0f; }

// Same rule as upstream: the short side stays the short side.
void JNICALL Cocoa_applyRequestedOrientation(JNIEnv*, jclass, jboolean landscape) {
    const int a = g_phone_w, b = g_phone_h;
    const int lo = a < b ? a : b, hi = a < b ? b : a;
    g_phone_w = landscape ? hi : lo;
    g_phone_h = landscape ? lo : hi;
}

jlong JNICALL Cocoa_createWindow(JNIEnv*, jclass, jstring) { return new_handle(); }
jlong JNICALL Cocoa_createView(JNIEnv*, jclass, jint, jint) { return new_handle(); }
jlong JNICALL Cocoa_getPlaceholderWindow(JNIEnv*, jclass) {
    static const jlong placeholder = new_handle();
    return placeholder;
}

// ---- the render tick ------------------------------------------------------

JavaVM* g_vm = nullptr;
std::mutex g_renderer_lock;
jobject g_renderer = nullptr;  // global ref to a MetalSurfaceCallbacks
jlong g_renderer_view = 0;
bool g_surface_announced = false;

// The surface is the phone size in pixels, at the density the Java side uses
// for the display metrics (DisplayConfig.seedFromCanvas).
float device_density(JNIEnv* env) {
    jclass cls = env->FindClass("android/util/DisplayMetrics");
    jmethodID m = cls ? env->GetStaticMethodID(cls, "macapkDeviceDensity", "()F") : nullptr;
    const float d = m ? env->CallStaticFloatMethod(cls, m) : 0.0f;
    if (env->ExceptionCheck()) env->ExceptionClear();
    return d > 0 ? d : 1.0f;
}

bool check_tick_exception(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    std::fprintf(stderr, "[win.apk] exception in the render tick:\n");
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

DWORD WINAPI render_thread(void*) {
    JNIEnv* env = nullptr;
    JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("winapk-render"), nullptr};
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) return 1;
    const float density = device_density(env);
    jmethodID changed = nullptr, draw = nullptr;
    for (;;) {
        Sleep(16);
        jobject renderer = nullptr;
        bool announce = false;
        {
            std::lock_guard<std::mutex> hold(g_renderer_lock);
            if (!g_renderer) continue;
            renderer = env->NewLocalRef(g_renderer);
            announce = !g_surface_announced;
            g_surface_announced = true;
        }
        if (!changed) {
            jclass cls = env->FindClass("com/macapk/loader/MetalSurfaceCallbacks");
            changed = env->GetMethodID(cls, "macapkSurfaceChanged", "(II)V");
            draw = env->GetMethodID(cls, "macapkDrawFrame", "()V");
            env->DeleteLocalRef(cls);
        }
        if (announce) {
            env->CallVoidMethod(renderer, changed, static_cast<jint>(g_phone_w * density + 0.5f),
                                static_cast<jint>(g_phone_h * density + 0.5f));
            check_tick_exception(env);
        }
        env->CallVoidMethod(renderer, draw);
        check_tick_exception(env);
        env->DeleteLocalRef(renderer);
    }
}

void JNICALL Cocoa_setRenderer(JNIEnv* env, jclass, jlong view, jobject callbacks) {
    static bool started = false;
    std::lock_guard<std::mutex> hold(g_renderer_lock);
    if (g_renderer) env->DeleteGlobalRef(g_renderer);
    g_renderer = callbacks ? env->NewGlobalRef(callbacks) : nullptr;
    g_renderer_view = view;
    g_surface_announced = false;
    if (!started && g_renderer) {
        started = true;
        env->GetJavaVM(&g_vm);
        HANDLE t = CreateThread(nullptr, 16 * 1024 * 1024, render_thread, nullptr,
                                STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (t) CloseHandle(t);
    }
}

void JNICALL Cocoa_releaseRenderer(JNIEnv* env, jclass, jlong view) {
    std::lock_guard<std::mutex> hold(g_renderer_lock);
    if (g_renderer && view == g_renderer_view) {
        env->DeleteGlobalRef(g_renderer);
        g_renderer = nullptr;
    }
}

// Upstream hands the boot thread to [NSApp run] here. Headless, there is
// nothing to run, and the app's own threads carry on; the process ends through
// exitProcess.
void JNICALL Cocoa_runLoop(JNIEnv*, jclass) {
    for (;;) Sleep(INFINITE);
}

// Upstream ends the process with _exit: no Java shutdown, no atexit. The
// Windows equivalent is TerminateProcess on ourselves.
void JNICALL Cocoa_exitProcess(JNIEnv*, jclass, jint code, jstring) {
    std::fflush(stdout);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
}

void JNICALL Cocoa_quitBackstopExit(JNIEnv* env, jclass cls) { Cocoa_exitProcess(env, cls, 0, nullptr); }

// Writes 32-bit ARGB pixels as a top-down BMP, which keeps the byte order
// (BGRA in memory) and needs no encoder.
bool write_bmp(const wchar_t* path, const jint* argb, int w, int h) {
    const uint32_t pixels = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4d42;  // "BM"
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + pixels;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(f, &fh, sizeof(fh), &n, nullptr) && WriteFile(f, &ih, sizeof(ih), &n, nullptr) &&
              WriteFile(f, argb, pixels, &n, nullptr);
    CloseHandle(f);
    return ok;
}

void JNICALL Cocoa_presentBitmap(JNIEnv* env, jclass, jintArray argb, jint w, jint h) {
    const long frame = ++g_frames;
    if (frame == 1) {
        std::fprintf(stderr, "[win.apk] first frame presented (%dx%d)\n", static_cast<int>(w), static_cast<int>(h));
        std::fflush(stderr);
    }
    static const std::wstring path = [] {
        wchar_t buf[MAX_PATH * 4];
        const DWORD n = GetEnvironmentVariableW(L"WINAPK_HEADLESS_FRAME", buf, MAX_PATH * 4);
        return (n > 0 && n < MAX_PATH * 4) ? std::wstring(buf, n) : std::wstring();
    }();
    static const long exit_after = [] {
        const char* e = std::getenv("WINAPK_HEADLESS_EXIT_AFTER_FRAMES");
        return (e && *e) ? std::atol(e) : 0L;
    }();
    if (exit_after > 0 && frame > exit_after) {
        std::fprintf(stderr, "[win.apk] %ld frames presented; exiting as WINAPK_HEADLESS_EXIT_AFTER_FRAMES asks\n",
                     exit_after);
        Cocoa_exitProcess(env, nullptr, 0, nullptr);
    }
    if (path.empty() || !argb || w <= 0 || h <= 0 || env->GetArrayLength(argb) < w * h) return;
    static std::mutex lock;
    std::lock_guard<std::mutex> hold(lock);
    jint* px = env->GetIntArrayElements(argb, nullptr);
    if (!px) return;
    write_bmp(path.c_str(), px, w, h);
    env->ReleaseIntArrayElements(argb, px, JNI_ABORT);
}

// The diffing present. Headless there is no layer to diff against, so every
// frame is published whole (PRESENT_PUBLISHED).
jint JNICALL Cocoa_presentFrame(JNIEnv* env, jclass cls, jintArray argb, jint w, jint h, jint, jint, jint, jint,
                                jint) {
    Cocoa_presentBitmap(env, cls, argb, w, h);
    return 1;
}

constexpr const char* kCls = "com.macapk.loader.NativeCocoa";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "init", "()Z", Cocoa_init),
    WINAPK_HOST_NATIVE(kCls, "setPhoneSize", "(II)V", Cocoa_setPhoneSize),
    WINAPK_HOST_NATIVE(kCls, "phoneWidth", "()I", Cocoa_phoneWidth),
    WINAPK_HOST_NATIVE(kCls, "phoneHeight", "()I", Cocoa_phoneHeight),
    WINAPK_HOST_NATIVE(kCls, "displayHz", "()I", Cocoa_displayHz),
    WINAPK_HOST_NATIVE(kCls, "backingScale", "()F", Cocoa_backingScale),
    WINAPK_HOST_NATIVE(kCls, "applyRequestedOrientation", "(Z)V", Cocoa_applyRequestedOrientation),
    WINAPK_HOST_NATIVE(kCls, "createWindow", "(Ljava/lang/String;)J", Cocoa_createWindow),
    WINAPK_HOST_NATIVE(kCls, "createMetalSurfaceView", "(II)J", Cocoa_createView),
    WINAPK_HOST_NATIVE(kCls, "createMetalSurfaceViewForGl", "(II)J", Cocoa_createView),
    WINAPK_HOST_NATIVE(kCls, "getPlaceholderWindow", "()J", Cocoa_getPlaceholderWindow),
    WINAPK_HOST_NATIVE(kCls, "runLoop", "()V", Cocoa_runLoop),
    WINAPK_HOST_NATIVE(kCls, "exitProcess", "(ILjava/lang/String;)V", Cocoa_exitProcess),
    WINAPK_HOST_NATIVE(kCls, "quitBackstopExit", "()V", Cocoa_quitBackstopExit),
    WINAPK_HOST_NATIVE(kCls, "presentBitmap", "([III)V", Cocoa_presentBitmap),
    WINAPK_HOST_NATIVE(kCls, "presentFrame", "([IIIIIIII)I", Cocoa_presentFrame),
    WINAPK_HOST_NATIVE(kCls, "setRenderer", "(JLcom/macapk/loader/MetalSurfaceCallbacks;)V", Cocoa_setRenderer),
    WINAPK_HOST_NATIVE(kCls, "releaseRenderer", "(J)V", Cocoa_releaseRenderer),
};

}  // namespace

HostMethods native_cocoa_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
