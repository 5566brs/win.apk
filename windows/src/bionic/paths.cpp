// paths.cpp -- where a guest path lives on the host.
//
// Guest code names files two ways: with the host paths Java hands it
// (getFilesDir(), nativeLibraryDir: "C:\...\android\data\...") and with
// Android's own absolute paths, hard-coded or built by hand ("/data/data/<pkg>/
// files", "/sdcard/...", "/system/fonts/Roboto-Regular.ttf"). On Windows a path
// that starts with '/' means the root of the current drive, which is never what
// the guest means. Upstream's shim (macapk_arm64_remap_path in
// runtime/translator/src/bionic_shim.cpp) solves the same problem the same way:
//
//   /system/etc/fonts.xml, /system/fonts/...  -> the fonts directory Boot wrote
//                                                (setSystemFontsDir)
//   /dev/null                                 -> NUL
//   any other /...                            -> <android root>/...
//
// The android root is the per-user folder the runtime mirrors Android's
// layout in (data/app, data/data/<pkg>, sdcard); the runtime sets it before
// any guest code runs.

#include "bionic_internal.h"

#include <cstring>
#include <mutex>
#include <string>

#include <windows.h>

#include "winapk/mini_bionic.h"

namespace winapk::bionic {

namespace {

std::mutex g_lock;
std::wstring g_android_root;  // no trailing separator
std::wstring g_fonts_dir;

std::wstring to_wide(const char* utf8, size_t len) {
    if (len == 0) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(len), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(len), &w[0], n);
    return w;
}

std::wstring trimmed(std::wstring w) {
    while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    return w;
}

bool starts_with(const char* s, const char* prefix) { return std::strncmp(s, prefix, std::strlen(prefix)) == 0; }

}  // namespace

std::wstring host_path(const char* p) {
    if (!p) return {};
    const size_t len = std::strlen(p);
    // A host path (drive letter, UNC, relative): as it is.
    if (p[0] != '/' || p[1] == '/') return to_wide(p, len);
    if (std::strcmp(p, "/dev/null") == 0) return L"NUL";

    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_fonts_dir.empty()) {
        if (std::strcmp(p, "/system/etc/fonts.xml") == 0) return g_fonts_dir + L"\\fonts.xml";
        if (starts_with(p, "/system/fonts") && (p[13] == '\0' || p[13] == '/')) {
            return g_fonts_dir + to_wide(p + 13, len - 13);
        }
    }
    if (g_android_root.empty()) return to_wide(p, len);
    return g_android_root + to_wide(p, len);
}

}  // namespace winapk::bionic

namespace winapk {

void set_android_root(const char* dir) {
    std::lock_guard<std::mutex> hold(bionic::g_lock);
    bionic::g_android_root = bionic::trimmed(bionic::to_wide(dir, dir ? std::strlen(dir) : 0));
}
void set_system_fonts_dir(const char* dir) {
    std::lock_guard<std::mutex> hold(bionic::g_lock);
    bionic::g_fonts_dir = bionic::trimmed(bionic::to_wide(dir, dir ? std::strlen(dir) : 0));
}

}  // namespace winapk
