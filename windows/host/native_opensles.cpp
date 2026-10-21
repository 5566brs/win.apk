// native_opensles.cpp -- libOpenSLES for an app's native code, until it is
// ported.
//
// Upstream implements OpenSL ES on its audio mixer (bionic_shim.cpp, "REAL
// OpenSL ES output"). Until that is ported onto audio_engine.h, the engine
// cannot be created, and says so: a missing function's stub returns 0, which
// for slCreateEngine is SL_RESULT_SUCCESS with no engine, and SDL then waits
// on audio that never starts. With an error, engines fall back to silence.

#include "native_opensles.h"

#include <cstdint>
#include <cstring>

#include "winapk/guest_abi.h"

namespace winapk::host {

namespace {

constexpr uint32_t kSlResultFeatureUnsupported = 12;

WINAPK_GUEST_ABI uint32_t slCreateEngine(void** engine, uint32_t, const void*, uint32_t, const void*, const void*) {
    if (engine) *engine = nullptr;
    return kSlResultFeatureUnsupported;
}

}  // namespace

void* native_opensles_resolve(const char* name) {
    return std::strcmp(name, "slCreateEngine") == 0 ? reinterpret_cast<void*>(&slCreateEngine) : nullptr;
}

}  // namespace winapk::host
