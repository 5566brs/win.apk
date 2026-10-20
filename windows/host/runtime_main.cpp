// runtime_main.cpp -- winapk-runtime, the Windows host that runs an APK.
//
// The Windows counterpart of runtime/src/main.cpp, in the same order:
//
//   1. Find the runtime files: <root>/lib holds the jars that
//      scripts/build_java.sh builds, <root>/jre (or JAVA_HOME) the JDK.
//   2. Convert the APK's DEX to <app dir>/classes.jar with upstream's
//      converter (com.macapk.dex.Main), in a child java.exe, unless the cached
//      jar is newer than the APK.
//   3. Create the JVM in-process with upstream's options, on a thread with a
//      large stack (-Xss does not apply to the thread that creates the VM).
//   4. Bind the host natives (host_natives.h).
//   5. Call com.macapk.loader.Boot.run.
//
//   winapk-runtime --apk <file.apk> [--data <dir>] [--jdk <dir>] [--lib <dir>]
//
// App data lives under %LOCALAPPDATA%\Win.apk unless --data says otherwise.
// There is no window yet (milestone M2); what the app does shows in the log.

#include "host_natives.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <jni.h>
#include <jvmti.h>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path apk, data, jdk, lib;
};

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring env_var(const wchar_t* name) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return (n > 0 && n < 32768) ? std::wstring(buf, n) : std::wstring();
}

fs::path exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    return fs::path(std::wstring(buf, n)).parent_path();
}

// HotSpot reads option strings in the ANSI code page, so a path with
// characters outside it (a user name in Hebrew, say) would not survive. The
// short 8.3 form is plain ASCII; NTFS has it unless the volume disabled it.
std::string jvm_path(const fs::path& p) {
    const std::wstring w = p.wstring();
    bool ascii = true;
    for (wchar_t c : w) ascii = ascii && c < 0x80;
    if (ascii) return utf8(w);
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetShortPathNameW(w.c_str(), buf, MAX_PATH * 4);
    if (n == 0 || n >= MAX_PATH * 4) {
        std::fprintf(stderr, "[win.apk] warning: no ASCII short name for %s; the JVM may not find it\n",
                     utf8(w).c_str());
        return utf8(w);
    }
    return utf8(std::wstring(buf, n));
}

bool parse_args(int argc, wchar_t** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&](fs::path* out) {
            if (i + 1 >= argc) return false;
            *out = fs::path(argv[++i]);
            return true;
        };
        bool ok = true;
        if (a == L"--apk") ok = value(&o->apk);
        else if (a == L"--data") ok = value(&o->data);
        else if (a == L"--jdk") ok = value(&o->jdk);
        else if (a == L"--lib") ok = value(&o->lib);
        else ok = false;
        if (!ok) return false;
    }
    return !o->apk.empty();
}

// Fills in whatever the command line left out, and checks it exists.
bool resolve_paths(Options* o) {
    std::error_code ec;
    o->apk = fs::absolute(o->apk, ec);
    if (!fs::is_regular_file(o->apk, ec)) {
        std::fprintf(stderr, "[win.apk] no such APK: %s\n", utf8(o->apk.wstring()).c_str());
        return false;
    }
    const fs::path root = exe_dir().parent_path();
    if (o->lib.empty()) o->lib = root / "lib";
    if (!fs::is_regular_file(o->lib / "android-framework.jar", ec)) {
        std::fprintf(stderr, "[win.apk] android-framework.jar not found in %s (use --lib)\n",
                     utf8(o->lib.wstring()).c_str());
        return false;
    }
    if (o->jdk.empty()) {
        if (!env_var(L"WINAPK_JAVA_HOME").empty()) o->jdk = env_var(L"WINAPK_JAVA_HOME");
        else if (fs::exists(root / "jre" / "bin" / "server" / "jvm.dll", ec)) o->jdk = root / "jre";
        else o->jdk = env_var(L"JAVA_HOME");
    }
    if (!fs::exists(o->jdk / "bin" / "server" / "jvm.dll", ec)) {
        std::fprintf(stderr, "[win.apk] no JDK with bin\\server\\jvm.dll (use --jdk or JAVA_HOME)\n");
        return false;
    }
    if (o->data.empty()) {
        const std::wstring local = env_var(L"LOCALAPPDATA");
        if (local.empty()) {
            std::fprintf(stderr, "[win.apk] LOCALAPPDATA is not set (use --data)\n");
            return false;
        }
        o->data = fs::path(local) / L"Win.apk";
    }
    return true;
}

// Runs a command line and returns its exit code, or -1 if it did not start.
int run_process(std::wstring cmdline) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::wstring quoted(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

// Step 2. Exit codes are com.macapk.dex.Main's: 0 converted, 3 no DEX in the
// APK (a valid case: the app then has no classes.jar).
bool convert_dex(const Options& o, const fs::path& app_dir, fs::path* jar) {
    std::error_code ec;
    *jar = app_dir / "classes.jar";
    if (fs::exists(*jar, ec) && fs::last_write_time(*jar, ec) >= fs::last_write_time(o.apk, ec)) return true;
    std::fprintf(stderr, "[win.apk] converting the app's code (once per install)...\n");
    const fs::path tmp = app_dir / "classes.jar.tmp";
    const int rc = run_process(quoted(o.jdk / "bin" / "java.exe") + L" -cp " + quoted(o.lib / "macapk-loader.jar") +
                               L" com.macapk.dex.Main " + quoted(o.apk) + L" " + quoted(tmp));
    if (rc == 3) {
        jar->clear();
        return true;
    }
    if (rc != 0) {
        std::fprintf(stderr, "[win.apk] DEX conversion failed (exit code %d)\n", rc);
        return false;
    }
    fs::rename(tmp, *jar, ec);
    return !ec;
}

struct BootArgs {
    Options options;
    fs::path app_dir, app_jar;
    int result = 1;
};

std::vector<std::string> jvm_options(const BootArgs& b) {
    const Options& o = b.options;
    std::error_code ec;
    std::vector<std::string> v;
    v.push_back("-Djava.class.path=" + jvm_path(o.lib / "android-framework.jar") + ";" +
                jvm_path(o.lib / "macapk-loader.jar") + ";" + jvm_path(o.lib / "bcprov-jdk18on-1.79.jar"));
    // Upstream's set (runtime/src/jvm_loader.cpp), minus the Mac-only parts.
    v.push_back("-Xrs");
    v.push_back("-Djava.library.path=");
    v.push_back("-XX:+UseSerialGC");
    v.push_back("-Xms128m");
    v.push_back("-Xmx1g");
    v.push_back("-Xss16m");
    v.push_back("-XX:+UnlockDiagnosticVMOptions");
    v.push_back("-XX:-BytecodeVerificationRemote");
    v.push_back("-XX:-BytecodeVerificationLocal");
    v.push_back("--enable-native-access=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/java.io=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/java.util=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/java.lang=ALL-UNNAMED");
    v.push_back("--add-opens=java.base/jdk.internal.loader=ALL-UNNAMED");
    v.push_back("--add-exports=java.base/sun.net.www.protocol.http=ALL-UNNAMED");
    v.push_back("--add-exports=java.base/sun.net.www.protocol.https=ALL-UNNAMED");
    if (fs::is_directory(o.lib / "java_base_patch", ec))
        v.push_back("--patch-module=java.base=" + jvm_path(o.lib / "java_base_patch"));
    if (fs::is_directory(o.lib / "jdk_unsupported_patch", ec))
        v.push_back("--patch-module=jdk.unsupported=" + jvm_path(o.lib / "jdk_unsupported_patch"));
    // Android's file system root (data/data/<app>, /proc mirror) and logs.
    const fs::path android_root = o.data / "android";
    const fs::path logs = o.data / "logs";
    fs::create_directories(android_root, ec);
    fs::create_directories(logs, ec);
    v.push_back("-Dmacapk.android.root=" + jvm_path(android_root));
    v.push_back("-Dmacapk.bundle.apk=" + jvm_path(o.apk));
    v.push_back("-XX:ErrorFile=" + jvm_path(logs) + "\\hs_err_pid%p.log");
    return v;
}

void check_exception(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) return;
    std::fprintf(stderr, "[win.apk] exception in %s:\n", what);
    env->ExceptionDescribe();
    env->ExceptionClear();
}

// Steps 3 to 5, on the large-stack thread.
DWORD WINAPI boot_thread(void* param) {
    auto* b = static_cast<BootArgs*>(param);
    const Options& o = b->options;

    // jvm.dll needs the DLLs next to java.exe (the VC runtime among them).
    SetDllDirectoryW((o.jdk / "bin").wstring().c_str());
    HMODULE jvm = LoadLibraryW((o.jdk / "bin" / "server" / "jvm.dll").wstring().c_str());
    if (!jvm) {
        std::fprintf(stderr, "[win.apk] cannot load jvm.dll (error %lu)\n", GetLastError());
        return 1;
    }
    using CreateVm = jint(JNICALL*)(JavaVM**, void**, void*);
    auto create = reinterpret_cast<CreateVm>(GetProcAddress(jvm, "JNI_CreateJavaVM"));
    if (!create) return 1;

    std::vector<std::string> opts = jvm_options(*b);
    std::vector<JavaVMOption> jopts(opts.size());
    for (size_t i = 0; i < opts.size(); ++i) {
        jopts[i].optionString = &opts[i][0];
        jopts[i].extraInfo = nullptr;
    }
    JavaVMInitArgs args{};
    args.version = JNI_VERSION_21;
    args.nOptions = static_cast<jint>(jopts.size());
    args.options = jopts.data();
    args.ignoreUnrecognized = JNI_FALSE;

    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    if (create(&vm, reinterpret_cast<void**>(&env), &args) != JNI_OK) {
        std::fprintf(stderr, "[win.apk] JNI_CreateJavaVM failed\n");
        return 1;
    }
    jvmtiEnv* jvmti = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK) {
        std::fprintf(stderr, "[win.apk] no JVMTI\n");
        return 1;
    }

    winapk::host::NativeStats stats;
    if (!winapk::host::register_host_natives(env, jvmti, &stats)) {
        std::fprintf(stderr, "[win.apk] binding host natives failed\n");
        return 1;
    }
    std::fprintf(stderr, "[win.apk] host natives: %d classes, %d implemented, %d stubbed\n", stats.classes,
                 stats.implemented, stats.stubbed);

    jclass boot = env->FindClass("com/macapk/loader/Boot");
    check_exception(env, "FindClass com.macapk.loader.Boot");
    jmethodID run = boot ? env->GetStaticMethodID(
                               boot, "run",
                               "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V")
                         : nullptr;
    if (!run) {
        check_exception(env, "Boot.run lookup");
        return 1;
    }
    // Java strings take UTF-8 here (modified UTF-8 is identical outside NUL
    // and supplementary characters, which paths do not contain).
    auto jstr = [&](const fs::path& p) { return env->NewStringUTF(utf8(p.wstring()).c_str()); };
    // The manifest argument names a decoded text manifest, which Windows does
    // not produce; Manifest.parse then reads the binary one from the APK.
    env->CallStaticVoidMethod(boot, run, jstr(o.apk), jstr(b->app_jar), jstr(b->app_dir),
                              jstr(b->app_dir / "AndroidManifest.txt"));
    check_exception(env, "Boot.run");

    // Returns once the app's non-daemon threads are done.
    vm->DestroyJavaVM();
    b->result = 0;
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    BootArgs b;
    if (!parse_args(argc, argv, &b.options)) {
        std::fprintf(stderr, "usage: winapk-runtime --apk <file.apk> [--data <dir>] [--jdk <dir>] [--lib <dir>]\n");
        return 2;
    }
    if (!resolve_paths(&b.options)) return 2;

    std::error_code ec;
    b.app_dir = b.options.data / "apps" / b.options.apk.stem();
    fs::create_directories(b.app_dir, ec);
    if (ec) {
        std::fprintf(stderr, "[win.apk] cannot create %s\n", utf8(b.app_dir.wstring()).c_str());
        return 1;
    }
    if (!convert_dex(b.options, b.app_dir, &b.app_jar)) return 1;

    // Upstream runs the JVM with -Xss16m; give the creating thread as much.
    HANDLE t = CreateThread(nullptr, 64 * 1024 * 1024, boot_thread, &b, STACK_SIZE_PARAM_IS_A_RESERVATION,
                            nullptr);
    if (!t) return 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return b.result;
}
