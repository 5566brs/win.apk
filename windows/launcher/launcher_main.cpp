// launcher_main.cpp -- Win.apk.exe, the app library.
//
// The Windows counterpart of upstream's launcher (launcher/*.swift), in its
// first form: one window that lists the installed apps with their icons and
// names. An APK dropped on the window, opened through "Add app", double-clicked
// in Explorer (the launcher registers itself for .apk files) or passed on the
// command line is installed:
//
//   1. copied into %LOCALAPPDATA%\Win.apk\library, so the app stays when the
//      download is deleted;
//   2. installed and converted once by winapk-runtime --install-only, which
//      also writes the app's icon and name (apps\<name>\icon.png, label.txt;
//      the name moves next to the APK as library\<name>.label, because the
//      app's first run moves files it does not know out of apps\<name>);
//   3. given a Start menu shortcut (Start > Win.apk Apps) and a desktop
//      shortcut, with the app's own icon, that start it through this program.
//
// Double-clicking an app runs it (winapk-runtime --apk, without a console);
// if it stops with an error the log's path is shown. The right-click menu
// opens, makes the desktop shortcut again, shows the log, or removes the app
// (its saved data stays, as an uninstall that keeps data would).
//
//   Win.apk.exe                      the library
//   Win.apk.exe <app.apk>            install, then run it
//   Win.apk.exe --run <library apk>  run an installed app (what shortcuts do)

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr UINT kInstalled = WM_APP + 1;  // wParam: ok, lParam: new std::wstring (the app's name)
constexpr UINT kRunFailed = WM_APP + 2;  // lParam: new std::wstring (the log)
constexpr int kIdList = 100, kIdAdd = 101, kIdStatus = 102, kIdEmpty = 103;
constexpr int kCmdOpen = 200, kCmdShortcut = 201, kCmdLog = 202, kCmdRemove = 203;

HINSTANCE g_inst;
HWND g_main, g_list, g_add, g_status, g_empty;
HIMAGELIST g_images;
HFONT g_font, g_title_font, g_sub_font, g_empty_font;
HICON g_app_icon_large;
int g_dpi = 96;
int g_installing = 0;
std::vector<std::wstring> g_run_after;  // installs started from the command line run when done

int px(int v) { return MulDiv(v, g_dpi, 96); }

// ---- places ------------------------------------------------------------------

fs::path exe_path() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return fs::path(std::wstring(buf, n));
}
fs::path root() { return exe_path().parent_path(); }
fs::path runtime_exe() { return root() / L"bin" / L"winapk-runtime.exe"; }

fs::path data_dir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 4);
    return fs::path(std::wstring(buf, n)) / L"Win.apk";
}
fs::path library_dir() { return data_dir() / L"library"; }
fs::path app_dir(const std::wstring& name) { return data_dir() / L"apps" / name; }
fs::path logs_dir() { return data_dir() / L"logs"; }

fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}
fs::path start_menu_apps() { return known_folder(FOLDERID_Programs) / L"Win.apk Apps"; }

bool is_app_file(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext == L".apk" || ext == L".apkm" || ext == L".xapk" || ext == L".apks";
}

std::wstring utf8_to_wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// A name Windows accepts as a file name.
std::wstring file_name_safe(std::wstring s) {
    for (auto& c : s) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    }
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    return s.empty() ? L"App" : s;
}

// ---- the library ---------------------------------------------------------------

struct App {
    std::wstring name;   // the library file's stem: the key everywhere
    std::wstring label;  // what the user sees
    fs::path apk;
};

fs::path label_file(const std::wstring& name) { return library_dir() / (name + L".label"); }
fs::path shortcut_icon(const std::wstring& name) { return library_dir() / (name + L".ico"); }

std::wstring label_of(const std::wstring& name) {
    std::ifstream f(label_file(name), std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::wstring w = utf8_to_wide(s);
    return w.empty() ? name : w;
}

std::vector<App> library() {
    std::vector<App> apps;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(library_dir(), ec)) {
        if (!e.is_regular_file(ec) || !is_app_file(e.path())) continue;
        const std::wstring name = e.path().stem().wstring();
        if (!fs::exists(label_file(name), ec)) continue;  // not installed (yet)
        apps.push_back({name, label_of(name), e.path()});
    }
    std::sort(apps.begin(), apps.end(), [](const App& a, const App& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.label.c_str(), -1, b.label.c_str(), -1,
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return apps;
}

// ---- icons -------------------------------------------------------------------

// A PNG as a 32-bit premultiplied bitmap of size x size, for the image list.
HBITMAP load_png(const fs::path& png, int size) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        return nullptr;
    }
    HBITMAP bmp = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(png.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                 &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame, size, size, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && FAILED(conv->CopyPixels(nullptr, size * 4, size * size * 4, static_cast<BYTE*>(bits)))) {
            DeleteObject(bmp);
            bmp = nullptr;
        }
    }
    for (IUnknown* u : {static_cast<IUnknown*>(conv), static_cast<IUnknown*>(scaler), static_cast<IUnknown*>(frame),
                        static_cast<IUnknown*>(dec), static_cast<IUnknown*>(wic)}) {
        if (u) u->Release();
    }
    return bmp;
}

// An .ico that holds the PNG as is (Windows reads PNG-compressed icons), for
// shortcuts.
bool write_ico(const fs::path& png, const fs::path& ico) {
    std::ifstream f(png, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 24 || data[1] != 'P') return false;
    const uint32_t w = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
    const uint32_t h = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
    std::ofstream o(ico, std::ios::binary | std::ios::trunc);
    auto u16 = [&](uint16_t v) { o.put(static_cast<char>(v & 0xff)).put(static_cast<char>(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(static_cast<uint16_t>(v & 0xffff)); u16(static_cast<uint16_t>(v >> 16)); };
    u16(0), u16(1), u16(1);
    o.put(static_cast<char>(w >= 256 ? 0 : w)).put(static_cast<char>(h >= 256 ? 0 : h)).put(0).put(0);
    u16(1), u16(32), u32(static_cast<uint32_t>(data.size())), u32(22);
    o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(o);
}

bool make_shortcut(const fs::path& lnk, const std::wstring& args, const fs::path& icon, const std::wstring& what) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(exe_path().c_str());
    link->SetArguments(args.c_str());
    link->SetWorkingDirectory(root().c_str());
    link->SetDescription(what.c_str());
    if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
    if (ok) {
        std::error_code ec;
        fs::create_directories(lnk.parent_path(), ec);
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

std::wstring run_args(const fs::path& apk) { return L"--run \"" + apk.wstring() + L"\""; }

void make_app_shortcuts(const App& app, bool desktop) {
    const fs::path ico = shortcut_icon(app.name);
    const bool has_icon = write_ico(app_dir(app.name) / L"icon.png", ico);
    const std::wstring file = file_name_safe(app.label) + L".lnk";
    const std::wstring what = app.label + L" (Android app, Win.apk)";
    make_shortcut(start_menu_apps() / file, run_args(app.apk), has_icon ? ico : fs::path(), what);
    if (desktop) {
        make_shortcut(known_folder(FOLDERID_Desktop) / file, run_args(app.apk), has_icon ? ico : fs::path(), what);
    }
}

void remove_app_shortcuts(const App& app) {
    std::error_code ec;
    const std::wstring file = file_name_safe(app.label) + L".lnk";
    fs::remove(start_menu_apps() / file, ec);
    fs::remove(known_folder(FOLDERID_Desktop) / file, ec);
}

// Win.apk itself: a Start menu entry, and .apk files open here. Per user, so
// no administrator rights; an .apk already claimed by another program stays
// with it.
void integrate_with_windows() {
    const fs::path self = known_folder(FOLDERID_Programs) / L"Win.apk.lnk";
    std::error_code ec;
    if (!fs::exists(self, ec)) make_shortcut(self, L"", fs::path(), L"Android apps on Windows");

    HKEY key = nullptr;
    wchar_t current[256] = {};
    DWORD size = sizeof(current);
    const bool claimed = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.apk", nullptr, RRF_RT_REG_SZ, nullptr,
                                      current, &size) == ERROR_SUCCESS &&
                         current[0] && wcscmp(current, L"WinApk.Package") != 0;
    if (claimed) return;
    const std::wstring exe = exe_path().wstring();
    const std::wstring open = L"\"" + exe + L"\" \"%1\"";
    const std::wstring icon = L"\"" + exe + L"\",0";
    auto set = [&](const wchar_t* sub, const std::wstring& value) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
            ERROR_SUCCESS) {
            RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
    };
    set(L"Software\\Classes\\.apk", L"WinApk.Package");
    set(L"Software\\Classes\\WinApk.Package", L"Android app");
    set(L"Software\\Classes\\WinApk.Package\\DefaultIcon", icon);
    set(L"Software\\Classes\\WinApk.Package\\shell\\open\\command", open);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// ---- running the runtime -----------------------------------------------------

// Starts winapk-runtime with `args`, its output in `log`, no console.
HANDLE start_runtime(const std::wstring& args, const fs::path& log) {
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring cmd = L"\"" + runtime_exe().wstring() + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   root().c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

DWORD wait_exit(HANDLE process) {
    WaitForSingleObject(process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    return code;
}

fs::path last_run_log() { return logs_dir() / L"last-run.log"; }

// Runs an installed app and waits; false (with the log) if it failed.
bool run_and_wait(const fs::path& apk) {
    HANDLE p = start_runtime(L"--apk \"" + apk.wstring() + L"\"", last_run_log());
    return p && wait_exit(p) == 0;
}

DWORD WINAPI run_thread(void* param) {
    auto* apk = static_cast<fs::path*>(param);
    if (!run_and_wait(*apk)) PostMessageW(g_main, kRunFailed, 0, reinterpret_cast<LPARAM>(new std::wstring(
                                                                       last_run_log().wstring())));
    delete apk;
    return 0;
}

void run_app(const fs::path& apk) {
    CloseHandle(CreateThread(nullptr, 0, run_thread, new fs::path(apk), 0, nullptr));
}

struct InstallJob {
    fs::path source;
    bool run_after;
};

DWORD WINAPI install_thread(void* param) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto* job = static_cast<InstallJob*>(param);
    std::error_code ec;
    fs::create_directories(library_dir(), ec);
    const std::wstring name = file_name_safe(job->source.stem().wstring());
    const fs::path target = library_dir() / (name + job->source.extension().wstring());
    bool ok = fs::equivalent(job->source, target, ec) ||
              CopyFileW(job->source.c_str(), target.c_str(), FALSE) != 0;
    if (ok) {
        HANDLE p = start_runtime(L"--apk \"" + target.wstring() + L"\" --install-only",
                                 logs_dir() / (L"install-" + name + L".log"));
        ok = p && wait_exit(p) == 0 &&
             MoveFileExW((app_dir(name) / L"label.txt").c_str(), label_file(name).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
    }
    if (ok) make_app_shortcuts({name, label_of(name), target}, true);
    if (ok && job->run_after) run_app(target);
    PostMessageW(g_main, kInstalled, ok, reinterpret_cast<LPARAM>(new std::wstring(name)));
    delete job;
    CoUninitialize();
    return 0;
}

void set_status(const std::wstring& text) { SendMessageW(g_status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str())); }

void refresh();

void install(const fs::path& source, bool run_after) {
    ++g_installing;
    set_status(L"Installing " + source.stem().wstring() + L"... The first install of an app takes up to a minute.");
    CloseHandle(CreateThread(nullptr, 0, install_thread, new InstallJob{source, run_after}, 0, nullptr));
    refresh();
}

// ---- the window --------------------------------------------------------------

std::vector<App> g_apps;

void refresh() {
    g_apps = library();
    ListView_DeleteAllItems(g_list);
    ImageList_RemoveAll(g_images);
    const int size = px(96);
    HBITMAP fallback = load_png(root() / L"bin" / L"winapk.png", size);
    for (size_t i = 0; i < g_apps.size(); ++i) {
        HBITMAP bmp = load_png(app_dir(g_apps[i].name) / L"icon.png", size);
        const int image = ImageList_Add(g_images, bmp ? bmp : fallback, nullptr);
        if (bmp) DeleteObject(bmp);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(g_apps[i].label.c_str());
        item.iImage = image;
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(g_list, &item);
    }
    if (fallback) DeleteObject(fallback);
    ShowWindow(g_empty, g_apps.empty() && !g_installing ? SW_SHOW : SW_HIDE);
    if (!g_installing) {
        const size_t n = g_apps.size();
        set_status(n == 0 ? L"No apps yet" : std::to_wstring(n) + (n == 1 ? L" app" : L" apps") +
                                                  L"  ·  double-click to open, right-click for more");
    }
}

const App* selected() {
    const int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    return i >= 0 && i < static_cast<int>(g_apps.size()) ? &g_apps[static_cast<size_t>(i)] : nullptr;
}

void remove_app(const App& app) {
    const std::wstring q = L"Remove " + app.label + L" from Win.apk?\n\nIts shortcuts go too. What the app saved stays.";
    if (MessageBoxW(g_main, q.c_str(), L"Win.apk", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    std::error_code ec;
    remove_app_shortcuts(app);
    fs::remove(app.apk, ec);
    fs::remove(label_file(app.name), ec);
    fs::remove(shortcut_icon(app.name), ec);
    fs::remove_all(app_dir(app.name), ec);
    fs::remove(data_dir() / L"android" / L"data" / L"app" / (L"." + app.name + L".installed"), ec);
    refresh();
}

void add_from_dialog() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC types[] = {{L"Android apps", L"*.apk;*.apkm;*.xapk;*.apks"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(L"Add an Android app");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST);
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlg->Show(g_main)) && SUCCEEDED(dlg->GetResults(&items))) {
        DWORD n = 0;
        items->GetCount(&n);
        for (DWORD i = 0; i < n; ++i) {
            IShellItem* item = nullptr;
            PWSTR path = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                install(path, false);
            }
            CoTaskMemFree(path);
            if (item) item->Release();
        }
        items->Release();
    }
    dlg->Release();
}

void layout() {
    RECT rc;
    GetClientRect(g_main, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sb;
    GetWindowRect(g_status, &sb);
    const int status_h = sb.bottom - sb.top;
    const int header = px(96);
    MoveWindow(g_add, rc.right - px(24) - px(140), (header - px(36)) / 2, px(140), px(36), TRUE);
    MoveWindow(g_list, 0, header, rc.right, rc.bottom - header - status_h, TRUE);
    MoveWindow(g_empty, 0, header + px(120), rc.right, px(80), TRUE);
    ListView_SetIconSpacing(g_list, px(150), px(150));
}

void paint_header(HDC dc) {
    RECT rc;
    GetClientRect(g_main, &rc);
    const int header = px(96);
    TRIVERTEX v[2] = {{0, 0, 0x1800, 0xC800, 0xA000, 0xFF00}, {rc.right, header, 0x1E00, 0x6400, 0xE600, 0xFF00}};
    GRADIENT_RECT gr{0, 1};
    GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
    DrawIconEx(dc, px(24), (header - px(56)) / 2, g_app_icon_large, px(56), px(56), 0, nullptr, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HGDIOBJ old = SelectObject(dc, g_title_font);
    RECT t{px(96), px(18), rc.right, px(56)};
    DrawTextW(dc, L"Win.apk", -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, g_sub_font);
    RECT s{px(98), px(56), rc.right, px(84)};
    DrawTextW(dc, L"Android apps on Windows  ·  preview", -1, &s, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

HFONT make_font(int pt, int weight) {
    NONCLIENTMETRICSW m{};
    m.cbSize = sizeof(m);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0);
    LOGFONTW lf = m.lfMessageFont;
    lf.lfHeight = -MulDiv(pt, g_dpi, 72);
    lf.lfWeight = weight;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

void make_fonts() {
    for (HFONT f : {g_font, g_title_font, g_sub_font, g_empty_font}) {
        if (f) DeleteObject(f);
    }
    g_font = make_font(10, FW_NORMAL);
    g_title_font = make_font(22, FW_SEMIBOLD);
    g_sub_font = make_font(11, FW_NORMAL);
    g_empty_font = make_font(13, FW_NORMAL);
}

void make_images() {
    if (g_images) ImageList_Destroy(g_images);
    g_images = ImageList_Create(px(96), px(96), ILC_COLOR32, 8, 8);
    ListView_SetImageList(g_list, g_images, LVSIL_NORMAL);
}

void apply_fonts() {
    SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(g_add, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(g_empty, WM_SETFONT, reinterpret_cast<WPARAM>(g_empty_font), TRUE);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_main = hwnd;
            g_dpi = static_cast<int>(GetDpiForWindow(hwnd));
            make_fonts();
            g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | LVS_ICON | LVS_AUTOARRANGE | LVS_SINGLESEL |
                                         LVS_SHOWSELALWAYS,
                                     0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kIdList), g_inst, nullptr);
            SetWindowTheme(g_list, L"Explorer", nullptr);
            ListView_SetExtendedListViewStyle(g_list, LVS_EX_DOUBLEBUFFER);
            g_add = CreateWindowExW(0, L"BUTTON", L"Add app...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                                    hwnd, reinterpret_cast<HMENU>(kIdAdd), g_inst, nullptr);
            g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                       reinterpret_cast<HMENU>(kIdStatus), g_inst, nullptr);
            g_empty = CreateWindowExW(0, L"STATIC",
                                      L"Drag an Android app (.apk) here, or click “Add app”.",
                                      WS_CHILD | SS_CENTER, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kIdEmpty), g_inst,
                                      nullptr);
            apply_fonts();
            make_images();
            DragAcceptFiles(hwnd, TRUE);
            layout();
            refresh();
            return 0;
        }
        case WM_SIZE:
            layout();
            return 0;
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            make_fonts();
            apply_fonts();
            make_images();
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            refresh();
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            paint_header(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lp) == g_empty) {
                SetTextColor(reinterpret_cast<HDC>(wp), RGB(110, 110, 110));
                SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdAdd: add_from_dialog(); return 0;
                case kCmdOpen: if (const App* a = selected()) run_app(a->apk); return 0;
                case kCmdShortcut: if (const App* a = selected()) make_app_shortcuts(*a, true); return 0;
                case kCmdLog: {
                    const fs::path log = last_run_log();
                    ShellExecuteW(hwnd, L"open", L"notepad.exe", log.c_str(), nullptr, SW_SHOWNORMAL);
                    return 0;
                }
                case kCmdRemove: if (const App* a = selected()) remove_app(*a); return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* n = reinterpret_cast<NMHDR*>(lp);
            if (n->idFrom == kIdList && n->code == LVN_ITEMACTIVATE) {
                if (const App* a = selected()) {
                    set_status(L"Opening " + a->label + L"...");
                    run_app(a->apk);
                }
                return 0;
            }
            if (n->idFrom == kIdList && n->code == NM_RCLICK && selected()) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, kCmdOpen, L"&Open");
                AppendMenuW(menu, MF_STRING, kCmdShortcut, L"Create &desktop shortcut");
                AppendMenuW(menu, MF_STRING, kCmdLog, L"Show the last &log");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, kCmdRemove, L"&Remove");
                SetMenuDefaultItem(menu, kCmdOpen, FALSE);
                POINT pt;
                GetCursorPos(&pt);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                return 0;
            }
            if (n->idFrom == kIdList && n->code == LVN_KEYDOWN &&
                reinterpret_cast<NMLVKEYDOWN*>(lp)->wVKey == VK_DELETE) {
                if (const App* a = selected()) remove_app(*a);
                return 0;
            }
            break;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH * 4];
                if (DragQueryFileW(drop, i, path, MAX_PATH * 4) && is_app_file(path)) install(path, false);
            }
            DragFinish(drop);
            return 0;
        }
        case kInstalled: {
            std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
            --g_installing;
            refresh();
            if (wp) {
                set_status(label_of(*name) + L" is installed: it is in the Start menu and on the desktop.");
            } else {
                const std::wstring log = (logs_dir() / (L"install-" + *name + L".log")).wstring();
                const std::wstring text = L"Installing " + *name + L" failed.\n\nThe log is in:\n" + log;
                MessageBoxW(hwnd, text.c_str(), L"Win.apk", MB_OK | MB_ICONWARNING);
                set_status(L"Installing " + *name + L" failed");
            }
            return 0;
        }
        case kRunFailed: {
            std::unique_ptr<std::wstring> log(reinterpret_cast<std::wstring*>(lp));
            const std::wstring text = L"The app stopped with an error.\n\nThe log is in:\n" + *log;
            MessageBoxW(hwnd, text.c_str(), L"Win.apk", MB_OK | MB_ICONWARNING);
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {px(520), px(380)};
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // A shortcut: run the app, and only show anything if it fails.
    if (argc >= 3 && wcscmp(argv[1], L"--run") == 0) {
        if (!run_and_wait(argv[2])) {
            const std::wstring text = L"The app stopped with an error.\n\nThe log is in:\n" + last_run_log().wstring();
            MessageBoxW(nullptr, text.c_str(), L"Win.apk", MB_OK | MB_ICONWARNING);
            return 1;
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    integrate_with_windows();

    g_app_icon_large = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, 0));
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"WinApkLibrary";
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Win.apk", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(960, static_cast<int>(dpi), 96), MulDiv(640, static_cast<int>(dpi), 96),
                                nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, show);

    // An .apk opened from Explorer, or dropped on the program: install and run.
    for (int i = 1; i < argc; ++i) {
        if (is_app_file(argv[i])) install(argv[i], true);
    }
    LocalFree(argv);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
