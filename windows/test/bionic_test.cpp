// bionic_test.cpp -- winapk-bionic-test: the Bionic shim (src/bionic/) from the
// guest's side (test/guest/bionic_guest.c).
//
//   winapk-bionic-test <libwinapk_bionic_guest.so> <scratch dir>
//
// The guest library is loaded through the guest linker with the shim as its
// libc, so every call is a real System V call into the shim with Bionic's
// layouts. An import the shim does not provide fails the load and is listed.
//
// Exit code: the number of failed checks.

#include "winapk/guest_abi.h"
#include "winapk/linker.h"
#include "winapk/mini_bionic.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: winapk-bionic-test <libwinapk_bionic_guest.so> <scratch dir>\n");
        return 2;
    }
    winapk::LinkerConfig config;
    config.host = winapk::mini_bionic_resolve;
    winapk::GuestLinker linker(config);
    std::string error;
    void* lib = linker.open(argv[1], winapk::GuestLinker::kRtldNow, &error);
    if (!lib) {
        std::printf("FAIL: cannot load the guest: %s\n", error.c_str());
        return 1;
    }
    using TestFn = int(WINAPK_GUEST_ABI*)(const char*);
    auto test = reinterpret_cast<TestFn>(linker.lookup(lib, "bionic_test", nullptr, &error));
    if (!test) {
        std::printf("FAIL: the guest has no bionic_test\n");
        return 1;
    }
    // Guest "/x" paths resolve under the scratch directory, as under the
    // android root in the runtime.
    winapk::set_android_root(argv[2]);
    const int failures = test(argv[2]);
    std::fflush(stdout);
    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures;
}
