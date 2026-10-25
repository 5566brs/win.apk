package com.macapk.loader;

/**
 * Android's System.mapLibraryName, System.load and Runtime.load for app code
 * on the Windows host. ClassFileRewriter Pattern N points the app's call sites
 * here; nothing else calls this class.
 *
 * mapLibraryName gives "lib" + name + ".so", as libcore's System does, where
 * the JVM gives "name.dll". Loaders that build a file name out of it --
 * SoLoader, ReLinker, Chromium's linker -- look for the app's ELF under its
 * Android name.
 *
 * load(path) is AppClassLoader.loadNativeOnDemand on the CALLER's loader: an
 * ELF goes through a stub DLL made for it on the spot (see the comment there),
 * and the load runs in that loader, so the library's natives bind to the
 * app's classes as System.load binds them on Android (libcore Runtime.load0
 * passes the caller's class loader to nativeLoad). A call made here directly
 * would register the library with this class's loader instead, which HotSpot
 * never searches for an app class's native methods.
 *
 * Mac.apk PR #97 (kksimp/Mac.apk, by dizzzan) routed the same two calls
 * through a bridge for unpacked arm64 libraries on macOS; this follows its
 * approach for the caller's loader.
 */
public final class NativeLoadBridge {
    private static final StackWalker WALKER =
        StackWalker.getInstance(StackWalker.Option.RETAIN_CLASS_REFERENCE);

    private NativeLoadBridge() {}

    /** libcore System.mapLibraryName. */
    public static String mapLibraryName(String libname) {
        if (libname == null) throw new NullPointerException("libname == null");
        return "lib" + libname + ".so";
    }

    /** Stands for {@code System.load(filename)}. */
    public static void load(String filename) {
        loadFor(WALKER.getCallerClass(), filename);
    }

    /** Stands for {@code runtime.load(filename)}. */
    public static void load(Runtime runtime, String filename) {
        java.util.Objects.requireNonNull(runtime);
        loadFor(WALKER.getCallerClass(), filename);
    }

    private static void loadFor(Class<?> caller, String filename) {
        ClassLoader loader = caller.getClassLoader();
        if (loader instanceof AppClassLoader) {
            ((AppClassLoader) loader).loadNativeOnDemand(filename);
        } else {
            // Only AppClassLoader rewrites classes, so this is not reached by
            // app code; keep the JDK's behaviour for anything else.
            Runtime.getRuntime().load(filename);
        }
    }
}
