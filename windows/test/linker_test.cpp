// linker_test.cpp -- winapk-linker-test: the guest linker against a real
// dependency tree of Android x86_64 libraries (test/guest/link_guest.c).
//
//   winapk-linker-test <dir with libwinapk_link_*.so>
//
// The tree is root -> mid -> base -> mid (a cycle), with libc, libdl and
// liblog as system dependencies, and a plugin that root dlopens at runtime.
// The checks cover DT_NEEDED loading, constructor order, symbol lookup through
// the local group, the guest's libdl, and the image registry.
//
// Exit code: the number of failed checks.

#include "winapk/guest_abi.h"
#include "winapk/linker.h"
#include "winapk/mini_bionic.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ",
                detail.c_str());
    if (!ok) ++g_failures;
}

void expect_eq(const char* what, int got, int want) {
    std::string detail = "got " + std::to_string(got);
    if (got != want) detail += ", want " + std::to_string(want);
    check(what, got == want, detail);
}

using IntFn = int(WINAPK_GUEST_ABI*)();

int call(winapk::GuestLinker& linker, void* handle, const char* name) {
    std::string error;
    auto fn = reinterpret_cast<IntFn>(linker.lookup(handle, name, nullptr, &error));
    if (!fn) {
        check(name, false, error);
        return -1000;
    }
    return fn();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: winapk-linker-test <dir with libwinapk_link_*.so>\n");
        return 2;
    }

    std::printf("Win.apk guest linker\n");
    winapk::LinkerConfig config;
    config.search_paths.push_back(std::filesystem::u8path(argv[1]));
    config.host = winapk::mini_bionic_resolve;
    winapk::GuestLinker linker(config);
    check("the linker is the process linker", winapk::GuestLinker::process() == &linker);

    std::string error;
    void* root = linker.open("libwinapk_link_root.so", winapk::GuestLinker::kRtldNow, &error);
    check("open root with its DT_NEEDED tree", root != nullptr, error);
    if (!root) return g_failures;
    check("root, mid and base are loaded (system libraries are not files)",
          linker.images().size() == 3, std::to_string(linker.images().size()) + " image(s)");

    expect_eq("constructors ran dependencies first", call(linker, root, "link_root_init_order"), 42);
    expect_eq("symbol bound through the local group, across the cycle",
              call(linker, root, "link_root_indirect"), 10);

    void* from_base = linker.lookup(root, "Java_WinApkLink_fromBase", nullptr, &error);
    check("dlsym on root finds a dependency's Java_* symbol", from_base != nullptr, error);
    const winapk::ElfImage* base_image = linker.image_containing(from_base);
    check("the registry maps that address to base",
          base_image && base_image->soname() == "libwinapk_link_base.so");
    check("the registry ignores host addresses",
          linker.image_containing(reinterpret_cast<void*>(&winapk::mini_bionic_resolve)) == nullptr);
    check("dlsym on root reaches libc through its system dependency",
          linker.lookup(root, "malloc", nullptr, &error) == winapk::mini_bionic_resolve("malloc"));

    check("opening root again returns the same handle",
          linker.open("libwinapk_link_root.so", 0, &error) == root &&
              linker.open("libwinapk_link_mid.so", 0, &error) != nullptr &&
              linker.images().size() == 3);

    check("RTLD_NOLOAD does not load the plugin",
          linker.open("libwinapk_link_plugin.so", winapk::GuestLinker::kRtldNoLoad, &error) == nullptr);
    expect_eq("guest dlopen + dlsym", call(linker, root, "link_root_dlopen"), 81);
    check("the plugin is loaded now",
          linker.open("libwinapk_link_plugin.so", winapk::GuestLinker::kRtldNoLoad, &error) != nullptr);
    expect_eq("guest dlerror after a failed dlopen", call(linker, root, "link_root_dlerror"), 1);
    expect_eq("guest dladdr", call(linker, root, "link_root_dladdr"), 1);
    expect_eq("guest dlsym(RTLD_DEFAULT)", call(linker, root, "link_root_rtld_default"), 1);

    void* liblog = linker.open("liblog.so", 0, &error);
    check("a system library opens as a host-backed handle",
          liblog && linker.lookup(liblog, "__android_log_write", nullptr, &error) != nullptr);

    error.clear();
    check("a missing library fails with a bionic-style message",
          linker.open("libwinapk_missing.so", 0, &error) == nullptr &&
              error.find("not found") != std::string::npos,
          error);

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
