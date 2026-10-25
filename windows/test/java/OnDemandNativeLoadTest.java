// OnDemandNativeLoadTest -- checks that app code on a Windows host loads ELF
// libraries the way it does on Android (ClassFileRewriter Pattern N,
// NativeLoadBridge, AppClassLoader.loadNativeOnDemand):
//
//   * System.mapLibraryName("foo") is "libfoo.so" in guest code, so a
//     ReLinker-style lookup finds lib/<abi>/libfoo.so in the APK;
//   * System.load / Runtime.load of an ELF path gets a stub made on the spot,
//     one per path, and the library binds to the GUEST's class loader: its
//     native method resolves, and the JDK refuses the same library for the
//     test's own loader;
//   * a stub the startup exposure made is reused, a same-named ELF elsewhere
//     gets a stub of its own, and a non-ELF path is loaded as given.
//
// A plain JVM has no guest linker, so the stub here is OnDemandStubFixture.c:
// a host JNI library the real translator copies as it copies
// winapk-guest-stub.dll. The ELF is only named, never mapped; a real run
// (WhatsApp's first launch) covers the guest linker.
//
//   clang -shared -O2 -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/win32" \
//         -o out/stub/winapk-guest-stub.dll windows/test/java/OnDemandStubFixture.c
//   cp <runtime>/bin/macapk-translator.exe out/stub/
//   javac -cp "<runtime>/lib/macapk-loader.jar;<runtime>/lib/android-framework.jar" \
//         -d out windows/test/java/OnDemandNativeLoadTest.java
//   java  -cp "<runtime>/lib/macapk-loader.jar;<runtime>/lib/android-framework.jar;out" \
//         OnDemandNativeLoadTest out/stub/macapk-translator.exe [off]
//
// "off": the run has MACAPK_ONDEMAND_NATIVE=0 set, and guest code must then
// see the JDK's behaviour. Needs a JDK (it compiles the guest class). Exits 0
// when every check passes.

import com.macapk.loader.AppClassLoader;
import com.macapk.loader.ClassFileRewriter;

import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import javax.tools.ToolProvider;

public final class OnDemandNativeLoadTest {

    /** Stands for app code; compiled at run time into a directory only the
     *  AppClassLoader sees, so the loader defines (and rewrites) it. */
    static final String GUEST = """
        import java.io.File;
        import java.io.InputStream;
        import java.nio.file.Files;
        import java.nio.file.StandardCopyOption;
        import java.util.zip.ZipEntry;
        import java.util.zip.ZipFile;

        public class OnDemandGuest {
            public static native int ping();
            public static String mapped(String name) { return System.mapLibraryName(name); }
            public static void systemLoad(String path) { System.load(path); }
            public static void runtimeLoad(String path) { Runtime.getRuntime().load(path); }
            // ReLinker's ApkLibraryInstaller, cut down: the APK entry under the
            // mapped name, copied out of the APK and System.load()ed.
            public static String relink(String apk, String abi, String name, String dir) throws Exception {
                String lib = System.mapLibraryName(name);
                try (ZipFile zip = new ZipFile(apk)) {
                    ZipEntry e = zip.getEntry("lib/" + abi + "/" + lib);
                    if (e == null) return "no lib/" + abi + "/" + lib;
                    File out = new File(dir, lib);
                    try (InputStream in = zip.getInputStream(e)) {
                        Files.copy(in, out.toPath(), StandardCopyOption.REPLACE_EXISTING);
                    }
                    System.load(out.getAbsolutePath());
                    return "loaded " + out.getName();
                }
            }
        }
        """;

    static int failures = 0;

    static void check(String what, Object got, Object want) {
        boolean ok = want.equals(got);
        System.out.println((ok ? "ok   " : "FAIL ") + what + ": " + got + (ok ? "" : " (want " + want + ")"));
        if (!ok) failures++;
    }

    static Object call(Class<?> c, String name, Object... args) throws Throwable {
        Class<?>[] types = new Class<?>[args.length];
        java.util.Arrays.fill(types, String.class);
        Method m = c.getMethod(name, types);
        try {
            return m.invoke(null, args);
        } catch (InvocationTargetException e) {
            throw e.getCause();
        }
    }

    /** The exception `name(args)` throws, as "SimpleName: message", or "none". */
    static String thrown(Class<?> c, String name, Object... args) {
        try {
            call(c, name, args);
            return "none";
        } catch (Throwable t) {
            return t.getClass().getSimpleName() + ": " + t.getMessage();
        }
    }

    /** What `name(args)` returns, or the exception it throws, as thrown() gives it. */
    static Object result(Class<?> c, String name, Object... args) {
        try {
            return call(c, name, args);
        } catch (Throwable t) {
            return t.getClass().getSimpleName() + ": " + t.getMessage();
        }
    }

    static int loads() { return Integer.parseInt(System.getProperty("ondemand.fixture.loads", "0")); }

    /** Log lines of the given kind since `mark` lines. */
    static java.util.List<String> logSince(int mark, String prefix) {
        String all = System.getProperty("ondemand.fixture.log", "");
        java.util.List<String> lines = all.isEmpty() ? java.util.List.of() : java.util.List.of(all.split("\n"));
        java.util.List<String> out = new java.util.ArrayList<>();
        for (String l : lines.subList(Math.min(mark, lines.size()), lines.size())) {
            if (l.startsWith(prefix)) out.add(l);
        }
        return out;
    }

    static int logMark() {
        String all = System.getProperty("ondemand.fixture.log", "");
        return all.isEmpty() ? 0 : all.split("\n").length;
    }

    /** The smallest file that reads as an x86_64 ELF shared object. */
    static byte[] tinyElf() {
        byte[] b = new byte[64];
        b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
        b[4] = 2;       // ELFCLASS64
        b[5] = 1;       // little endian
        b[6] = 1;       // EV_CURRENT
        b[16] = 3;      // ET_DYN
        b[18] = 62;     // EM_X86_64
        b[20] = 1;      // e_version
        return b;
    }

    /** The ELF a stub's .source sidecar names, canonical. */
    static String sourceOf(File stub) throws Exception {
        return canon(new File(new String(Files.readAllBytes(new File(stub.getPath() + ".source").toPath()),
                StandardCharsets.UTF_8).trim()));
    }

    static String canon(File f) throws Exception { return f.getCanonicalPath(); }

    public static void main(String[] args) throws Throwable {
        File translator = new File(args[0]).getAbsoluteFile();
        boolean off = args.length > 1 && args[1].equals("off");
        if (!System.mapLibraryName("x").endsWith(".dll")) {
            System.out.println("OnDemandNativeLoadTest: not a Windows host; Pattern N is Windows-only. FAIL");
            System.exit(1);
        }
        byte[] elf = tinyElf();
        Path tmp = Files.createTempDirectory("ondemand-test").toAbsolutePath();  // System.load takes absolute paths only
        System.out.println("work dir " + tmp + ", switch " + (off ? "off" : "on"));

        // NativeLog.print for the loader's log lines, from the fixture.
        System.load(new File(translator.getParentFile(), "winapk-guest-stub.dll").getPath());
        int base = loads();

        Path src = Files.createDirectories(tmp.resolve("src")).resolve("OnDemandGuest.java");
        Files.writeString(src, GUEST);
        Path guestDir = tmp.resolve("guest");
        int rc = ToolProvider.getSystemJavaCompiler().run(null, null, null,
                "--release", "21", "-d", guestDir.toString(), src.toString());
        check("guest compiles", rc, 0);

        byte[] raw = Files.readAllBytes(guestDir.resolve("OnDemandGuest.class"));
        ClassFileRewriter.Result r = ClassFileRewriter.maybeRewrite(raw.clone(), "OnDemandGuest");
        check("rewritten (" + r.reason + ")", r.rewritten, !off);
        if (!off) {
            check("a second pass finds nothing to rewrite",
                    ClassFileRewriter.maybeRewrite(r.bytes.clone(), "OnDemandGuest").rewritten, false);
        }

        File appDir = Files.createDirectories(tmp.resolve("app")).toFile();
        AppClassLoader loader = new AppClassLoader(new URL[] {guestDir.toUri().toURL()},
                OnDemandNativeLoadTest.class.getClassLoader(), null, appDir, translator, null);
        Class<?> g = loader.loadClass("OnDemandGuest");
        check("guest class is the app loader's", g.getClassLoader() == loader, true);

        // ---- System.mapLibraryName ----
        check("guest System.mapLibraryName(\"foo\")", call(g, "mapped", "foo"), off ? "foo.dll" : "libfoo.so");
        check("host System.mapLibraryName(\"foo\")", System.mapLibraryName("foo"), "foo.dll");

        // ---- ReLinker: lib/x86_64/<mapped name> out of the APK, System.load ----
        File apk = tmp.resolve("base.apk").toFile();
        try (ZipOutputStream z = new ZipOutputStream(Files.newOutputStream(apk.toPath()))) {
            z.putNextEntry(new ZipEntry("lib/x86_64/libfoo.so"));
            z.write(elf);
            z.closeEntry();
        }
        File appLib = Files.createDirectories(tmp.resolve("data/app_lib")).toFile();
        File foo = new File(appLib, "libfoo.so");
        int mark = logMark();
        Object relinked = call(g, "relink", apk.getPath(), "x86_64", "foo", appLib.getPath());
        if (off) {
            check("ReLinker lookup (JDK name)", relinked, "no lib/x86_64/foo.dll");
            Files.write(foo.toPath(), elf);
            String t = thrown(g, "systemLoad", foo.getPath());
            check("System.load(ELF) fails as before", t.startsWith("UnsatisfiedLinkError"), true);
            check("no on-demand stub", logSince(0, "on-demand native:").size(), 0);
            check("no stub file", new File(appDir, "lib_native").exists(), false);
            finish(tmp);
            return;
        }
        check("ReLinker lookup", relinked, "loaded libfoo.so");
        File fooStub = new File(appDir, "lib_native/libfoo.dylib");
        check("stub made in findLibrary's slot", fooStub.isFile(), true);
        check("stub's .source names the ELF", sourceOf(fooStub), canon(foo));
        check("stub's .src stamp is the ELF's size:mtime",
                Files.readString(new File(fooStub.getPath() + ".src").toPath()).trim(),
                foo.length() + ":" + foo.lastModified());
        java.util.List<String> made = logSince(mark, "on-demand native:");
        check("one log line for the stub", made.size(), 1);
        check("it says a new stub", made.size() == 1 && made.get(0).contains("-> new stub "), true);
        check("JNI_OnLoad ran once", loads() - base, 1);
        check("the guest's native resolves (bound to the app loader)", result(g, "ping"), 73);
        String twice = thrown(OnDemandNativeLoadTest.class, "hostLoad", fooStub.getPath());
        check("the JDK refuses the stub for the test's loader",
                twice.contains("already loaded in another classloader"), true);

        // ---- the same path again, both ways: the same module ----
        mark = logMark();
        call(g, "systemLoad", foo.getPath());
        call(g, "runtimeLoad", foo.getPath());
        call(g, "systemLoad", appLib.getPath() + File.separator + "." + File.separator + "libfoo.so");
        check("repeat loads make no stub and no log line", logSince(mark, "").size(), 0);
        check("repeat loads run no second JNI_OnLoad", loads() - base, 1);

        // ---- Runtime.load of an ELF the app unpacked itself ----
        File unpacked = Files.createDirectories(tmp.resolve("data/files/decompressed/libs.spo")).toFile();
        File bar = new File(unpacked, "libbar.so");
        Files.write(bar.toPath(), elf);
        mark = logMark();
        call(g, "runtimeLoad", bar.getPath());
        File barStub = new File(appDir, "lib_native/libbar.dylib");
        check("Runtime.load(ELF) makes a stub", barStub.isFile() && sourceOf(barStub).equals(canon(bar)), true);
        check("one log line for it", logSince(mark, "on-demand native:").size(), 1);
        check("JNI_OnLoad count", loads() - base, 2);

        // ---- a stub the startup exposure made is reused ----
        File soDir = Files.createDirectories(tmp.resolve("install/lib/x86_64")).toFile();
        File baz = new File(soDir, "libbaz.so");
        Files.write(baz.toPath(), elf);
        Method expose = AppClassLoader.class.getDeclaredMethod("exposeDylibsForSoLoader",
                File.class, File.class, java.util.Map.class);
        expose.setAccessible(true);
        int exposed = (int) expose.invoke(loader, soDir, soDir, new java.util.LinkedHashMap<String, File[]>());
        check("startup exposure made its stub", exposed, 1);
        check("and its X.dll copy", new File(soDir, "baz.dll").isFile(), true);
        File bazStub = new File(appDir, "lib_native/libbaz.dylib");
        mark = logMark();
        call(g, "systemLoad", baz.getPath());
        java.util.List<String> reused = logSince(mark, "on-demand native:");
        check("System.load reuses it", reused.size() == 1 && reused.get(0).endsWith("-> reused stub " + bazStub.getPath()), true);
        check("without running the translator", logSince(mark, "running translator:").size(), 0);

        // ---- a same-named ELF elsewhere does not take that slot ----
        File other = Files.createDirectories(tmp.resolve("data/files/other")).toFile();
        File baz2 = new File(other, "libbaz.so");
        Files.write(baz2.toPath(), elf);
        mark = logMark();
        call(g, "systemLoad", baz2.getPath());
        java.util.List<String> own = logSince(mark, "on-demand native:");
        check("it gets a slot of its own",
                own.size() == 1 && own.get(0).contains("lib_native" + File.separator + "ondemand" + File.separator), true);
        check("the startup stub still names its own ELF", sourceOf(bazStub), canon(baz));
        check("JNI_OnLoad count", loads() - base, 4);
        check("the guest's native still resolves", result(g, "ping"), 73);

        // ---- anything but an ELF is loaded as given ----
        mark = logMark();
        String missing = thrown(g, "systemLoad", new File(tmp.toFile(), "missing.so").getPath());
        check("a missing file: the JDK's UnsatisfiedLinkError", missing.startsWith("UnsatisfiedLinkError"), true);
        String relative = thrown(g, "systemLoad", "libfoo.so");
        check("a relative path: the JDK's UnsatisfiedLinkError",
                relative.startsWith("UnsatisfiedLinkError") && relative.contains("absolute"), true);
        check("null: the JDK's NullPointerException", thrown(g, "systemLoad", (Object) null).startsWith("NullPointerException"), true);
        check("no stub for those", logSince(mark, "on-demand native:").size(), 0);

        finish(tmp);
    }

    /** System.load from THIS class, i.e. for the test's own loader. */
    public static void hostLoad(String path) { System.load(path); }

    static void finish(Path tmp) {
        System.out.println(failures == 0 ? "OnDemandNativeLoadTest: OK"
                                         : "OnDemandNativeLoadTest: " + failures + " failure(s)");
        System.out.println("(loaded DLLs stay locked until exit; work dir left at " + tmp + ")");
        System.exit(failures == 0 ? 0 : 1);
    }
}
