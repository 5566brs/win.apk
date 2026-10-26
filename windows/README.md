# Win.apk: the Windows x64 port

Win.apk is an unofficial port of [Mac.apk](https://github.com/kksimp/Mac.apk) to
Windows on Intel/AMD (x64). It is not affiliated with or endorsed by the Mac.apk
project or Voltare. Everything outside `windows/` is the upstream tree. It is kept
as close to upstream as possible so that `git merge upstream/main` stays cheap.

## Why x64 is a different problem from the Mac

Mac.apk runs an app's native code as is, because Apple Silicon and Android phones
are both arm64. An Intel/AMD CPU cannot execute arm64 code, so on x64 Android apps
fall into three groups:

| Apps | Plan |
|---|---|
| Java/Kotlin only (no `.so`) | Run on HotSpot like on the Mac. Needs only the host layer (window, input, ANGLE on D3D11, audio). |
| Ship `lib/x86_64/*.so` | Load the x86_64 libraries natively with the loader in this directory. Full speed. |
| ARM-only native code | Needs dynamic binary translation (dynarmic, 0BSD). This is a research track and comes last. |

## What is here

| Path | What it is |
|---|---|
| `include/winapk/elf64_defs.h` | ELF64 / x86-64 psABI layouts (Windows has no `<elf.h>`) |
| `include/winapk/guest_abi.h` | `WINAPK_GUEST_ABI`: the boundary between System V and Microsoft x64 calls |
| `src/elf_loader.cpp` | Android x86_64 ELF loader: segments, RELA/RELR/APS2 relocations, protections, RELRO, init/fini |
| `src/elf_tls.cpp` | Dynamic ELF TLS for guest libraries: module ids, `__tls_get_addr`, a block per thread and module |
| `src/fs_tls_patch.cpp` | Rewrites the Bionic stack guard `%fs:0x28` to `%gs:0x28` (`NT_TIB.ArbitraryUserPointer`) |
| `src/mini_bionic.cpp`, `src/bionic/` | The Windows Bionic shim (sysv_abi functions over the UCRT and Win32): strings, printf, pthread, files, time, `mmap`, system properties and the environment, Android paths, pipes/eventfd/epoll, sockets on Winsock, `ALooper`, and `libz.so` on a statically linked zlib. `test/bionic_test.cpp` (`winapk-bionic-test`) checks it against `test/guest/bionic_guest.c` |
| `tools/elfrun_main.cpp` | `winapk-elfrun`: loads `.so` files and runs the PoC checks |
| `test/guest/poc_guest.c` | The Android x86_64 test library |
| `include/winapk/abi_thunk.h`, `src/abi_thunk.cpp` | Runtime machine-code thunks for the JNI bridge: native-method thunks from Microsoft x64 callers to System V code (built per Java signature) and the System V variadic `va_list` entry stub |
| `tools/gen_jni_slots.py`, `src/jni_slots.inc` | Generated `JNINativeInterface_` slot table (235 slots) the guest `JNIEnv` wrapper is built from |
| `include/winapk/guest_jni.h`, `src/guest_jni.cpp` | The guest `JNIEnv`/`JavaVM` wrapper: per-thread guest env, a generic System V forwarder into HotSpot's table for every non-variadic slot, and `GetEnv`/`Attach`/`Detach`. Needs a JDK's `jni.h` at build time (`-DWINAPK_JNI_INCLUDE=$JAVA_HOME/include`) |
| `include/winapk/jni_variadic.h`, `src/jni_variadic.cpp` | The variadic `Call<T>Method` / `...V` / `NewObject` bridge (31 families, one template keyed on the `...A` slot): reads the argument stream against the method signature and repacks it into the `jvalue[]` of HotSpot's A-form. Needs a `SignatureProvider` (JVMTI-backed in the real host, a map in tests) |
| `include/winapk/linker.h`, `src/linker.cpp` | The guest linker: loads a library with its `DT_NEEDED` tree (bionic's lookup order, system libraries answered by the shim), keeps the registry of loaded images, and implements the guest's `dlopen`, `dlsym`, `dladdr`, `dlerror` and `dlclose`. `test/linker_test.cpp` (`winapk-linker-test`) checks it against `test/guest/link_guest.c` |
| `include/winapk/jni_bind.h`, `src/jni_bind.cpp` | Binding guest natives into HotSpot: the guest `RegisterNatives` slot wraps each function in a thunk, and a JVMTI `NativeMethodBind` hook does the same for natives bound by name. Also the cached JVMTI signature provider |
| `host/` | `winapk-runtime.exe`, the host that runs an APK: DEX conversion, the in-process JVM, the host natives (window and input, `NativeOs`, `NativeLooper`, `NativeImageCodec` with libwebp and WIC, `NativeAudio` on WASAPI; the rest are still stubs), app native libraries (`guest_libs.cpp`), and `Boot.run`. `test/audio_test.cpp` (`winapk-audio-test`) checks the audio backend |
| `scripts/` | `build_java.sh` (upstream's Java runtime with a stock JDK), `build_framework_res.sh` (the platform resource APK) and `stage_runtime.sh` (a runnable `bin/` + `lib/` folder) |
| `tools/jvmrun_main.cpp` | `winapk-jvm-run`: the real-JVM end-to-end test. Creates a HotSpot JVM, loads an Android x86_64 `.so` through the guest linker, runs its `JNI_OnLoad` (which registers a native itself), binds a second native through the `NativeMethodBind` hook, and calls both from Java; one of them calls back into Java through the guest `JNIEnv`. `test/guest/jni_guest.c` and `test/java/WinApkJniTest.java` are its inputs; `test/build_e2e.sh` builds them and `test/run_e2e_wine.sh` runs it under Wine |

What the PoC checks: int/double/stack arguments across the ABI boundary, RELATIVE,
`R_X86_64_64`, GLOB_DAT/JUMP_SLOT, RELR and Android APS2 relocations, `DT_INIT_ARRAY`,
calls from guest code into the shim, host callbacks into guest code, and stack-protected guest code
on the main thread and on new threads.

## Build and run

On Windows ([MSYS2](https://www.msys2.org/), CLANG64 shell):

```sh
pacman -S mingw-w64-clang-x86_64-{clang,cmake,ninja,zlib}
cmake -S windows -B build -G Ninja && cmake --build build
./build/winapk-elfrun.exe path/to/libwinapk_poc_guest.so
```

The guest libraries are built with plain clang + lld, so no NDK is needed:
`windows/test/build_guest.sh`.

On Linux (cross-compile, run under Wine): `windows/test/run_poc_wine.sh`.

The JNI library and its tests (`winapk-jni-test`, `winapk-jni-variadic-test`,
`winapk-jvm-run`) build only when CMake is given a JDK's include directory:
`-DWINAPK_JNI_INCLUDE=$JAVA_HOME/include`.
The real-JVM test runs under Wine with `windows/test/run_e2e_wine.sh`; see
`PORTING.md`, section 9, for the full local setup.

CI: `.github/workflows/windows-poc.yml` builds the guests on Ubuntu and runs all
tests on a real `windows-latest` runner.

Toolchain: clang only. MSVC cannot emit System V calls, and mingw GCC 13 emits
broken SEH unwind data for `sysv_abi` functions.

## Trying it

Download `Win.apk-Setup.exe` (or `Win.apk-windows-x64.zip`) from the
repository's Releases page. The installer puts Win.apk in the Start menu for
the current user and opens `.apk` files with it. `Win.apk.exe` is the app
library (`launcher/launcher_main.cpp`, the counterpart of upstream's
`launcher/`):

- **Adding an app:** drop an APK on it to install the app. The APK is copied into
  `%LOCALAPPDATA%\Win.apk\library`, installed and converted once by
  `winapk-runtime --install-only` (which also renders the app's icon and
  resolves its name), and given Start menu and desktop shortcuts with its own
  icon.
- **Running and removing:** double-click an app to run it, or right-click to
  remove it.

The package holds the runtime, a Java runtime, ANGLE and `README.txt`, which
says what works, where app data and logs are, and what to attach to a bug
report. Every CI run on
`windows-latest` also uploads the same folder as the `winapk-preview`
artifact.

A release is made by running the "Windows release" workflow
(`.github/workflows/windows-release.yml`) by hand with a tag such as
`v0.1.0-preview`: it builds on Windows, packages `windows/package/` with the
staged runtime, boots an APK from the package as a smoke test, builds the
installer with Inno Setup, and publishes both as a GitHub pre-release with
`windows/package/RELEASE_NOTES.md`.

## Running an APK

```sh
windows/scripts/stage_runtime.sh build runtime     # after building with CMake
runtime/bin/winapk-runtime.exe --apk app.apk --jdk "$JAVA_HOME"
```

`--apk` also takes a split bundle (`.apkm`, `.xapk` or `.apks`), installed once
as `base.apk` plus `split_*.apk` under `%LOCALAPPDATA%\Win.apk\app`. The app
opens in its own window, with its own icon, and takes mouse, wheel and keyboard input
(Escape is Android's Back). Java-only apps work this way, including OpenGL
from Java (`GLSurfaceView`, libGDX) on ANGLE, and so do apps that draw with
OpenGL ES from their own native code through a SurfaceView (SDL games);
`NativeActivity` apps and native audio (OpenSL ES, AAudio) are not there yet. App data goes to `%LOCALAPPDATA%\Win.apk`.

The runtime and the app data may live in folders with names in any language,
whatever "Language for non-Unicode programs" is set to: the runtime's manifest
makes its ANSI code page UTF-8 (Windows 10 1903 and later), which is how the
JVM gets such paths whole, and the steps that run in a separate JVM (install,
DEX conversion) run in a child of `winapk-runtime.exe` rather than `java.exe`.
Older Windows ignores the manifest; there the runtime passes the JVM 8.3 short
names, which covers the app data, but the runtime's own folder needs plain
letters (the JDK loads its DLLs by their long names).

The app sees a 360x800 dp phone, the most common phone screen.
`WINAPK_PHONE_SIZE=412x915` sets another size, in dp. The window shows the
phone at the monitor's scale. When the whole window, title bar included, would
not fit the monitor's work area (the screen without the taskbar), the app gets
a lower density instead, so it keeps its dp size and still draws at the
window's own pixel size. The window is fitted again when it moves to another
monitor or the taskbar changes. `WINAPK_DENSITY` (a dpi) overrides the density.

### Web content (WebView2)

Web pages an app shows run in Microsoft Edge WebView2, the counterpart of
upstream's WKWebView. It needs the WebView2 Runtime, which Windows 11 and most
Windows 10 machines already have; `WebView2Loader.dll`, the SDK's
redistributable loader, sits next to `winapk-runtime.exe`
(`windows/scripts/fetch_webview2.sh`). Without the runtime, without the loader,
headless, or with `WINAPK_WEBVIEW2=0`, web links open in the system browser as
before, and the log says so once.

- A Custom Tab or Trusted Web Activity launch (androidx.browser's
  `CustomTabsIntent`, AppAuth's sign-in, a TWA launcher) opens a tab window over
  the app: close, back and the URL on a toolbar, the page title as the window
  title. As in a Custom Tab on a phone, every navigation is checked against the
  app's own manifest: a page that goes to a URL the app handles (an OAuth or
  sign-in callback, as a custom scheme or an `https` App Link) is not loaded but
  delivered to the app as the browser's `ACTION_VIEW` intent, and the tab
  closes. Other pages keep loading in the tab. The browser row the
  PackageManager lists (`com.macapk.host`) answers androidx.browser's Custom
  Tabs service, so apps that look for one choose a Custom Tab.
- A plain link (an "open in browser", a Privacy Policy link) still opens in the
  system browser: on a phone that is the user's browser app too.
- Cookies and storage are the app's own, in `app_webview2` in its data
  directory, kept between runs.

For tests, `WINAPK_TEST_WORKAREA=1366x728` stands in for the work area, in
pixels (`1366x728-0+0` puts it at the top right of the real one;
`@file` reads the value from a file at every fit),
`WINAPK_HEADLESS=1` runs without a window,
`WINAPK_HEADLESS_FRAME=frame.bmp` saves the last frame the app drew, and
`WINAPK_HEADLESS_EXIT_AFTER_FRAMES=30` exits that many display frames after the
first one the app presents (an idle app counts too). On
Linux, `windows/test/run_app_wine.sh` boots an APK headless under Wine.

## Status and next steps

The goal is the whole of Mac.apk on Windows x64: every Mac piece ported or
replaced with its Windows counterpart (PORTING.md section 4 lists them one by
one), then x86_64 apps at full speed, then ARM-only apps through translation.

Done so far:

- The loader, linker, ELF TLS and the JNI bridge for Android x86_64 libraries.
- The Bionic shim: about 520 libc functions (threads, files, sockets,
  epoll, `ALooper`, printf and scanf), on the UCRT and Win32.
- The host: the window and input, `NativeOs`, `NativeLooper`, images (libwebp,
  WIC), audio (WASAPI), clipboard, file pickers, game controllers, ICU,
  SQLite.
- OpenGL ES on ANGLE, from Java and from app native code (`ANativeWindow`,
  EGL window surfaces), plus libandroid's assets.
- The app library (`Win.apk.exe`) with shortcuts per app, the installer
  (`package/winapk.iss`) and the release workflow.
- WebView2 for Custom Tabs and Trusted Web Activities: sign-in pages whose
  callback returns to the app.

Next, in order: OpenSL ES and AAudio for native sound, `NativeActivity`,
`NativeMedia` (MediaPlayer and video on Media Foundation), WebView2 behind
`android.webkit.WebView`, the camera and location, signals through vectored
exception handling, then the launcher (M8) and ARM translation (D8).

`PORTING.md` holds the design decisions, the technical findings, the known
gaps, and the milestone plan up to a production release.

## Keeping up with upstream

```sh
git remote add upstream https://github.com/kksimp/Mac.apk.git   # once
git fetch upstream
git merge upstream/main
```

Win.apk code lives under `windows/`. Upstream files are changed only where a
Windows hook is unavoidable, and every such change is listed here:

- `.github/workflows/cla.yml`: the CLA job runs only in `kksimp/Mac.apk`.
- `README.md`: a Win.apk section at the top.
- `runtime/java/android/content/res/Resources.java`: `getIntArray` resolves the
  items of a typed `<array>` (for example a list of color references) instead
  of returning an empty array. Not Windows-specific; worth offering upstream.
- `runtime/java/android/view/ViewPropertyAnimator.java`: starts by itself on
  the next frame when the app never calls `start()`, as on Android. Not
  Windows-specific; worth offering upstream.
- `runtime/java/android/widget/RatingBar.java` and `ProgressBar.java`: a rating
  bar reads `numStars`, `rating`, `stepSize` and `isIndicator` from XML and
  measures one row of stars, as AOSP does. Not Windows-specific.
- `runtime/java/android/database/sqlite/SQLiteDatabase.java`: `BEGIN`,
  `COMMIT` and `ROLLBACK` given to `execSQL` nest in the transaction stack, as
  AOSP's `SQLiteSession.executeSpecial` does. Not Windows-specific.
- `runtime/java/android/view/LayoutInflater.java`: a `<merge>` layout inflated
  without a parent merges into a `FrameLayout` instead of being dropped, so
  `setContentView` of a `<merge>` layout works. Not Windows-specific.
- `runtime/java/com/macapk/loader/ActivityHost.java`: an activity that calls
  `finish()` in its own `onCreate` is torn down after `onCreate` returns, as on
  AOSP, not in the middle of it. Not Windows-specific.
- `runtime/java/com/macapk/loader/FragmentInflaterFactory.java`: a fragment in
  a layout is hosted by the activity the inflater belongs to, which also works
  during the first activity's `onCreate`. Not Windows-specific.
- `runtime/java/android/security/keystore/AndroidKeyStoreKeyFactorySpi.java`:
  delegates to the other providers only, instead of recursing into itself
  until the stack overflows. Not Windows-specific.
- `runtime/java/android/os/Build.java`, `dalvik/system/VMRuntime.java`,
  `com/macapk/loader/Boot.java` (`os.arch`), `InstalledSplits.java` and
  `MacApkPackageManager.java`: the device ABI is the host's, so x86_64 on
  Windows x64, and the native library directory includes `lib/x86_64`. On the
  Mac nothing changes. `Boot.java` also looks for `bin/macapk-translator.exe`.
- `runtime/java/android/content/Context.java`: `getDatabasePath` also takes a
  drive-letter path as absolute, so a database named by its full path is not
  nested inside `databases\`.
- `runtime/java/com/macapk/loader/CameraBridge.java`: when the capture natives
  are unbound or report no device (Windows has no capture backend yet), one
  virtual back camera delivers dark-grey frames, so screens built around a
  preview open and their other controls work. `MACAPK_VIRTUAL_CAMERA=0` turns
  it off. On the Mac a real camera always wins.
- `runtime/java/com/macapk/loader/ActivityHost.java` and
  `MacApkPackageManager.java`: a Custom Tab launch opens in
  `HostBrowserTab` (new, with `HostCustomTabsService`) when the host has a
  browser tab of its own, and the host browser row then answers the Custom Tabs
  service; its PackageInfo carries `signingInfo`. On the Mac, which has no such
  tab, nothing changes but the `signingInfo`.

## License

AGPL-3.0, like Mac.apk (see `LICENSE`). The Mac.apk name and logo belong to the
upstream project (see `TRADEMARKS.md`).
