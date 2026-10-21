// native_cocoa.cpp -- com.macapk.loader.NativeCocoa on Windows.
//
// NativeCocoa is upstream's window system: the window, the surfaces the app
// draws into, input, the run loop and process exit. Here the app gets one
// Win32 window (win32_window.h) that shows its frames and turns the mouse and
// keyboard into Android input. Windows and views Java creates are plain handle
// numbers; they all draw into that one window. With WINAPK_HEADLESS=1, or
// when there is no desktop, there is no window at all, which is how CI runs.
//
// Drawing is driven from here, as on the Mac, where the display link calls the
// renderer the app's window registered (setRenderer) once per vsync: here a
// 60 Hz thread does it. Frames come back through presentFrame or
// presentBitmap. With WINAPK_HEADLESS_FRAME set to a
// file path, the latest one is written there as a BMP, which is how a headless
// run shows that the app really drew something. WINAPK_HEADLESS_EXIT_AFTER_FRAMES
// ends the process with exit code 0 that many display frames after the first
// presented one, which is what the CI smoke test waits for. It counts display
// frames, not presented ones: an app that draws once and then sits idle
// presents nothing more, and still has to finish.

#include "host_natives.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <string>

#include <windows.h>
#include <shellapi.h>

#include "gl/gl_surface.h"
#include "win32_window.h"
#include "winapk/mini_bionic.h"

namespace winapk::host {

namespace {

// Upstream's default phone size (cocoa_bridge.mm), in points.
std::atomic<int> g_phone_w{360};
std::atomic<int> g_phone_h{640};
std::atomic<jlong> g_next_handle{1};
std::atomic<long> g_frames{0};

jlong new_handle() { return g_next_handle.fetch_add(1); }

JavaVM* g_vm = nullptr;

// ---- input: the window's UI thread calls into the TouchDispatcher -----------

jobject g_dispatcher = nullptr;  // global ref, set by registerTouchDispatcher
jmethodID g_dispatch_mid = nullptr, g_key_mid = nullptr;

JNIEnv* ui_thread_env() {
    thread_local JNIEnv* env = nullptr;
    if (!env && g_vm) {
        JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("winapk-ui"), nullptr};
        if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) env = nullptr;
    }
    return env;
}

void report_input_exception(JNIEnv* env) {
    if (!env->ExceptionCheck()) return;
    std::fprintf(stderr, "[win.apk] exception while delivering input:\n");
    env->ExceptionDescribe();
    env->ExceptionClear();
}

class JavaInput : public WindowInput {
public:
    // TouchDispatcher.dispatch takes AppKit view coordinates: points, origin at
    // the bottom left. The window is the phone scaled to the client area.
    void pointer(int action, float x, float y, float view_w, float view_h) override {
        JNIEnv* env = ui_thread_env();
        if (!env || !g_dispatcher || view_w <= 0 || view_h <= 0) return;
        const float pw = static_cast<float>(g_phone_w), ph = static_cast<float>(g_phone_h);
        const float px = x * pw / view_w;
        const float py = ph - y * ph / view_h;
        env->CallVoidMethod(g_dispatcher, g_dispatch_mid, action, px, py, pw, ph);
        report_input_exception(env);
    }
    void key(int action, int key_code, int unicode, int meta) override {
        JNIEnv* env = ui_thread_env();
        if (!env || !g_dispatcher) return;
        env->CallVoidMethod(g_dispatcher, g_key_mid, action, key_code, unicode, meta);
        report_input_exception(env);
    }
    // Upstream ends the process on a user quit too (cocoa_bridge.mm, _exit);
    // the app's state is whatever it saved on its way through onPause/onStop.
    void close_requested() override {
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
};

JavaInput g_java_input;

void JNICALL Cocoa_registerTouchDispatcher(JNIEnv* env, jclass, jobject dispatcher) {
    if (g_dispatcher) env->DeleteGlobalRef(g_dispatcher);
    g_dispatcher = dispatcher ? env->NewGlobalRef(dispatcher) : nullptr;
    if (!dispatcher) return;
    jclass cls = env->GetObjectClass(dispatcher);
    g_dispatch_mid = env->GetMethodID(cls, "dispatch", "(IFFFF)V");
    g_key_mid = env->GetMethodID(cls, "dispatchKey", "(IIII)V");
    env->DeleteLocalRef(cls);
}

// ---- window and metrics -----------------------------------------------------

bool headless_requested() {
    const char* e = std::getenv("WINAPK_HEADLESS");
    return e && *e && *e != '0';
}

// ---- the app icon -----------------------------------------------------------
//
// Boot renders the app's launcher icon, adaptive and vector ones included, to
// <app dir>/icon.png for upstream's launcher. Here it becomes the window and
// taskbar icon: right away when an earlier run left one, and again whenever
// the file changes (a first run renders it during boot, before the activity
// shows its window).

std::mutex g_icon_lock;
std::wstring g_icon_path;
FILETIME g_icon_applied{};

void apply_app_icon() {
    std::lock_guard<std::mutex> hold(g_icon_lock);
    if (g_icon_path.empty() || !window_running()) return;
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesExW(g_icon_path.c_str(), GetFileExInfoStandard, &attrs)) return;
    if (CompareFileTime(&attrs.ftLastWriteTime, &g_icon_applied) == 0) return;
    if (window_set_icon_png(g_icon_path)) g_icon_applied = attrs.ftLastWriteTime;
}

jboolean JNICALL Cocoa_init(JNIEnv* env, jclass) {
    env->GetJavaVM(&g_vm);
    if (headless_requested()) {
        std::fprintf(stderr, "[win.apk] WINAPK_HEADLESS is set; running without a window\n");
    } else if (!window_start(g_phone_w, g_phone_h, &g_java_input)) {
        std::fprintf(stderr, "[win.apk] no desktop to open a window on; running headless\n");
    }
    apply_app_icon();
    return JNI_TRUE;
}

void JNICALL Cocoa_setPhoneSize(JNIEnv*, jclass, jint w, jint h) {
    if (w > 0) g_phone_w = w;
    if (h > 0) g_phone_h = h;
    window_set_phone_size(g_phone_w, g_phone_h);
}

jint JNICALL Cocoa_phoneWidth(JNIEnv*, jclass) { return g_phone_w; }
jint JNICALL Cocoa_phoneHeight(JNIEnv*, jclass) { return g_phone_h; }
jint JNICALL Cocoa_displayHz(JNIEnv*, jclass) { return 60; }
float device_density(JNIEnv* env);

// Surface pixels per point. TouchDispatcher multiplies view points by it, and
// the surface is the phone size at the device density, so the two must agree.
// (On the Mac this is the display's scale, which equals the density on a
// Retina screen.)
jfloat JNICALL Cocoa_backingScale(JNIEnv* env, jclass) {
    static const float density = device_density(env);
    return density;
}

// Same rule as upstream: the short side stays the short side.
void JNICALL Cocoa_applyRequestedOrientation(JNIEnv*, jclass, jboolean landscape) {
    const int a = g_phone_w, b = g_phone_h;
    const int lo = a < b ? a : b, hi = a < b ? b : a;
    g_phone_w = landscape ? hi : lo;
    g_phone_h = landscape ? lo : hi;
    window_set_phone_size(g_phone_w, g_phone_h);
}

// ActivityInfo.screenOrientation: the landscape and portrait families turn
// the phone; the rest (unspecified, sensor, behind...) leave it as it is.
void apply_screen_orientation(jint o) {
    if (o == 0 || o == 6 || o == 8 || o == 11) Cocoa_applyRequestedOrientation(nullptr, nullptr, JNI_TRUE);
    else if (o == 1 || o == 7 || o == 9 || o == 12) Cocoa_applyRequestedOrientation(nullptr, nullptr, JNI_FALSE);
}
void JNICALL Cocoa_applyWindowOrientation(JNIEnv*, jclass, jint o) { apply_screen_orientation(o); }
void JNICALL Cocoa_applyWindowOrientationFor(JNIEnv*, jclass, jstring, jint, jint o, jint) {
    apply_screen_orientation(o);
}
// The user rotation is the boot orientation, portrait (there is no Cmd+R
// rotation here).
void JNICALL Cocoa_restoreUserOrientation(JNIEnv*, jclass) {
    Cocoa_applyRequestedOrientation(nullptr, nullptr, JNI_FALSE);
}
// No user aspect-ratio override exists here, so the record's own orientation stands.
jint JNICALL Cocoa_windowOverrideOrientation(JNIEnv*, jclass, jstring, jint record) { return record; }
// No per-activity window policy session: Java falls back to the display
// commit (documented null).
jintArray JNICALL Cocoa_activityWindowTransition(JNIEnv*, jclass, jstring, jint, jboolean, jboolean, jint, jint, jint,
                                                 jint, jint) {
    return nullptr;
}

// One window holds every activity, so these window-shaping calls have
// nothing to act on.
void JNICALL Cocoa_setTopActivityConfigChanges(JNIEnv*, jclass, jint) {}
void JNICALL Cocoa_setMaxAspectRatio(JNIEnv*, jclass, jfloat) {}
// The view a window shows. When it is a GL view (createMetalSurfaceViewForGl),
// the window shows that view's GL frames and the Java UI comes as the overlay
// layer; otherwise it shows what the view tree presents (presentBitmap).
std::atomic<jlong> g_content_view{0};
std::mutex g_gl_views_lock;
std::set<jlong> g_gl_views;
bool is_gl_view(jlong view) {
    std::lock_guard<std::mutex> hold(g_gl_views_lock);
    return g_gl_views.count(view) != 0;
}
void JNICALL Cocoa_setContentView(JNIEnv*, jclass, jlong, jlong view) { g_content_view = view; }
void JNICALL Cocoa_closeWindow(JNIEnv*, jclass, jlong) {}

// FLAG_SECURE: kept out of screenshots, recording and screen sharing.
void JNICALL Cocoa_setSecureContent(JNIEnv*, jclass, jboolean secure) { window_set_secure(secure == JNI_TRUE); }

// An implicit ACTION_VIEW/SENDTO/DIAL intent nobody in the app handles: the
// system opens the URL (browser, mail, ...). Only URLs with a scheme, and not
// file:, so an intent cannot start a program by its path.
jboolean JNICALL Cocoa_openHostUrl(JNIEnv* env, jclass, jstring url) {
    if (!url) return JNI_FALSE;
    const jchar* c = env->GetStringChars(url, nullptr);
    const std::wstring u(reinterpret_cast<const wchar_t*>(c), static_cast<size_t>(env->GetStringLength(url)));
    env->ReleaseStringChars(url, c);
    size_t colon = u.find(L':');
    bool scheme = colon != std::wstring::npos && colon > 1 && iswalpha(u[0]);
    for (size_t i = 1; scheme && i < colon; ++i) {
        scheme = iswalnum(u[i]) || u[i] == L'+' || u[i] == L'-' || u[i] == L'.';
    }
    if (!scheme || _wcsnicmp(u.c_str(), L"file:", 5) == 0) return JNI_FALSE;
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32 ? JNI_TRUE : JNI_FALSE;
}

// Custom URL schemes for the app (OAuth redirects) need the scheme registered
// and a running instance to forward to; until then the intake answers
// "unavailable", which HostUrlIntake treats as no host routing.
jobjectArray JNICALL Cocoa_takeHostUrls(JNIEnv* env, jclass) {
    return env->NewObjectArray(0, env->FindClass("java/lang/String"), nullptr);
}
jobjectArray JNICALL Cocoa_urlSchemeHandlers(JNIEnv*, jclass, jstring) { return nullptr; }
jint JNICALL Cocoa_setUrlSchemeDefault(JNIEnv*, jclass, jstring, jstring) { return -1; }

// The GL overlay layer: dialogs and toasts over content the view tree does
// not draw (GL, surfaces).
void JNICALL Cocoa_presentOverlayBitmap(JNIEnv* env, jclass, jintArray argb, jint w, jint h) {
    if (!argb || w <= 0 || h <= 0 || env->GetArrayLength(argb) < w * h) return;
    jint* px = env->GetIntArrayElements(argb, nullptr);
    if (!px) return;
    window_set_overlay(reinterpret_cast<const uint32_t*>(px), w, h);
    env->ReleaseIntArrayElements(argb, px, JNI_ABORT);
}
void JNICALL Cocoa_clearOverlayBitmap(JNIEnv*, jclass) { window_clear_overlay(); }

// Quitting: the window goes away at once, before the app's teardown runs.
jboolean JNICALL Cocoa_hostHideWindowsForQuit(JNIEnv*, jclass) {
    window_hide();
    return window_running() ? JNI_TRUE : JNI_FALSE;
}

// Upstream shows "Translating <lib>..." while it translates ARM code; x86_64
// libraries load without translation, so there is nothing to show.
void JNICALL Cocoa_showTranslationOverlay(JNIEnv*, jclass, jstring) {}
void JNICALL Cocoa_updateTranslationOverlay(JNIEnv*, jclass, jstring) {}
void JNICALL Cocoa_hideTranslationOverlay(JNIEnv*, jclass) {}

// A RENDERMODE_WHEN_DIRTY GLSurfaceView with nothing to draw withholds the
// tick: the pbuffer still holds the last frame, so the readback is skipped.
// (Upstream may ask for a draw anyway for its self-capture; there is none.)
thread_local bool t_tick_withheld = false;
jboolean JNICALL Cocoa_beginWithheldTick(JNIEnv*, jclass, jboolean) {
    t_tick_withheld = true;
    return JNI_FALSE;
}
jboolean JNICALL Cocoa_hostRenderTick(JNIEnv*, jclass) { return JNI_FALSE; }

void JNICALL Cocoa_showWindow(JNIEnv*, jclass, jlong) {
    apply_app_icon();
    window_show();
}
void JNICALL Cocoa_moveTaskToBack(JNIEnv*, jclass) { window_minimize(); }
void JNICALL Cocoa_moveTaskToFront(JNIEnv*, jclass) { window_restore(); }
void JNICALL Cocoa_setStartingWindowColor(JNIEnv*, jclass, jint argb) {
    window_set_background(static_cast<uint32_t>(argb));
}

// Upstream mirrors the app's System.setProperty calls into the native property
// store so guest code reading __system_property_get sees the same values.
void JNICALL Cocoa_setSystemProperty(JNIEnv* env, jclass, jstring key, jstring value) {
    if (!key) return;
    const char* k = env->GetStringUTFChars(key, nullptr);
    const char* v = value ? env->GetStringUTFChars(value, nullptr) : nullptr;
    if (k) winapk::property_set(k, v ? v : "");
    if (v) env->ReleaseStringUTFChars(value, v);
    if (k) env->ReleaseStringUTFChars(key, k);
}

// Boot writes Android's fonts (and a fonts.xml) to a cache directory; native
// text stacks (Skia in Flutter apps) read them as /system/fonts.
void JNICALL Cocoa_setSystemFontsDir(JNIEnv* env, jclass, jstring dir) {
    if (!dir) return;
    const char* d = env->GetStringUTFChars(dir, nullptr);
    if (d) {
        winapk::set_system_fonts_dir(d);
        env->ReleaseStringUTFChars(dir, d);
    }
}
// Upstream uses these for its /proc/self/maps and relative-path emulation,
// which the Windows shim does not have; accepting them keeps boot quiet.
void JNICALL Cocoa_setNativeLibDir(JNIEnv*, jclass, jstring) {}
void JNICALL Cocoa_setDataSubdir(JNIEnv*, jclass, jstring) {}

void JNICALL Cocoa_setWindowTitle(JNIEnv* env, jclass, jlong, jstring title) {
    if (!title) return;
    const char* t = env->GetStringUTFChars(title, nullptr);
    if (t) {
        window_set_title(t);
        env->ReleaseStringUTFChars(title, t);
    }
    apply_app_icon();
}

jlong JNICALL Cocoa_createWindow(JNIEnv*, jclass, jstring) { return new_handle(); }
jlong JNICALL Cocoa_createView(JNIEnv*, jclass, jint, jint) { return new_handle(); }
jlong JNICALL Cocoa_createGlView(JNIEnv*, jclass, jint, jint) {
    const jlong view = new_handle();
    std::lock_guard<std::mutex> hold(g_gl_views_lock);
    g_gl_views.insert(view);
    return view;
}
jlong JNICALL Cocoa_getPlaceholderWindow(JNIEnv*, jclass) {
    static const jlong placeholder = new_handle();
    return placeholder;
}

// ---- the render tick ------------------------------------------------------

// Every view with a renderer (MetalSurfaceCallbacks) is driven each tick:
// the canvas views draw the view tree and present it themselves; a GL view
// draws with ANGLE's context current on its pbuffer (gl_surface.h).
struct Renderer {
    jobject callbacks = nullptr;  // global ref
    bool gl = false;
    int announced_w = 0, announced_h = 0;
};
std::mutex g_renderer_lock;
std::map<jlong, Renderer> g_renderers;
std::vector<jlong> g_released_gl_views;  // pbuffers for the render thread to drop

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

void JNICALL Cocoa_exitProcess(JNIEnv*, jclass, jint code, jstring);

void present_pixels(const uint32_t* argb, int w, int h);

// One GL view's tick: ANGLE's context on its pbuffer, the renderer's
// lifecycle and frame, then the pixels for the window when it is the content.
void gl_tick(JNIEnv* env, jlong view, Renderer& r, jobject callbacks, jmethodID changed, jmethodID draw,
             int w, int h) {
    const bool ready = gl_frame_begin(view, w, h);
    if (r.announced_w != w || r.announced_h != h) {
        r.announced_w = w;
        r.announced_h = h;
        env->CallVoidMethod(callbacks, changed, w, h);
        check_tick_exception(env);
    }
    t_tick_withheld = false;
    env->CallVoidMethod(callbacks, draw);
    check_tick_exception(env);
    static std::vector<uint32_t> pixels;
    int fw = 0, fh = 0;
    if (ready && !t_tick_withheld && view == g_content_view.load() && gl_frame_read(&pixels, &fw, &fh)) {
        present_pixels(pixels.data(), fw, fh);
    }
}

DWORD WINAPI render_thread(void*) {
    JNIEnv* env = nullptr;
    JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("winapk-render"), nullptr};
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) return 1;
    const float density = device_density(env);
    const char* e = std::getenv("WINAPK_HEADLESS_EXIT_AFTER_FRAMES");
    const long exit_after = (e && *e) ? std::atol(e) : 0L;
    long ticks_since_first = 0;
    jclass cls = env->FindClass("com/macapk/loader/MetalSurfaceCallbacks");
    const jmethodID changed = env->GetMethodID(cls, "macapkSurfaceChanged", "(II)V");
    const jmethodID draw = env->GetMethodID(cls, "macapkDrawFrame", "()V");
    env->DeleteLocalRef(cls);
    for (;;) {
        Sleep(16);
        const int w = static_cast<int>(g_phone_w * density + 0.5f), h = static_cast<int>(g_phone_h * density + 0.5f);
        std::vector<std::pair<jlong, jobject>> now;
        std::vector<jlong> dropped;
        {
            std::lock_guard<std::mutex> hold(g_renderer_lock);
            for (auto& [view, r] : g_renderers) now.emplace_back(view, env->NewLocalRef(r.callbacks));
            dropped.swap(g_released_gl_views);
        }
        for (jlong view : dropped) gl_view_release(view);
        for (auto& [view, callbacks] : now) {
            Renderer r;
            {
                std::lock_guard<std::mutex> hold(g_renderer_lock);
                auto it = g_renderers.find(view);
                if (it == g_renderers.end()) {
                    env->DeleteLocalRef(callbacks);
                    continue;
                }
                r = it->second;
            }
            if (r.gl) {
                gl_tick(env, view, r, callbacks, changed, draw, w, h);
            } else {
                if (r.announced_w != w || r.announced_h != h) {
                    r.announced_w = w;
                    r.announced_h = h;
                    env->CallVoidMethod(callbacks, changed, w, h);
                    check_tick_exception(env);
                }
                env->CallVoidMethod(callbacks, draw);
                check_tick_exception(env);
            }
            {
                std::lock_guard<std::mutex> hold(g_renderer_lock);
                auto it = g_renderers.find(view);
                if (it != g_renderers.end() && it->second.callbacks == r.callbacks) {
                    it->second.announced_w = r.announced_w;
                    it->second.announced_h = r.announced_h;
                }
            }
            env->DeleteLocalRef(callbacks);
        }
        if (exit_after > 0 && g_frames > 0 && ++ticks_since_first >= exit_after) {
            std::fprintf(stderr,
                         "[win.apk] %ld frames presented in %ld display frames; exiting as "
                         "WINAPK_HEADLESS_EXIT_AFTER_FRAMES asks\n",
                         g_frames.load(), ticks_since_first);
            Cocoa_exitProcess(env, nullptr, 0, nullptr);
        }
    }
}

void JNICALL Cocoa_setRenderer(JNIEnv* env, jclass, jlong view, jobject callbacks) {
    static bool started = false;
    std::lock_guard<std::mutex> hold(g_renderer_lock);
    auto it = g_renderers.find(view);
    if (it != g_renderers.end()) {
        env->DeleteGlobalRef(it->second.callbacks);
        g_renderers.erase(it);
    }
    if (!callbacks) return;
    Renderer r;
    r.callbacks = env->NewGlobalRef(callbacks);
    r.gl = is_gl_view(view);
    g_renderers[view] = r;
    if (!started) {
        started = true;
        env->GetJavaVM(&g_vm);
        HANDLE t = CreateThread(nullptr, 16 * 1024 * 1024, render_thread, nullptr,
                                STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (t) CloseHandle(t);
    }
}

void JNICALL Cocoa_releaseRenderer(JNIEnv* env, jclass, jlong view) {
    std::lock_guard<std::mutex> hold(g_renderer_lock);
    auto it = g_renderers.find(view);
    if (it == g_renderers.end()) return;
    if (it->second.gl) g_released_gl_views.push_back(view);
    env->DeleteGlobalRef(it->second.callbacks);
    g_renderers.erase(it);
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
// (BGRA in memory) and needs no encoder. It goes to a temporary file that then
// replaces `path`, so the process ending mid-write (the headless exit runs on
// the render thread, not the presenting one) never leaves half a frame.
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
    const std::wstring tmp = std::wstring(path) + L".part";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(f, &fh, sizeof(fh), &n, nullptr) && WriteFile(f, &ih, sizeof(ih), &n, nullptr) &&
              WriteFile(f, argb, pixels, &n, nullptr);
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING);
}

// A frame for the window (and for WINAPK_HEADLESS_FRAME): from the view tree,
// or from the content GL view (gl_tick).
void present_pixels(const uint32_t* argb, int w, int h) {
    const long frame = ++g_frames;
    if (frame == 1) {
        std::fprintf(stderr, "[win.apk] first frame presented (%dx%d)\n", w, h);
        std::fflush(stderr);
    }
    static const std::wstring path = [] {
        wchar_t buf[MAX_PATH * 4];
        const DWORD n = GetEnvironmentVariableW(L"WINAPK_HEADLESS_FRAME", buf, MAX_PATH * 4);
        return (n > 0 && n < MAX_PATH * 4) ? std::wstring(buf, n) : std::wstring();
    }();
    // An ARGB int is B, G, R, A in memory: the byte order of a 32-bit DIB.
    window_present(argb, w, h);
    if (!path.empty()) {
        static std::mutex lock;
        std::lock_guard<std::mutex> hold(lock);
        write_bmp(path.c_str(), reinterpret_cast<const jint*>(argb), w, h);
    }
}

// The view tree's frame. While a GL view is the window's content the view
// tree reaches the window as the overlay layer instead (presentOverlayBitmap).
void JNICALL Cocoa_presentBitmap(JNIEnv* env, jclass, jintArray argb, jint w, jint h) {
    if (!argb || w <= 0 || h <= 0 || env->GetArrayLength(argb) < w * h) return;
    if (is_gl_view(g_content_view.load())) return;
    jint* px = env->GetIntArrayElements(argb, nullptr);
    if (!px) return;
    present_pixels(reinterpret_cast<const uint32_t*>(px), w, h);
    env->ReleaseIntArrayElements(argb, px, JNI_ABORT);
}

// The diffing present. There is no layer to diff against here, so every frame
// is published whole (PRESENT_PUBLISHED).
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
    WINAPK_HOST_NATIVE(kCls, "createMetalSurfaceViewForGl", "(II)J", Cocoa_createGlView),
    WINAPK_HOST_NATIVE(kCls, "getPlaceholderWindow", "()J", Cocoa_getPlaceholderWindow),
    WINAPK_HOST_NATIVE(kCls, "runLoop", "()V", Cocoa_runLoop),
    WINAPK_HOST_NATIVE(kCls, "exitProcess", "(ILjava/lang/String;)V", Cocoa_exitProcess),
    WINAPK_HOST_NATIVE(kCls, "quitBackstopExit", "()V", Cocoa_quitBackstopExit),
    WINAPK_HOST_NATIVE(kCls, "presentBitmap", "([III)V", Cocoa_presentBitmap),
    WINAPK_HOST_NATIVE(kCls, "registerTouchDispatcher", "(Lcom/macapk/loader/TouchDispatcher;)V",
                       Cocoa_registerTouchDispatcher),
    WINAPK_HOST_NATIVE(kCls, "showWindow", "(J)V", Cocoa_showWindow),
    WINAPK_HOST_NATIVE(kCls, "moveTaskToBack", "()V", Cocoa_moveTaskToBack),
    WINAPK_HOST_NATIVE(kCls, "moveTaskToFront", "()V", Cocoa_moveTaskToFront),
    WINAPK_HOST_NATIVE(kCls, "setStartingWindowColor", "(I)V", Cocoa_setStartingWindowColor),
    WINAPK_HOST_NATIVE(kCls, "setSystemProperty", "(Ljava/lang/String;Ljava/lang/String;)V",
                       Cocoa_setSystemProperty),
    WINAPK_HOST_NATIVE(kCls, "setSystemFontsDir", "(Ljava/lang/String;)V", Cocoa_setSystemFontsDir),
    WINAPK_HOST_NATIVE(kCls, "setNativeLibDir", "(Ljava/lang/String;)V", Cocoa_setNativeLibDir),
    WINAPK_HOST_NATIVE(kCls, "setDataSubdir", "(Ljava/lang/String;)V", Cocoa_setDataSubdir),
    WINAPK_HOST_NATIVE(kCls, "applyWindowOrientation", "(I)V", Cocoa_applyWindowOrientation),
    WINAPK_HOST_NATIVE(kCls, "applyWindowOrientationFor", "(Ljava/lang/String;III)V", Cocoa_applyWindowOrientationFor),
    WINAPK_HOST_NATIVE(kCls, "restoreUserOrientation", "()V", Cocoa_restoreUserOrientation),
    WINAPK_HOST_NATIVE(kCls, "windowOverrideOrientation", "(Ljava/lang/String;I)I", Cocoa_windowOverrideOrientation),
    WINAPK_HOST_NATIVE(kCls, "activityWindowTransition", "(Ljava/lang/String;IZZIIIII)[I",
                       Cocoa_activityWindowTransition),
    WINAPK_HOST_NATIVE(kCls, "setTopActivityConfigChanges", "(I)V", Cocoa_setTopActivityConfigChanges),
    WINAPK_HOST_NATIVE(kCls, "setMaxAspectRatio", "(F)V", Cocoa_setMaxAspectRatio),
    WINAPK_HOST_NATIVE(kCls, "setContentView", "(JJ)V", Cocoa_setContentView),
    WINAPK_HOST_NATIVE(kCls, "closeWindow", "(J)V", Cocoa_closeWindow),
    WINAPK_HOST_NATIVE(kCls, "setSecureContent", "(Z)V", Cocoa_setSecureContent),
    WINAPK_HOST_NATIVE(kCls, "openHostUrl", "(Ljava/lang/String;)Z", Cocoa_openHostUrl),
    WINAPK_HOST_NATIVE(kCls, "takeHostUrls", "()[Ljava/lang/String;", Cocoa_takeHostUrls),
    WINAPK_HOST_NATIVE(kCls, "urlSchemeHandlers", "(Ljava/lang/String;)[Ljava/lang/String;", Cocoa_urlSchemeHandlers),
    WINAPK_HOST_NATIVE(kCls, "setUrlSchemeDefault", "(Ljava/lang/String;Ljava/lang/String;)I", Cocoa_setUrlSchemeDefault),
    WINAPK_HOST_NATIVE(kCls, "presentOverlayBitmap", "([III)V", Cocoa_presentOverlayBitmap),
    WINAPK_HOST_NATIVE(kCls, "clearOverlayBitmap", "()V", Cocoa_clearOverlayBitmap),
    WINAPK_HOST_NATIVE(kCls, "hostHideWindowsForQuit", "()Z", Cocoa_hostHideWindowsForQuit),
    WINAPK_HOST_NATIVE(kCls, "showTranslationOverlay", "(Ljava/lang/String;)V", Cocoa_showTranslationOverlay),
    WINAPK_HOST_NATIVE(kCls, "updateTranslationOverlay", "(Ljava/lang/String;)V", Cocoa_updateTranslationOverlay),
    WINAPK_HOST_NATIVE(kCls, "hideTranslationOverlay", "()V", Cocoa_hideTranslationOverlay),
    WINAPK_HOST_NATIVE(kCls, "beginWithheldTick", "(Z)Z", Cocoa_beginWithheldTick),
    WINAPK_HOST_NATIVE(kCls, "hostRenderTick", "()Z", Cocoa_hostRenderTick),
    WINAPK_HOST_NATIVE(kCls, "setWindowTitle", "(JLjava/lang/String;)V", Cocoa_setWindowTitle),
    WINAPK_HOST_NATIVE(kCls, "presentFrame", "([IIIIIIII)I", Cocoa_presentFrame),
    WINAPK_HOST_NATIVE(kCls, "setRenderer", "(JLcom/macapk/loader/MetalSurfaceCallbacks;)V", Cocoa_setRenderer),
    WINAPK_HOST_NATIVE(kCls, "releaseRenderer", "(J)V", Cocoa_releaseRenderer),
};

}  // namespace

HostMethods native_cocoa_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

void set_app_dir(const std::wstring& dir) {
    std::lock_guard<std::mutex> hold(g_icon_lock);
    g_icon_path = dir + L"\\icon.png";
}

}  // namespace winapk::host
