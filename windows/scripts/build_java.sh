#!/usr/bin/env bash
# build_java.sh -- builds upstream's Java side (runtime/java and friends) with a
# stock JDK 21, on Windows (MSYS2) or Linux. The output is what the Windows host
# puts on the JVM's classpath:
#
#   <out>/lib/android-framework.jar   the Android framework reimplementation
#   <out>/lib/macapk-loader.jar       boot code and the DEX-to-JVM converter
#   <out>/lib/bcprov-jdk18on-1.79.jar the "BC" security provider Android has
#   <out>/lib/winapk-tools.jar        Windows-side helpers (windows/java)
#   <out>/lib/java_base_patch/        classes patched into java.base
#   <out>/lib/jdk_unsupported_patch/  classes patched into jdk.unsupported
#
#   windows/scripts/build_java.sh [out]       (default: windows/build/runtime)
#
# This mirrors the Java half of runtime/scripts/build.sh, which also builds the
# Mac native runtime and so only runs on macOS. Left out on purpose: the pairip
# reference files and the platform resource APK (both need macOS tooling; the
# resource APK is a later step). Keep the two in sync when upstream changes
# its Java build.
#
# JAVA_HOME selects the JDK (21); otherwise javac and jar come from PATH.

set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
RT="$(cd -- "$HERE/../../runtime" >/dev/null 2>&1 && pwd)"
OUT="${1:-$HERE/../build/runtime}"
mkdir -p "$OUT"
OUT="$(cd -- "$OUT" && pwd)"  # absolute: the packaging steps cd elsewhere
LIB="$OUT/lib"
WORK="$OUT/.build"  # scratch: classes, file lists, javac log
TMP="$WORK/classes"

if [[ -n "${JAVA_HOME:-}" ]]; then
    JAVAC="$JAVA_HOME/bin/javac"
    JAR="$JAVA_HOME/bin/jar"
else
    JAVAC=javac
    JAR=jar
fi

# javac reads the @file lists itself, so on MSYS2, where only command-line
# arguments are translated to Windows paths, the lists need Windows paths.
native_paths() {
    if command -v cygpath > /dev/null 2>&1; then cygpath -m -f -; else cat; fi
}
# The same for a path inside an argument ("module=path"), which MSYS2 does not
# always recognize.
native_path() { printf '%s\n' "$1" | native_paths; }

# A stale class from a deleted source would otherwise ship forever; javac
# recompiles everything anyway, so wiping costs nothing.
rm -rf "$TMP"
mkdir -p "$TMP" "$LIB"

echo "[build_java] compiling runtime/java..."
LIST="$WORK/java.list"
# Paths go into an @file, which splits on spaces; upstream skips Finder
# conflict copies ("Foo 2.java") the same way.
find "$RT/java" -name '*.java' ! -path '* [2-9].java' | sort | native_paths > "$LIST"
# Upstream's sources produce a few dozen warnings (sun.misc.Unsafe use) that
# -nowarn does not silence, so the output goes to a log shown only on failure.
if ! "$JAVAC" -J-Xmx2g -d "$TMP" --release 21 -nowarn -Xlint:-options @"$LIST" > "$WORK/javac.log" 2>&1; then
    cat "$WORK/javac.log" >&2
    echo "[build_java] javac failed" >&2
    exit 1
fi

# The resolver provider is a separate pass: it needs the running JDK's API
# rather than --release 21.
if [[ -d "$RT/net_spi" ]]; then
    find "$RT/net_spi" -name '*.java' ! -path '* [2-9].java' | sort | native_paths > "$WORK/net_spi.list"
    if [[ -s "$WORK/net_spi.list" ]]; then
        "$JAVAC" -d "$TMP" -nowarn -Xlint:-options @"$WORK/net_spi.list"
    fi
    if [[ -d "$RT/net_spi/META-INF/services" ]]; then
        mkdir -p "$TMP/META-INF/services"
        cp -f "$RT"/net_spi/META-INF/services/* "$TMP/META-INF/services/"
    fi
fi

# Classes the JDK only accepts from inside its own modules.
patch_module() {  # patch_module <module> <source dir> <out dir>
    local module="$1" src="$2" dst="$3"
    [[ -d "$src" ]] || return 0
    echo "[build_java] compiling ${src#"$RT"/} (--patch-module $module)..."
    rm -rf "$dst"
    mkdir -p "$dst"
    find "$src" -name '*.java' | sort | native_paths > "$WORK/$module.list"
    "$JAVAC" -nowarn --patch-module "$module=$(native_path "$src")" -d "$dst" @"$WORK/$module.list"
}
patch_module java.base "$RT/java_base_patch" "$LIB/java_base_patch"
patch_module jdk.unsupported "$RT/jdk_unsupported_patch" "$LIB/jdk_unsupported_patch"

# Shipped key-bind profiles are classpath resources of the loader jar.
if [[ -d "$RT/java/com/macapk/loader/keybinds" ]]; then
    mkdir -p "$TMP/com/macapk/loader/keybinds"
    cp -f "$RT"/java/com/macapk/loader/keybinds/* "$TMP/com/macapk/loader/keybinds/"
fi

echo "[build_java] packaging..."
( cd "$TMP" && find . -type f -name '*.class' ! -path './com/macapk/*' | sort > "$WORK/framework.list" )
( cd "$TMP" && "$JAR" --create --file "$LIB/android-framework.jar" @"$WORK/framework.list" )
if compgen -G "$RT/fonts/*.ttf" > /dev/null; then
    mkdir -p "$TMP/fonts"
    cp -f "$RT"/fonts/*.ttf "$TMP/fonts/"
    ( cd "$TMP" && "$JAR" --update --file "$LIB/android-framework.jar" fonts )
fi
( cd "$TMP" && "$JAR" --create --file "$LIB/macapk-loader.jar" com/macapk )
if [[ -d "$TMP/META-INF/services" ]]; then
    ( cd "$TMP" && "$JAR" --update --file "$LIB/macapk-loader.jar" META-INF/services )
fi
cp -f "$RT/thirdparty/bcprov-jdk18on-1.79.jar" "$LIB/"

# Our own helpers, kept out of upstream's jars.
echo "[build_java] compiling windows/java..."
TOOLS="$WORK/tools"
rm -rf "$TOOLS"
mkdir -p "$TOOLS"
find "$HERE/../java" -name '*.java' | sort | native_paths > "$WORK/tools.list"
"$JAVAC" -d "$TOOLS" --release 21 @"$WORK/tools.list"
( cd "$TOOLS" && "$JAR" --create --file "$LIB/winapk-tools.jar" . )

echo "[build_java] $LIB/android-framework.jar"
echo "[build_java] $LIB/macapk-loader.jar"
echo "[build_java] $LIB/winapk-tools.jar"
