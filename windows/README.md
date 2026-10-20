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
| `src/fs_tls_patch.cpp` | Rewrites the Bionic stack guard `%fs:0x28` to `%gs:0x28` (`NT_TIB.ArbitraryUserPointer`) |
| `src/mini_bionic.cpp` | First slice of the Windows Bionic shim (sysv_abi wrappers over the UCRT) |
| `tools/elfrun_main.cpp` | `winapk-elfrun`: loads `.so` files and runs the PoC checks |
| `test/guest/poc_guest.c` | The Android x86_64 test library |
| `include/winapk/abi_thunk.h`, `src/abi_thunk.cpp` | Runtime machine-code thunks for the JNI bridge: native-method thunks from Microsoft x64 callers to System V code (built per Java signature) and the System V variadic `va_list` entry stub |
| `tools/gen_jni_slots.py`, `src/jni_slots.inc` | Generated `JNINativeInterface_` slot table (235 slots) the guest `JNIEnv` wrapper is built from |
| `include/winapk/guest_jni.h`, `src/guest_jni.cpp` | The guest `JNIEnv`/`JavaVM` wrapper: per-thread guest env, a generic System V forwarder into HotSpot's table for every non-variadic slot, and `GetEnv`/`Attach`/`Detach`. Needs a JDK's `jni.h` at build time (`-DWINAPK_JNI_INCLUDE=$JAVA_HOME/include`) |
| `include/winapk/jni_variadic.h`, `src/jni_variadic.cpp` | The variadic `Call<T>Method` / `...V` / `NewObject` bridge (31 families, one template keyed on the `...A` slot): reads the argument stream against the method signature and repacks it into the `jvalue[]` of HotSpot's A-form. Needs a `SignatureProvider` (JVMTI-backed in the real host, a map in tests) |
| `include/winapk/linker.h`, `src/linker.cpp` | The guest linker: loads a library with its `DT_NEEDED` tree (bionic's lookup order, system libraries answered by the shim), keeps the registry of loaded images, and implements the guest's `dlopen`, `dlsym`, `dladdr`, `dlerror` and `dlclose`. `test/linker_test.cpp` (`winapk-linker-test`) checks it against `test/guest/link_guest.c` |
| `include/winapk/jni_bind.h`, `src/jni_bind.cpp` | Binding guest natives into HotSpot: the guest `RegisterNatives` slot wraps each function in a thunk, and a JVMTI `NativeMethodBind` hook does the same for natives bound by name. Also the cached JVMTI signature provider |
| `host/` | `winapk-runtime.exe`, the host that runs an APK: DEX conversion, the in-process JVM, the host natives (many still stubs), the app's Win32 window with input, and `Boot.run` |
| `scripts/` | `build_java.sh` (upstream's Java runtime with a stock JDK), `build_framework_res.sh` (the platform resource APK) and `stage_runtime.sh` (a runnable `bin/` + `lib/` folder) |
| `tools/jvmrun_main.cpp` | `winapk-jvm-run`: the real-JVM end-to-end test. Creates a HotSpot JVM, loads an Android x86_64 `.so` through the guest linker, runs its `JNI_OnLoad` (which registers a native itself), binds a second native through the `NativeMethodBind` hook, and calls both from Java; one of them calls back into Java through the guest `JNIEnv`. `test/guest/jni_guest.c` and `test/java/WinApkJniTest.java` are its inputs; `test/build_e2e.sh` builds them and `test/run_e2e_wine.sh` runs it under Wine |

What the PoC checks: int/double/stack arguments across the ABI boundary, RELATIVE,
`R_X86_64_64`, GLOB_DAT/JUMP_SLOT, RELR and Android APS2 relocations, `DT_INIT_ARRAY`,
calls from guest code into the shim, host callbacks into guest code, and stack-protected guest code
on the main thread and on new threads.

## Build and run

On Windows ([MSYS2](https://www.msys2.org/), CLANG64 shell):

```sh
pacman -S mingw-w64-clang-x86_64-{clang,cmake,ninja}
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

Every CI run on `windows-latest` uploads a `winapk-preview` artifact: a
ready-to-run folder with the runtime, a Java runtime and `Run APK.bat`. Unzip
it and drag an `.apk` onto `Run APK.bat`. Its `README.txt` says what works
and where the logs are. The folder comes from `windows/package/` plus the
staged runtime.

## Running an APK

```sh
windows/scripts/stage_runtime.sh build runtime     # after building with CMake
runtime/bin/winapk-runtime.exe --apk app.apk --jdk "$JAVA_HOME"
```

The app opens in its own window and takes mouse, wheel and keyboard input
(Escape is Android's Back). Java-only apps work this way; OpenGL apps and games
do not render yet. App data goes to `%LOCALAPPDATA%\Win.apk`.

For tests, `WINAPK_HEADLESS=1` runs without a window,
`WINAPK_HEADLESS_FRAME=frame.bmp` saves the last frame the app drew, and
`WINAPK_HEADLESS_EXIT_AFTER_FRAMES=30` stops after a number of frames. On
Linux, `windows/test/run_app_wine.sh` boots an APK headless under Wine.

## Status and next steps

`PORTING.md` holds the design decisions, the technical findings so far, the
known gaps, and the milestone plan up to a production release.

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

## License

AGPL-3.0, like Mac.apk (see `LICENSE`). The Mac.apk name and logo belong to the
upstream project (see `TRADEMARKS.md`).
