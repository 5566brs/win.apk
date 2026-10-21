// guest_gl.cpp -- libEGL, libGLESv1_CM, libGLESv2 and libGLESv3 for an app's
// native code, on ANGLE.
//
// Most functions are plain: guest_gl.inc (tools/gen_gl_stubs.py) wraps each
// forwarder of gl_stubs.inc in a System V function. The ones here differ from
// upstream's straight dlsym into ANGLE because Windows has no layer to render
// a window surface into:
//
//   * eglGetDisplay hands out the framework's own display, so the app shares
//     its ANGLE backend, and eglTerminate leaves it running;
//   * a window surface is a pbuffer that holds the native window
//     (native_window.h) in either orientation, and eglSwapBuffers reads the
//     window's part back and presents it as
//     the guest layer: the readback upstream's "offscreen decouple" path does,
//     here the only path;
//   * eglChooseConfig asks for pbuffer configs instead of window ones and drops
//     the Android-only attributes ANGLE rejects (upstream drops
//     EGL_RECORDABLE_ANDROID too);
//   * eglGetProcAddress answers with the System V functions, and the debug
//     callback, which ANGLE would call with the Microsoft convention, is not
//     installed.

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "winapk/guest_abi.h"

#include "gl/gl_loader.h"
#include "gl/gl_surface.h"
#include "native_window.h"

typedef khronos_int32_t GLclampx;

namespace winapk::host {

namespace {

#include "gl/guest_gl.inc"

constexpr EGLint kRecordableAndroid = 0x3142, kFramebufferTargetAndroid = 0x3147;

// A window surface's pbuffer is square, its side the window's longer one, so
// the window can turn (an app asking for landscape after making its surface,
// as SDL does) without a new surface: the app sees the live window size from
// eglQuerySurface, draws from the bottom-left corner (GL's origin), and that
// corner is what eglSwapBuffers reads back.
struct WindowSurface {
    const void* window = nullptr;
    int side = 0;
};

void live_size(const WindowSurface& ws, int* w, int* h) {
    if (!native_window_buffer_size(ws.window, w, h)) *w = *h = ws.side;
    *w = *w < ws.side ? *w : ws.side;
    *h = *h < ws.side ? *h : ws.side;
}
std::mutex g_lock;
std::unordered_map<EGLSurface, WindowSurface> g_window_surfaces;

EGLDisplay shared_display() { return static_cast<EGLDisplay>(gl_shared_display()); }

WINAPK_GUEST_ABI EGLDisplay guest_eglGetDisplay(EGLNativeDisplayType) {
    EGLDisplay d = shared_display();
    return d ? d : EGL_NO_DISPLAY;
}

WINAPK_GUEST_ABI EGLDisplay guest_eglGetPlatformDisplay(EGLenum, void*, const EGLAttrib*) {
    return guest_eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

WINAPK_GUEST_ABI EGLDisplay guest_eglGetPlatformDisplayEXT(EGLenum, void*, const EGLint*) {
    return guest_eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

// The shared display is initialized already; this reports its version.
WINAPK_GUEST_ABI EGLBoolean guest_eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
    return eglInitialize(dpy, major, minor);
}

// The framework renders on the same display, so it stays up. The app's
// surfaces and contexts are its own to destroy, as on a device whose
// eglTerminate is reference-counted (AOSP's, upstream's).
WINAPK_GUEST_ABI EGLBoolean guest_eglTerminate(EGLDisplay dpy) {
    return dpy == shared_display() ? EGL_TRUE : eglTerminate(dpy);
}

WINAPK_GUEST_ABI EGLBoolean guest_eglChooseConfig(EGLDisplay dpy, const EGLint* attribs, EGLConfig* configs,
                                                  EGLint size, EGLint* count) {
    std::vector<EGLint> a;
    bool surface_type = false;
    for (const EGLint* p = attribs; p && *p != EGL_NONE; p += 2) {
        if (p[0] == kRecordableAndroid || p[0] == kFramebufferTargetAndroid) continue;
        EGLint value = p[1];
        if (p[0] == EGL_SURFACE_TYPE) {
            surface_type = true;
            if (value != EGL_DONT_CARE && (value & EGL_WINDOW_BIT)) value = (value & ~EGL_WINDOW_BIT) | EGL_PBUFFER_BIT;
        }
        a.push_back(p[0]);
        a.push_back(value);
    }
    // EGL_SURFACE_TYPE defaults to EGL_WINDOW_BIT.
    if (!surface_type) {
        a.push_back(EGL_SURFACE_TYPE);
        a.push_back(EGL_PBUFFER_BIT);
    }
    a.push_back(EGL_NONE);
    return eglChooseConfig(dpy, a.data(), configs, size, count);
}

EGLSurface create_window_surface(EGLDisplay dpy, EGLConfig config, const void* window) {
    int w = 0, h = 0;
    if (!native_window_buffer_size(window, &w, &h)) {
        std::fprintf(stderr, "[win.apk] GL: eglCreateWindowSurface on a window the runtime did not make\n");
        return EGL_NO_SURFACE;
    }
    const int side = w > h ? w : h;
    const EGLint attribs[] = {EGL_WIDTH, side, EGL_HEIGHT, side, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(dpy, config, attribs);
    if (s == EGL_NO_SURFACE) {
        std::fprintf(stderr, "[win.apk] GL: the app's window surface (%dx%d) failed (0x%x)\n", w, h, eglGetError());
        return s;
    }
    std::fprintf(stderr, "[win.apk] GL: the app's window surface is a %dx%d pbuffer, showing %dx%d\n", side, side, w,
                 h);
    std::lock_guard<std::mutex> hold(g_lock);
    g_window_surfaces[s] = WindowSurface{window, side};
    return s;
}

WINAPK_GUEST_ABI EGLSurface guest_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win,
                                                         const EGLint*) {
    return create_window_surface(dpy, config, reinterpret_cast<const void*>(win));
}

WINAPK_GUEST_ABI EGLSurface guest_eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* win,
                                                                 const EGLAttrib*) {
    return create_window_surface(dpy, config, win);
}

WINAPK_GUEST_ABI EGLSurface guest_eglCreatePlatformWindowSurfaceEXT(EGLDisplay dpy, EGLConfig config, void* win,
                                                                    const EGLint*) {
    return create_window_surface(dpy, config, win);
}

WINAPK_GUEST_ABI EGLBoolean guest_eglDestroySurface(EGLDisplay dpy, EGLSurface surface) {
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_window_surfaces.erase(surface);
    }
    return eglDestroySurface(dpy, surface);
}

WINAPK_GUEST_ABI EGLBoolean guest_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    WindowSurface ws;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_window_surfaces.find(surface);
        if (it == g_window_surfaces.end()) return eglSwapBuffers(dpy, surface);
        ws = it->second;
    }
    // Only the current surface can be read; EGL requires it to be current to
    // swap anyway.
    if (eglGetCurrentSurface(EGL_DRAW) != surface) return EGL_TRUE;
    int w = 0, h = 0;
    live_size(ws, &w, &h);
    thread_local std::vector<uint32_t> argb;
    gl_read_current(&argb, w, h);
    guest_frame_present(argb.data(), w, h);
    return EGL_TRUE;
}

// A window surface's size is the window's (see WindowSurface).
WINAPK_GUEST_ABI EGLBoolean guest_eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
                                                  EGLint* value) {
    if (attribute == EGL_WIDTH || attribute == EGL_HEIGHT) {
        WindowSurface ws;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            auto it = g_window_surfaces.find(surface);
            if (it != g_window_surfaces.end()) ws = it->second;
        }
        if (ws.window && value) {
            int w = 0, h = 0;
            live_size(ws, &w, &h);
            *value = attribute == EGL_WIDTH ? w : h;
            return EGL_TRUE;
        }
    }
    return eglQuerySurface(dpy, surface, attribute, value);
}

WINAPK_GUEST_ABI EGLBoolean guest_eglSwapBuffersWithDamage(EGLDisplay dpy, EGLSurface surface, const EGLint*,
                                                           EGLint) {
    return guest_eglSwapBuffers(dpy, surface);
}

// Hints the window has no use for.
WINAPK_GUEST_ABI EGLBoolean guest_eglPresentationTimeANDROID(EGLDisplay, EGLSurface, EGLnsecsANDROID) {
    return EGL_TRUE;
}
WINAPK_GUEST_ABI EGLBoolean guest_eglSetDamageRegionKHR(EGLDisplay, EGLSurface, EGLint*, EGLint) {
    return EGL_TRUE;
}
WINAPK_GUEST_ABI EGLint guest_eglDupNativeFenceFDANDROID(EGLDisplay, EGLSyncKHR) {
    return EGL_NO_NATIVE_FENCE_FD_ANDROID;
}

// The KHR image and fence extensions, forwarded to ANGLE's.
#define WINAPK_EGL_EXT(ret, name, params, args)                                                  \
    WINAPK_GUEST_ABI ret guest_##name params {                                                    \
        using Fn = ret(EGLAPIENTRY*) params;                                                      \
        static const Fn fn = reinterpret_cast<Fn>(gl_proc(#name, GlLib::Egl));                    \
        return fn ? fn args : ret{};                                                              \
    }
WINAPK_EGL_EXT(EGLImageKHR, eglCreateImageKHR,
               (EGLDisplay d, EGLContext c, EGLenum t, EGLClientBuffer b, const EGLint* a), (d, c, t, b, a))
WINAPK_EGL_EXT(EGLBoolean, eglDestroyImageKHR, (EGLDisplay d, EGLImageKHR i), (d, i))
WINAPK_EGL_EXT(EGLSyncKHR, eglCreateSyncKHR, (EGLDisplay d, EGLenum t, const EGLint* a), (d, t, a))
WINAPK_EGL_EXT(EGLBoolean, eglDestroySyncKHR, (EGLDisplay d, EGLSyncKHR s), (d, s))
WINAPK_EGL_EXT(EGLint, eglClientWaitSyncKHR, (EGLDisplay d, EGLSyncKHR s, EGLint f, EGLTimeKHR t), (d, s, f, t))
WINAPK_EGL_EXT(EGLint, eglWaitSyncKHR, (EGLDisplay d, EGLSyncKHR s, EGLint f), (d, s, f))
WINAPK_EGL_EXT(EGLBoolean, eglGetSyncAttribKHR, (EGLDisplay d, EGLSyncKHR s, EGLint a, EGLint* v), (d, s, a, v))
#undef WINAPK_EGL_EXT

WINAPK_GUEST_ABI void guest_glDebugMessageCallback(GLDEBUGPROC, const void*) {}
WINAPK_GUEST_ABI void guest_glDebugMessageCallbackKHR(GLDEBUGPROCKHR, const void*) {}

WINAPK_GUEST_ABI __eglMustCastToProperFunctionPointerType guest_eglGetProcAddress(const char* name);

#define WINAPK_GUEST_GL(name) {#name, reinterpret_cast<void*>(&guest_##name)}
const std::unordered_map<std::string, void*>& symbols() {
    static const std::unordered_map<std::string, void*> m = {
        WINAPK_GUEST_GL_SYMBOLS
        WINAPK_GUEST_GL(eglGetDisplay),
        WINAPK_GUEST_GL(eglGetPlatformDisplay),
        WINAPK_GUEST_GL(eglGetPlatformDisplayEXT),
        WINAPK_GUEST_GL(eglInitialize),
        WINAPK_GUEST_GL(eglTerminate),
        WINAPK_GUEST_GL(eglChooseConfig),
        WINAPK_GUEST_GL(eglCreateWindowSurface),
        WINAPK_GUEST_GL(eglCreatePlatformWindowSurface),
        WINAPK_GUEST_GL(eglCreatePlatformWindowSurfaceEXT),
        WINAPK_GUEST_GL(eglDestroySurface),
        WINAPK_GUEST_GL(eglSwapBuffers),
        WINAPK_GUEST_GL(eglQuerySurface),
        {"eglSwapBuffersWithDamageKHR", reinterpret_cast<void*>(&guest_eglSwapBuffersWithDamage)},
        {"eglSwapBuffersWithDamageEXT", reinterpret_cast<void*>(&guest_eglSwapBuffersWithDamage)},
        WINAPK_GUEST_GL(eglPresentationTimeANDROID),
        WINAPK_GUEST_GL(eglSetDamageRegionKHR),
        WINAPK_GUEST_GL(eglDupNativeFenceFDANDROID),
        WINAPK_GUEST_GL(eglCreateImageKHR),
        WINAPK_GUEST_GL(eglDestroyImageKHR),
        WINAPK_GUEST_GL(eglCreateSyncKHR),
        WINAPK_GUEST_GL(eglDestroySyncKHR),
        WINAPK_GUEST_GL(eglClientWaitSyncKHR),
        WINAPK_GUEST_GL(eglWaitSyncKHR),
        WINAPK_GUEST_GL(eglGetSyncAttribKHR),
        WINAPK_GUEST_GL(eglGetProcAddress),
        WINAPK_GUEST_GL(glDebugMessageCallback),
        WINAPK_GUEST_GL(glDebugMessageCallbackKHR),
    };
    return m;
}

// Only functions with a System V wrapper can be handed out; anything else is
// "not supported", which EGL allows.
WINAPK_GUEST_ABI __eglMustCastToProperFunctionPointerType guest_eglGetProcAddress(const char* name) {
    if (!name) return nullptr;
    auto it = symbols().find(name);
    return it == symbols().end() ? nullptr : reinterpret_cast<__eglMustCastToProperFunctionPointerType>(it->second);
}

}  // namespace

void* guest_gl_resolve(const char* name) {
    if (!name || (std::strncmp(name, "gl", 2) != 0 && std::strncmp(name, "egl", 3) != 0)) return nullptr;
    auto it = symbols().find(name);
    return it == symbols().end() ? nullptr : it->second;
}

}  // namespace winapk::host
