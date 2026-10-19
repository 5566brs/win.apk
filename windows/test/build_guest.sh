#!/usr/bin/env bash
# build_guest.sh -- builds the PoC Android x86_64 libraries with plain clang + lld.
#
# No NDK needed: the guest declares its libc imports by hand and links with
# -nostdlib, so its undefined symbols are left for the Win.apk loader.
#
#   windows/test/build_guest.sh [out_dir]      (default: windows/build/guest)
#
# Two variants of the same source:
#   libwinapk_poc_guest.so         plain RELA relocations
#   libwinapk_poc_guest_packed.so  Android packed relocations (APS2 + RELR),
#                                  what NDK builds for recent minSdk produce

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
OUT="${1:-$HERE/../build/guest}"
CC="${CC_ANDROID:-clang}"
mkdir -p "$OUT"

COMMON=(
    --target=x86_64-linux-android24
    -fPIC -O2 -fstack-protector-all -ffreestanding
    -nostdlib -shared -fuse-ld=lld
    -fvisibility=hidden
    -Wl,-z,relro -Wl,-z,now
)

"$CC" "${COMMON[@]}" -Wl,-soname,libwinapk_poc_guest.so \
    -o "$OUT/libwinapk_poc_guest.so" "$HERE/guest/poc_guest.c"

"$CC" "${COMMON[@]}" -Wl,-soname,libwinapk_poc_guest_packed.so \
    -Wl,--pack-dyn-relocs=android+relr -Wl,--use-android-relr-tags \
    -o "$OUT/libwinapk_poc_guest_packed.so" "$HERE/guest/poc_guest.c"

echo "[build_guest] $OUT/libwinapk_poc_guest.so"
echo "[build_guest] $OUT/libwinapk_poc_guest_packed.so"
