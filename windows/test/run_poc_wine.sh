#!/usr/bin/env bash
# run_poc_wine.sh -- build the phase-1 PoC on Linux and run it under Wine.
#
# Needs: clang, lld, cmake, ninja, mingw-w64 (headers/CRT), wine64.
#   Ubuntu: apt install clang lld cmake ninja-build mingw-w64 wine64
#
# Wine is a convenience, not the verdict: the CI job on a real Windows runner
# (.github/workflows/windows-poc.yml) is what counts.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
WIN_DIR="$(cd -- "$HERE/.." >/dev/null 2>&1 && pwd)"
BUILD="${WINAPK_BUILD:-$WIN_DIR/build}"

"$HERE/build_guest.sh" "$BUILD/guest"

cmake -S "$WIN_DIR" -B "$BUILD/host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$WIN_DIR/cmake/clang-mingw-x86_64.cmake" >/dev/null
cmake --build "$BUILD/host"

WINE="${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}"
WINEDEBUG=-all "$WINE" "$BUILD/host/winapk-elfrun.exe" \
    "$BUILD/guest/libwinapk_poc_guest.so" \
    "$BUILD/guest/libwinapk_poc_guest_packed.so"
WINEDEBUG=-all "$WINE" "$BUILD/host/winapk-linker-test.exe" "$BUILD/guest/link"
