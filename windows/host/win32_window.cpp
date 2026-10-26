// win32_window.cpp -- see win32_window.h.

#include "win32_window.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <windows.h>
#include <wincodec.h>

namespace winapk::host {

namespace {

constexpr UINT kMsgShow = WM_APP + 1;
constexpr UINT kMsgResize = WM_APP + 2;
constexpr UINT kMsgShowState = WM_APP + 3;  // wParam: SW_MINIMIZE, SW_RESTORE or SW_HIDE
constexpr wchar_t kClassName[] = L"WinApkAppWindow";

// android.view.KeyEvent META_* bits.
constexpr int kMetaShiftOn = 0x1;
constexpr int kMetaAltOn = 0x2;
constexpr int kMetaCtrlOn = 0x1000;

HWND g_hwnd = nullptr;
WindowInput* g_input = nullptr;
int g_phone_w = 360, g_phone_h = 640;
bool g_pointer_down = false;

std::mutex g_frame_lock;
std::vector<uint32_t> g_frame;
int g_frame_w = 0, g_frame_h = 0;
std::vector<uint32_t> g_overlay;  // empty: none
int g_overlay_w = 0, g_overlay_h = 0;
std::vector<uint32_t> g_composed;

// The frame with the overlay blended over it ("source over", straight alpha),
// the overlay scaled to the frame by nearest sampling.
const uint32_t* composed_frame() {
    if (g_overlay.empty()) return g_frame.data();
    g_composed = g_frame;
    for (int y = 0; y < g_frame_h; ++y) {
        const int oy = y * g_overlay_h / g_frame_h;
        for (int x = 0; x < g_frame_w; ++x) {
            const uint32_t o = g_overlay[static_cast<size_t>(oy) * g_overlay_w + x * g_overlay_w / g_frame_w];
            const uint32_t a = o >> 24;
            if (a == 0) continue;
            uint32_t& d = g_composed[static_cast<size_t>(y) * g_frame_w + x];
            if (a == 255) {
                d = o;
                continue;
            }
            uint32_t out = 0xff000000u;
            for (int shift = 0; shift < 24; shift += 8) {
                const uint32_t sc = (o >> shift) & 0xff, dc = (d >> shift) & 0xff;
                out |= ((sc * a + dc * (255 - a)) / 255) << shift;
            }
            d = out;
        }
    }
    return g_composed.data();
}
COLORREF g_background = RGB(255, 255, 255);  // until the first frame

constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW;

UINT window_dpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && hwnd) return fn(hwnd);
    HDC dc = GetDC(nullptr);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

// ---- fitting the window to the work area ----------------------------------
//
// The window shows the phone at a scale in pixels per dp. At launch that is
// the monitor's own scale (its DPI / 96), unless the whole window -- title bar
// and borders included, plus a small margin -- would not fit the monitor's
// work area (the screen without the taskbar). Then it is the largest scale at
// which it fits, the same for width and height. The runtime derives the app's
// density from that same scale (window_plan), so the app draws its frames at
// exactly the window's pixel size: a small or scaled screen gets a lower
// density, not a shrunk bitmap, and the app keeps its dp layout (360x800 dp
// stays 360x800 dp).
//
// The density is fixed for the life of the process. When the window later
// moves to another monitor, the work area changes (the taskbar moved or grew)
// or the app turns the phone, the window is fitted again: it keeps its launch
// size in physical terms, made smaller when that no longer fits, and the
// frames are resampled to it. The next start picks the density that fits.

struct Plan {
    bool set = false;
    HMONITOR monitor = nullptr;
    UINT dpi = 96;
    float scale = 1.0f;  // px per dp at launch: the monitor's scale, or less to fit
};
Plan g_plan;
// px per dp the window shows the phone at on the launch monitor: the frames'
// own scale when the density came from the plan (1:1, nothing resampled),
// else (WINAPK_DENSITY) the plan's scale.
float g_base_scale = 1.0f;
float g_surface_scale = 1.0f;  // px per dp of the frames (the app's density / 160)
HMONITOR g_fit_monitor = nullptr;  // the monitor the window was last fitted for
bool g_in_size_move = false;
bool g_refit_pending = false;  // a fit came while minimized or maximized

UINT monitor_dpi(HMONITOR mon) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto fn = [] {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");  // Windows 8.1 and later
        return shcore ? reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
    }();
    UINT x = 0, y = 0;
    if (fn && mon && SUCCEEDED(fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x > 0) return x;
    return window_dpi(nullptr);
}

// The window rect around a client rect at `dpi` (Windows 10 1607 and later;
// before that, the system DPI's frame).
void adjust_for_dpi(RECT* r, UINT dpi) {
    using AdjustFn = BOOL(WINAPI*)(RECT*, DWORD, BOOL, DWORD, UINT);
    static auto fn = reinterpret_cast<AdjustFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    if (!fn || !fn(r, kStyle, FALSE, 0, dpi)) AdjustWindowRectEx(r, kStyle, FALSE, 0);
}

int margin_px(UINT dpi) { return MulDiv(4, static_cast<int>(dpi), 96); }

// "<w>x<h>", optionally followed by "+<x>+<y>" (from the left and top edge of
// the real work area) or "-<x>-<y>" (from the right and bottom edge).
bool parse_geometry(const wchar_t* s, const RECT& real, RECT* out) {
    int w = 0, h = 0, n = 0;
    if (swscanf(s, L"%dx%d%n", &w, &h, &n) < 2 || w < 100 || h < 100) return false;
    s += n;
    int offs[2] = {0, 0};
    bool from_end[2] = {false, false};
    for (int i = 0; i < 2 && (*s == L'+' || *s == L'-'); ++i) {
        from_end[i] = *s == L'-';
        int v = 0, m = 0;
        if (swscanf(s + 1, L"%d%n", &v, &m) < 1) return false;
        offs[i] = v;
        s += 1 + m;
    }
    const LONG x = from_end[0] ? real.right - offs[0] - w : real.left + offs[0];
    const LONG y = from_end[1] ? real.bottom - offs[1] - h : real.top + offs[1];
    *out = RECT{x, y, x + w, y + h};
    return true;
}

// WINAPK_TEST_WORKAREA=<w>x<h>[+<x>+<y> | -<x>-<y>], in pixels, stands in for
// every monitor's work area, so that fitting can be tried on a big screen.
// "@<file>" reads that text from the file at every fit, so a test can change
// the work area while the app runs (then send WM_SETTINGCHANGE).
bool test_work_area(const RECT& real, RECT* out) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(L"WINAPK_TEST_WORKAREA", buf, 1024);
    if (n == 0 || n >= 1024) return false;
    if (buf[0] != L'@') return parse_geometry(buf, real, out);
    char text[128] = {};
    HANDLE f = CreateFileW(buf + 1, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    ReadFile(f, text, sizeof(text) - 1, &got, nullptr);
    CloseHandle(f);
    wchar_t wtext[128] = {};
    for (DWORD i = 0; i < got; ++i) wtext[i] = static_cast<unsigned char>(text[i]);
    return parse_geometry(wtext, real, out);
}

RECT work_area(HMONITOR mon) {
    RECT wa{};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    RECT test;
    if (test_work_area(wa, &test)) wa = test;
    return wa;
}

// The largest px per dp at which a phone of w x h dp, with the window's frame
// at `dpi` and the margin on every side, fits in `wa`.
float fit_scale(const RECT& wa, UINT dpi, int w, int h) {
    RECT f{0, 0, 0, 0};
    adjust_for_dpi(&f, dpi);
    const int m = margin_px(dpi);
    const float aw = static_cast<float>((wa.right - wa.left) - (f.right - f.left) - 2 * m);
    const float ah = static_cast<float>((wa.bottom - wa.top) - (f.bottom - f.top) - 2 * m);
    if (w <= 0 || h <= 0 || aw <= 0 || ah <= 0) return 0.25f;
    return std::max(0.25f, std::min(aw / static_cast<float>(w), ah / static_cast<float>(h)));
}

int client_px(int dp, float scale) { return static_cast<int>(static_cast<float>(dp) * scale + 0.5f); }

// What the window shows the phone at on a monitor at `dpi` with work area
// `wa`: its launch size in physical terms, or less to fit.
float target_scale(UINT dpi, const RECT& wa) {
    const float keep = g_base_scale * static_cast<float>(dpi) / static_cast<float>(g_plan.dpi ? g_plan.dpi : 96);
    return std::min(keep, fit_scale(wa, dpi, g_phone_w, g_phone_h));
}

// The window size whose client area shows the phone at `scale`.
SIZE outer_size(UINT dpi, float scale) {
    RECT r{0, 0, client_px(g_phone_w, scale), client_px(g_phone_h, scale)};
    adjust_for_dpi(&r, dpi);
    return {r.right - r.left, r.bottom - r.top};
}

// Sizes the window for monitor `mon` at `dpi`, at `at` (its current place when
// null); with `keep_inside`, moved as needed to lie wholly in the work area.
void fit_window(HWND hwnd, HMONITOR mon, UINT dpi, const RECT* at, bool keep_inside) {
    if (IsIconic(hwnd) || IsZoomed(hwnd)) {
        g_refit_pending = true;
        return;
    }
    g_refit_pending = false;
    g_fit_monitor = mon;
    const RECT wa = work_area(mon);
    const float scale = target_scale(dpi, wa);
    const SIZE s = outer_size(dpi, scale);
    RECT wr{};
    if (at) wr = *at;
    else GetWindowRect(hwnd, &wr);
    LONG x = wr.left, y = wr.top;
    if (keep_inside) {
        const int m = margin_px(dpi);
        x = std::max(std::min(x, wa.right - m - s.cx), wa.left + m);
        y = std::max(std::min(y, wa.bottom - m - s.cy), wa.top + m);
    }
    SetWindowPos(hwnd, nullptr, x, y, s.cx, s.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    static float s_logged = 0;
    static SIZE s_logged_size{};
    if (scale != s_logged || s.cx != s_logged_size.cx || s.cy != s_logged_size.cy) {
        s_logged = scale;
        s_logged_size = s;
        std::fprintf(stderr,
                     "[win.apk] window: %dx%d dp at %.3f px/dp (client %dx%d, frames %dx%d), %u dpi, work area "
                     "%ldx%ld at %ld,%ld\n",
                     g_phone_w, g_phone_h, scale, client_px(g_phone_w, scale), client_px(g_phone_h, scale),
                     client_px(g_phone_w, g_surface_scale), client_px(g_phone_h, g_surface_scale), dpi,
                     wa.right - wa.left, wa.bottom - wa.top, wa.left, wa.top);
    }
}

void refit(HWND hwnd) {
    fit_window(hwnd, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), window_dpi(hwnd), nullptr, true);
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    {
        std::lock_guard<std::mutex> hold(g_frame_lock);
        if (g_frame_w > 0 && g_frame_h > 0) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = g_frame_w;
            bi.bmiHeader.biHeight = -g_frame_h;  // top-down, like the ARGB rows
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, g_frame_w, g_frame_h, composed_frame(), &bi,
                          DIB_RGB_COLORS, SRCCOPY);
        } else {
            HBRUSH brush = CreateSolidBrush(g_background);
            FillRect(dc, &client, brush);
            DeleteObject(brush);
        }
    }
    EndPaint(hwnd, &ps);
}

void send_pointer(HWND hwnd, int action, LPARAM lp) {
    RECT client;
    GetClientRect(hwnd, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    const float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
    const float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
    g_input->pointer(action, x, y, static_cast<float>(client.right), static_cast<float>(client.bottom));
}

// A wheel step becomes a short drag, as upstream turns a mouse wheel into
// DOWN, MOVE, UP (cocoa_bridge.mm scrollWheel:): a line is 16 points, scaled by
// MACAPK_SCROLL_SCALE (default 0.5), and a notch is the user's "lines per
// notch" setting. Wheel up scrolls toward the top, so the finger moves down.
void send_wheel(HWND hwnd, WPARAM wp, LPARAM lp, bool horizontal) {
    if (g_pointer_down) return;  // a drag is in progress; do not interleave
    static const float scale = [] {
        const char* e = std::getenv("MACAPK_SCROLL_SCALE");
        const float v = e ? static_cast<float>(std::atof(e)) : 0.5f;
        return (v >= 0.05f && v <= 4.0f) ? v : 0.5f;
    }();
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == WHEEL_PAGESCROLL || lines == 0) lines = 3;
    const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
    const float points = notches * static_cast<float>(lines) * 16.0f * scale;
    RECT client;
    GetClientRect(hwnd, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    POINT p{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};  // screen coordinates
    ScreenToClient(hwnd, &p);
    const float w = static_cast<float>(client.right), h = static_cast<float>(client.bottom);
    // Client pixels per point, along the scroll axis.
    const float per_point = horizontal ? w / static_cast<float>(g_phone_w) : h / static_cast<float>(g_phone_h);
    // A scrolling view takes the gesture over on the first move past the touch
    // slop (8 dp) and scrolls only on later moves, so the drag first steps just
    // past the slop and then by the whole amount. Upstream's single move gets
    // swallowed by that first step. The events share a timestamp, so there is
    // no fling.
    const float sign = horizontal ? (points > 0 ? -1.0f : 1.0f) : (points > 0 ? 1.0f : -1.0f);
    const float slop = 10.0f * per_point * sign;
    const float travel = std::fabs(points) * per_point * sign;
    const float x0 = static_cast<float>(p.x), y0 = static_cast<float>(p.y);
    const float x1 = horizontal ? x0 + slop : x0, y1 = horizontal ? y0 : y0 + slop;
    const float x2 = horizontal ? x1 + travel : x0, y2 = horizontal ? y0 : y1 + travel;
    g_input->pointer(0, x0, y0, w, h);
    g_input->pointer(2, x1, y1, w, h);
    g_input->pointer(2, x2, y2, w, h);
    g_input->pointer(1, x2, y2, w, h);
}

int meta_state() {
    int meta = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) meta |= kMetaShiftOn;
    if (GetKeyState(VK_MENU) & 0x8000) meta |= kMetaAltOn;
    if (GetKeyState(VK_CONTROL) & 0x8000) meta |= kMetaCtrlOn;
    return meta;
}

void send_key(int action, WPARAM vk, LPARAM lp) {
    BYTE state[256];
    GetKeyboardState(state);
    wchar_t buf[4] = {};
    const UINT scan = (lp >> 16) & 0xff;
    // Flag 4: do not change the keyboard state (dead keys stay pending).
    const int n = ToUnicode(static_cast<UINT>(vk), scan, state, buf, 4, 4);
    const int meta = meta_state();
    wchar_t ch = n == 1 ? buf[0] : 0;
    // What KeyEvent.getUnicodeChar gives on a device: Enter is '\n', Tab is
    // '\t', other control keys and Ctrl chords (upstream's host_keymap.h) none.
    if (ch == L'\r') ch = L'\n';
    if ((meta & kMetaCtrlOn) || (ch < 0x20 && ch != L'\n' && ch != L'\t')) ch = 0;
    const int code = android_key_code(static_cast<unsigned>(vk), ch);
    if (code == 0 && ch == 0) return;
    g_input->key(action, code, ch, meta);
}

// Whether the foreground window is one of this process's (the app window, a
// browser tab, a dialog), i.e. whether the user is in the app right now.
bool foreground_is_ours() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return fg && pid == GetCurrentProcessId();
}

// The process was started with a show command that does not activate
// (STARTF_USESHOWWINDOW with SW_SHOWNOACTIVATE, SW_SHOWNA or a minimized one):
// its first window is shown without taking the foreground, as Windows' own
// first-ShowWindow rule does for a program that passes the command through.
bool started_without_activation() {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    GetStartupInfoW(&si);
    if (!(si.dwFlags & STARTF_USESHOWWINDOW)) return false;
    switch (si.wShowWindow) {
        case SW_SHOWNOACTIVATE:
        case SW_SHOWNA:
        case SW_SHOWMINNOACTIVE:
        case SW_SHOWMINIMIZED:
        case SW_MINIMIZE:
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // paint covers the whole client area
        case WM_LBUTTONDOWN:
            // A click on the app's own pixels takes the keyboard back from a
            // WebView placed in the window (native_webview.cpp). Only in the
            // active window: SetFocus would activate an inactive one.
            if (GetActiveWindow() == hwnd && GetFocus() != hwnd) SetFocus(hwnd);
            SetCapture(hwnd);
            g_pointer_down = true;
            send_pointer(hwnd, 0, lp);
            return 0;
        case WM_MOUSEMOVE:
            if (g_pointer_down) send_pointer(hwnd, 2, lp);
            return 0;
        case WM_LBUTTONUP:
            if (g_pointer_down) {
                g_pointer_down = false;
                send_pointer(hwnd, 1, lp);
                ReleaseCapture();
            }
            return 0;
        case WM_MOUSEWHEEL:
            send_wheel(hwnd, wp, lp, false);
            return 0;
        case WM_MOUSEHWHEEL:
            send_wheel(hwnd, wp, lp, true);
            return 0;
        case WM_CAPTURECHANGED:
            if (g_pointer_down) {  // capture taken away mid-drag
                g_pointer_down = false;
                send_pointer(hwnd, 3, 0);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;  // Alt+F4 closes, as everywhere
            // Alt+Left is Back, as in a browser (Escape is Back too).
            if (msg == WM_SYSKEYDOWN && wp == VK_LEFT) {
                g_input->key(0, 4, 0, 0);
                return 0;
            }
            send_key(0, wp, lp);
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (msg == WM_SYSKEYUP && wp == VK_LEFT) {
                g_input->key(1, 4, 0, 0);
                return 0;
            }
            send_key(1, wp, lp);
            return 0;
        // The mouse's back side button is Back too.
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) g_input->key(msg == WM_XBUTTONDOWN ? 0 : 1, 4, 0, 0);
            return TRUE;
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;  // characters already went out with the key-down
        // Another monitor's scale: Windows suggests a rect at the new DPI; the
        // window takes its place but fits its size to that monitor. While the
        // user drags, it is not pushed around; WM_EXITSIZEMOVE settles it.
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            fit_window(hwnd, MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), HIWORD(wp), r, !g_in_size_move);
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            g_in_size_move = true;
            break;
        case WM_EXITSIZEMOVE:
            g_in_size_move = false;
            if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != g_fit_monitor) refit(hwnd);
            break;
        // Moved to another monitor without a drag (Win+Shift+Arrow, a
        // program): fitted for that monitor's work area.
        case WM_WINDOWPOSCHANGED:
            if (!g_in_size_move && g_fit_monitor && IsWindowVisible(hwnd) &&
                MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != g_fit_monitor) {
                refit(hwnd);
            }
            break;
        // The work area changed: the taskbar moved, grew or was set to hide.
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) refit(hwnd);
            break;
        case WM_DISPLAYCHANGE:
            refit(hwnd);
            break;
        // The first show takes the foreground (the user just started the
        // app), unless whoever started the process asked for no activation;
        // later ones -- an activity starting, the task coming to the front --
        // only from a window of this process. A WebView2 in the process
        // (webview2_host.h) lets it take the foreground from any program,
        // and an app must not pull the user out of another one because a
        // page redirected or an intent arrived.
        case kMsgShow: {
            static bool shown = false;
            const bool first = !shown;
            shown = true;
            if ((first && !started_without_activation()) || (!first && foreground_is_ours())) {
                ShowWindow(hwnd, SW_SHOWNORMAL);
                SetForegroundWindow(hwnd);
            } else if (!IsWindowVisible(hwnd)) {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
            return 0;
        }
        case kMsgShowState:
            if (wp == SW_RESTORE && !foreground_is_ours()) {
                ShowWindow(hwnd, IsIconic(hwnd) ? SW_SHOWNOACTIVATE : SW_SHOWNA);
                return 0;
            }
            ShowWindow(hwnd, static_cast<int>(wp));
            if (wp == SW_RESTORE) SetForegroundWindow(hwnd);
            return 0;
        case kMsgResize:  // the app turned the phone
            refit(hwnd);
            return 0;
        case WM_CLOSE:
            g_input->close_requested();
            return 0;
        case WM_SIZE: {
            // BACK at the task root minimizes the window (moveTaskToBack) and
            // stops the top activity; un-minimizing must resume it, or every
            // touch after that goes to no activity at all.
            static bool minimized = false;
            if (wp == SIZE_MINIMIZED) {
                minimized = true;
            } else if (minimized && (wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED)) {
                minimized = false;
                g_input->restored();
            }
            // A fit that came while minimized or maximized.
            if (wp == SIZE_RESTORED && g_refit_pending) PostMessageW(hwnd, kMsgResize, 0, 0);
            break;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct StartArgs {
    HANDLE ready;
    bool ok;
};

DWORD WINAPI ui_thread(void* param) {
    auto* args = static_cast<StartArgs*>(param);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    const SIZE s = outer_size(g_plan.dpi, target_scale(g_plan.dpi, work_area(g_plan.monitor)));
    g_hwnd = CreateWindowExW(0, kClassName, L"Win.apk", kStyle, CW_USEDEFAULT, CW_USEDEFAULT, s.cx, s.cy, nullptr,
                             nullptr, wc.hInstance, nullptr);
    if (g_hwnd) {
        // CW_USEDEFAULT cascades the window down the primary monitor. It
        // opens on the monitor the plan was made for (where the user started
        // the app), wholly inside its work area.
        RECT wr{};
        GetWindowRect(g_hwnd, &wr);
        if (g_plan.monitor && MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST) != g_plan.monitor) {
            const RECT wa = work_area(g_plan.monitor);
            const LONG w = wr.right - wr.left, h = wr.bottom - wr.top;
            const LONG x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;
            wr = RECT{x, y, x + w, y + h};
            SetWindowPos(g_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        fit_window(g_hwnd, g_plan.monitor ? g_plan.monitor : MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST),
                   window_dpi(g_hwnd), &wr, true);
    }
    args->ok = g_hwnd != nullptr;
    SetEvent(args->ready);
    if (!g_hwnd) return 1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// Per-monitor DPI awareness, so Windows does not bitmap-stretch the window.
// Looked up at runtime: older Windows 10 builds lack the call.
void enable_dpi_awareness() {
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    auto fn = reinterpret_cast<SetContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (fn) fn(reinterpret_cast<HANDLE>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

}  // namespace

float window_plan(int phone_w, int phone_h) {
    enable_dpi_awareness();
    // The monitor the user started the app from: the foreground window's
    // (the launcher, a Start menu, Explorer), else the primary one.
    HWND fg = GetForegroundWindow();
    g_plan.monitor = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY)
                        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    g_plan.dpi = monitor_dpi(g_plan.monitor);
    const float natural = static_cast<float>(g_plan.dpi) / 96.0f;
    g_plan.scale = std::min(natural, fit_scale(work_area(g_plan.monitor), g_plan.dpi, phone_w, phone_h));
    g_plan.set = true;
    return g_plan.scale;
}

int window_density_dpi(float scale) {
    return std::clamp(static_cast<int>(std::floor(scale * 160.0f + 0.001f)), 120, 640);
}

bool window_start(int phone_w, int phone_h, float surface_scale, WindowInput* input) {
    if (g_hwnd) return true;
    g_input = input;
    if (!g_plan.set) window_plan(phone_w, phone_h);
    g_phone_w = phone_w;
    g_phone_h = phone_h;
    g_surface_scale = surface_scale > 0 ? surface_scale : 1.0f;
    // The density came from the plan (no WINAPK_DENSITY): the window shows
    // the frames pixel for pixel.
    const bool planned = std::fabs(g_surface_scale * 160.0f - static_cast<float>(window_density_dpi(g_plan.scale))) < 0.5f;
    g_base_scale = planned ? g_surface_scale : g_plan.scale;
    StartArgs args{CreateEventW(nullptr, TRUE, FALSE, nullptr), false};
    HANDLE t = CreateThread(nullptr, 0, ui_thread, &args, 0, nullptr);
    if (!t) return false;
    WaitForSingleObject(args.ready, INFINITE);
    CloseHandle(args.ready);
    CloseHandle(t);
    return args.ok;
}

bool window_running() { return g_hwnd != nullptr; }

namespace {

// Decodes `path` with WIC and scales it to size x size, as a top-down 32-bit
// BGRA DIB with straight alpha, which is what an icon's color bitmap takes.
HICON icon_from_png(const std::wstring& path, int size) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HICON icon = nullptr;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame, size, size, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(scaler, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0.0, WICBitmapPaletteTypeCustom))) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        if (color && mask &&
            SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(size) * 4,
                                            static_cast<UINT>(size) * static_cast<UINT>(size) * 4,
                                            static_cast<BYTE*>(bits)))) {
            ICONINFO ii{};
            ii.fIcon = TRUE;
            ii.hbmMask = mask;
            ii.hbmColor = color;
            icon = CreateIconIndirect(&ii);
        }
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
    }
    if (converter) converter->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return icon;
}

}  // namespace

bool window_set_icon_png(const std::wstring& path) {
    if (!g_hwnd) return false;
    // The sizes Windows asks for at the window's DPI: the title bar takes the
    // small one, Alt+Tab and the taskbar the big one.
    using DpiFn = UINT(WINAPI*)(HWND);
    using MetricsFn = int(WINAPI*)(int, UINT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto dpi_for = reinterpret_cast<DpiFn>(GetProcAddress(user32, "GetDpiForWindow"));
    auto metrics_for = reinterpret_cast<MetricsFn>(GetProcAddress(user32, "GetSystemMetricsForDpi"));
    const UINT dpi = dpi_for ? dpi_for(g_hwnd) : 96;
    auto metric = [&](int m) { return metrics_for ? metrics_for(m, dpi) : GetSystemMetrics(m); };
    HICON big = icon_from_png(path, metric(SM_CXICON));
    HICON small = icon_from_png(path, metric(SM_CXSMICON));
    if (!big || !small) {
        if (big) DestroyIcon(big);
        if (small) DestroyIcon(small);
        return false;
    }
    // The window keeps using the icons, so the previous pair is freed only
    // once the new one is in.
    static HICON s_big = nullptr, s_small = nullptr;
    SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    if (s_big) DestroyIcon(s_big);
    if (s_small) DestroyIcon(s_small);
    s_big = big;
    s_small = small;
    return true;
}

void window_show() {
    if (g_hwnd) PostMessageW(g_hwnd, kMsgShow, 0, 0);
}

void window_set_title(const std::string& utf8) {
    if (!g_hwnd) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    SetWindowTextW(g_hwnd, w.c_str());  // sends to the UI thread
}

void window_set_phone_size(int phone_w, int phone_h) {
    g_phone_w = phone_w;
    g_phone_h = phone_h;
    if (g_hwnd) PostMessageW(g_hwnd, kMsgResize, 0, 0);
}

void* window_handle() { return g_hwnd; }

void window_hide() {
    if (g_hwnd) PostMessageW(g_hwnd, kMsgShowState, SW_HIDE, 0);
}

void window_minimize() {
    if (g_hwnd) PostMessageW(g_hwnd, kMsgShowState, SW_MINIMIZE, 0);
}

void window_restore() {
    if (g_hwnd) PostMessageW(g_hwnd, kMsgShowState, SW_RESTORE, 0);
}

void window_set_background(uint32_t argb) {
    g_background = RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void window_present(const uint32_t* argb, int w, int h) {
    if (!g_hwnd || w <= 0 || h <= 0) return;
    {
        std::lock_guard<std::mutex> hold(g_frame_lock);
        g_frame.assign(argb, argb + static_cast<size_t>(w) * static_cast<size_t>(h));
        g_frame_w = w;
        g_frame_h = h;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void window_set_overlay(const uint32_t* argb, int w, int h) {
    if (!g_hwnd || w <= 0 || h <= 0) return;
    {
        std::lock_guard<std::mutex> hold(g_frame_lock);
        g_overlay.assign(argb, argb + static_cast<size_t>(w) * static_cast<size_t>(h));
        g_overlay_w = w;
        g_overlay_h = h;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void window_clear_overlay() {
    {
        std::lock_guard<std::mutex> hold(g_frame_lock);
        if (g_overlay.empty()) return;
        g_overlay.clear();
        g_composed.clear();
    }
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void window_set_secure(bool secure) {
    // WDA_EXCLUDEFROMCAPTURE (Windows 10 2004); older systems show it black.
    if (g_hwnd && !SetWindowDisplayAffinity(g_hwnd, secure ? 0x11 : WDA_NONE) && secure) {
        SetWindowDisplayAffinity(g_hwnd, WDA_MONITOR);
    }
}

int android_key_code(unsigned vk, wchar_t ch) {
    if (vk >= '0' && vk <= '9') return 7 + static_cast<int>(vk - '0');   // KEYCODE_0..9
    if (vk >= 'A' && vk <= 'Z') return 29 + static_cast<int>(vk - 'A');  // KEYCODE_A..Z
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return 144 + static_cast<int>(vk - VK_NUMPAD0);
    switch (vk) {
        case VK_RETURN: return 66;   // ENTER
        case VK_BACK: return 67;     // DEL
        case VK_DELETE: return 112;  // FORWARD_DEL
        case VK_SPACE: return 62;
        case VK_TAB: return 61;
        case VK_ESCAPE: return 4;    // BACK, as upstream maps Escape
        case VK_LEFT: return 21;
        case VK_RIGHT: return 22;
        case VK_UP: return 19;
        case VK_DOWN: return 20;
        case VK_HOME: return 122;    // MOVE_HOME
        case VK_END: return 123;     // MOVE_END
        case VK_PRIOR: return 92;    // PAGE_UP
        case VK_NEXT: return 93;     // PAGE_DOWN
        case VK_SHIFT: case VK_LSHIFT: return 59;
        case VK_RSHIFT: return 60;
        case VK_CONTROL: case VK_LCONTROL: return 113;
        case VK_RCONTROL: return 114;
        case VK_MENU: case VK_LMENU: return 57;
        case VK_RMENU: return 58;
        default: break;
    }
    switch (ch) {  // punctuation by character, layout-independent
        case L'.': return 56; case L',': return 55; case L'-': return 69;
        case L'=': return 70; case L'+': return 81; case L';': return 74;
        case L'\'': return 75; case L'/': return 76; case L'\\': return 73;
        case L'[': return 71; case L']': return 72; case L'`': return 68;
        default: break;
    }
    return 0;  // KEYCODE_UNKNOWN; the character still goes out
}

}  // namespace winapk::host
