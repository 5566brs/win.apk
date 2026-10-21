// native_window.h -- the NDK's ANativeWindow for an app's native code.
//
// Upstream (bionic_shim.cpp, MacApkNativeWindow) has one native window per
// process, backed by a CAMetalLayer that ANGLE renders into. Windows has no
// such layer: the window is a size and a format, an EGL window surface on it
// is a pbuffer (guest_gl.cpp), and its frames, whether eglSwapBuffers' or
// ANativeWindow_unlockAndPost's, go to native_cocoa as the guest layer,
// composited under the view tree like upstream's (SurfaceView's hole).

#pragma once

#include <cstddef>
#include <cstdint>

namespace winapk::host {

// The size an EGL surface or a locked buffer on `window` has: the buffer
// geometry the app set, else the content area in pixels. False if `window`
// is not the runtime's native window.
bool native_window_buffer_size(const void* window, int* w, int* h);

// A frame from the app's native code for the window, 0xAARRGGBB rows, top row
// first. Any thread (native_cocoa.cpp).
void guest_frame_present(const uint32_t* argb, int w, int h);

// The window's content area in pixels, once the render thread has run; else
// false (native_cocoa.cpp).
bool content_pixel_size(int* w, int* h);

// The guest-callable libandroid/libnativewindow functions (ANativeWindow_*),
// by name; null for other names.
void* native_window_resolve(const char* name);

// The guest-callable libEGL/libGLESv*, by name (guest_gl.cpp); null for other
// names.
void* guest_gl_resolve(const char* name);

}  // namespace winapk::host
