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
#
# And, in link/, the libraries for winapk-linker-test (see guest/link_guest.c).

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

# The linker test's dependency tree. Stub system libraries give the DT_NEEDED
# entries an NDK build would have; they are only link inputs and never ship.
LINK="$OUT/link"
STUBS="$OUT/link-stubs"
mkdir -p "$LINK" "$STUBS"
for lib in libc libdl liblog; do
    "$CC" "${COMMON[@]}" -Wl,-soname,$lib.so -o "$STUBS/$lib.so" -x c /dev/null
done
link_lib() {  # link_lib <part> <name> [linker args...]
    local part="$1" name="$2"
    shift 2
    "$CC" "${COMMON[@]}" -DWINAPK_LINK_$part -Wl,-soname,$name.so -Wl,--no-as-needed \
        -o "$LINK/$name.so" "$HERE/guest/link_guest.c" "$@"
}
# base and mid need each other: link base against a placeholder that only
# carries mid's soname, then link the real mid against base.
"$CC" "${COMMON[@]}" -Wl,-soname,libwinapk_link_mid.so -o "$STUBS/libwinapk_link_mid.so" -x c /dev/null
link_lib PLUGIN libwinapk_link_plugin
link_lib BASE libwinapk_link_base -L"$STUBS" -lwinapk_link_mid
link_lib MID libwinapk_link_mid -L"$LINK" -L"$STUBS" -lwinapk_link_base -llog
link_lib ROOT libwinapk_link_root -L"$LINK" -L"$STUBS" -lwinapk_link_mid -lc -ldl

echo "[build_guest] $OUT/libwinapk_poc_guest.so"
echo "[build_guest] $OUT/libwinapk_poc_guest_packed.so"
echo "[build_guest] $LINK/libwinapk_link_{root,mid,base,plugin}.so"
