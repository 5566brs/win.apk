// win32_window.cpp -- see win32_window.h.

#include "win32_window.h"

#include <cstdio>
#include <mutex>
#include <vector>

#include <windows.h>

namespace winapk::host {

namespace {

constexpr UINT kMsgShow = WM_APP + 1;
constexpr UINT kMsgResize = WM_APP + 2;
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

// The window size whose client area shows the phone at the monitor's scale.
SIZE outer_size(HWND hwnd, DWORD style) {
    const UINT dpi = window_dpi(hwnd);
    RECT r{0, 0, MulDiv(g_phone_w, dpi, 96), MulDiv(g_phone_h, dpi, 96)};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    return {r.right - r.left, r.bottom - r.top};
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
            StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, g_frame_w, g_frame_h, g_frame.data(), &bi,
                          DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
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

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // paint covers the whole client area
        case WM_LBUTTONDOWN:
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
        case WM_CAPTURECHANGED:
            if (g_pointer_down) {  // capture taken away mid-drag
                g_pointer_down = false;
                send_pointer(hwnd, 3, 0);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;  // Alt+F4 closes, as everywhere
            send_key(0, wp, lp);
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            send_key(1, wp, lp);
            return 0;
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;  // characters already went out with the key-down
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case kMsgShow:
            ShowWindow(hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd);
            return 0;
        case kMsgResize: {
            const SIZE s = outer_size(hwnd, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)));
            SetWindowPos(hwnd, nullptr, 0, 0, s.cx, s.cy, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            g_input->close_requested();
            return 0;
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
    const DWORD style = WS_OVERLAPPEDWINDOW;
    const SIZE s = outer_size(nullptr, style);
    g_hwnd = CreateWindowExW(0, kClassName, L"Win.apk", style, CW_USEDEFAULT, CW_USEDEFAULT, s.cx, s.cy, nullptr,
                             nullptr, wc.hInstance, nullptr);
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

bool window_start(int phone_w, int phone_h, WindowInput* input) {
    if (g_hwnd) return true;
    g_input = input;
    g_phone_w = phone_w;
    g_phone_h = phone_h;
    enable_dpi_awareness();
    StartArgs args{CreateEventW(nullptr, TRUE, FALSE, nullptr), false};
    HANDLE t = CreateThread(nullptr, 0, ui_thread, &args, 0, nullptr);
    if (!t) return false;
    WaitForSingleObject(args.ready, INFINITE);
    CloseHandle(args.ready);
    CloseHandle(t);
    return args.ok;
}

bool window_running() { return g_hwnd != nullptr; }

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
