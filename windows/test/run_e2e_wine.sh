#!/usr/bin/env bash
# run_e2e_wine.sh -- build and run the real-JVM end-to-end test under Wine.
#
# Needs: clang, lld, cmake, ninja, wine64, and a Windows JDK (for jvm.dll and
# jni.h/jvmti.h). Point WINAPK_WIN_JDK at the Windows JDK; WINAPK_HOST_JAVAC at
# a javac for building the test class (any JDK 21+).
#
#   WINAPK_WIN_JDK=/path/to/windows-jdk windows/test/run_e2e_wine.sh

set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
WIN_DIR="$(cd -- "$HERE/.." >/dev/null 2>&1 && pwd)"
BUILD="${WINAPK_BUILD:-$WIN_DIR/build}"
JDK="${WINAPK_WIN_JDK:?set WINAPK_WIN_JDK to a Windows JDK dir}"
JAVAC="${WINAPK_HOST_JAVAC:-javac}"
WINE="${WINE:-$(command -v wine64 || echo /usr/lib/wine/wine64)}"

# 1. Guest .so + Java class.
"$HERE/build_e2e.sh" "$JDK/include" "$JAVAC" "$BUILD/e2e"

# 2. The harness.
cmake -S "$WIN_DIR" -B "$BUILD/host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$WIN_DIR/cmake/clang-mingw-x86_64.cmake" \
    -DWINAPK_JNI_INCLUDE="$JDK/include" >/dev/null
cmake --build "$BUILD/host" --target winapk-jvm-run

# 3. Run it, with the JDK on the Windows PATH so jvm.dll and its deps resolve.
WINEPATH="$JDK/bin;$JDK/bin/server" WINEDEBUG=-all "$WINE" "$BUILD/host/winapk-jvm-run.exe" \
    "$BUILD/e2e/libwinapk_jni_guest.so" "$BUILD/e2e/classes"
