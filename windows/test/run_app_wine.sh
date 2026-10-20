#!/usr/bin/env bash
# run_app_wine.sh -- stage the runtime on Linux and boot an APK headless under
# Wine, the local counterpart of CI's "Boot an APK headless" step.
#
#   WINAPK_WIN_JDK=/path/to/windows-jdk windows/test/run_app_wine.sh app.apk [frames]
#
# Stops after <frames> presented frames (default 30) and leaves the last one in
# windows/build/frame.bmp and the log in windows/build/boot.log.
#
# Wine 9 lacks a function the JDK's NetworkInterface code delay-loads, and
# the JDK's default SecureRandom seeds itself from it, so the JVM dies at the
# first temp file. Real Windows is not affected. Here the Windows provider
# (SunMSCAPI) is put first, so new SecureRandom() never takes that path.

set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
WIN_DIR="$(cd -- "$HERE/.." >/dev/null 2>&1 && pwd)"
BUILD="${WINAPK_BUILD:-$WIN_DIR/build}"
JDK="${WINAPK_WIN_JDK:?set WINAPK_WIN_JDK to a Windows JDK dir}"
APK="${1:?usage: run_app_wine.sh <app.apk> [frames]}"
FRAMES="${2:-30}"
WINE="${WINE:-$(command -v wine64 || command -v wine)}"

cmake -S "$WIN_DIR" -B "$BUILD/host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$WIN_DIR/cmake/clang-mingw-x86_64.cmake" \
    -DWINAPK_JNI_INCLUDE="$JDK/include" >/dev/null
cmake --build "$BUILD/host" --target winapk-runtime
"$WIN_DIR/scripts/stage_runtime.sh" "$BUILD/host" "$BUILD/runtime"

printf 'security.provider.1=SunMSCAPI\nsecurity.provider.2=SUN\n' > "$BUILD/wine.security"
winpath() { "$WINE" winepath -w "$1" 2>/dev/null; }

MACAPK_JVM_OPTS="-Djava.security.properties=$(winpath "$BUILD/wine.security")" \
WINAPK_HEADLESS=1 WINAPK_HEADLESS_EXIT_AFTER_FRAMES="$FRAMES" \
WINAPK_HEADLESS_FRAME="$(winpath "$BUILD/frame.bmp")" \
WINEDEBUG=-all "$WINE" "$BUILD/runtime/bin/winapk-runtime.exe" \
    --apk "$(winpath "$(realpath "$APK")")" --jdk "$(winpath "$JDK")" --data "$(winpath "$BUILD/appdata")" \
    > "$BUILD/boot.log" 2>&1 || { tail -n 100 "$BUILD/boot.log"; exit 1; }
grep -E "first frame presented|frames presented" "$BUILD/boot.log"
echo "[run_app_wine] log: $BUILD/boot.log, last frame: $BUILD/frame.bmp"
