// win32_window.cpp -- see win32_window.h.

#include "win32_window.h"

#include <cmath>
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
        case kMsgShowState:
            ShowWindow(hwnd, static_cast<int>(wp));
            if (wp == SW_RESTORE) SetForegroundWindow(hwnd);
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
