// win32_window.h -- the one top-level window an app runs in.
//
// Upstream draws into an NSWindow per app; this is its Win32 counterpart, kept
// apart from the JNI code in native_cocoa.cpp. The window has its own UI thread
// with a message loop, so nothing here depends on which Java thread calls in.
//
// Frames arrive as ARGB pixels at the app's surface size (phone points times
// the device density) and are scaled to the client area with GDI. Mouse and
// keyboard input go back out through WindowInput, already translated to
// Android's MotionEvent and KeyEvent terms.

#pragma once

#include <cstdint>
#include <string>

namespace winapk::host {

// Called on the window's UI thread.
class WindowInput {
public:
    virtual ~WindowInput() = default;
    // MotionEvent action (DOWN 0, UP 1, MOVE 2, CANCEL 3); x and y in client
    // pixels, origin at the top left, of a client area view_w by view_h.
    virtual void pointer(int action, float x, float y, float view_w, float view_h) = 0;
    // KeyEvent action (DOWN 0, UP 1), KEYCODE_*, the character or 0, META_*.
    virtual void key(int action, int key_code, int unicode, int meta) = 0;
    // The user closed the window.
    virtual void close_requested() = 0;
};

// Creates the window (hidden) for a phone of phone_w by phone_h points.
// Returns false when there is no desktop to put a window on.
bool window_start(int phone_w, int phone_h, WindowInput* input);
bool window_running();

void window_show();
void window_set_title(const std::string& utf8);
void window_set_phone_size(int phone_w, int phone_h);
// The app moved its task to the back or front (Activity.moveTaskToBack).
void window_minimize();
void window_restore();
// The window and taskbar icon, from a PNG (the app's rendered launcher icon).
// Returns false if the file cannot be read or decoded.
bool window_set_icon_png(const std::wstring& path);
// The color shown until the first frame (the theme's windowBackground).
void window_set_background(uint32_t argb);
// Copies the frame; it is painted on the UI thread.
void window_present(const uint32_t* argb, int w, int h);

// Translates a Windows virtual key to an Android KEYCODE_* (0 if none), as
// upstream's host_keymap.h does for macOS key codes.
int android_key_code(unsigned vk, wchar_t ch);

}  // namespace winapk::host
