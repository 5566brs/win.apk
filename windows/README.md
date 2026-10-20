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

## What is here (phase 1: loader proof of concept)

| Path | What it is |
|---|---|
| `include/winapk/elf64_defs.h` | ELF64 / x86-64 psABI layouts (Windows has no `<elf.h>`) |
| `include/winapk/guest_abi.h` | `WINAPK_GUEST_ABI`: the System V ↔ Microsoft x64 call boundary |
| `src/elf_loader.cpp` | Android x86_64 ELF loader: segments, RELA/RELR/APS2 relocations, protections, RELRO, init/fini |
| `src/fs_tls_patch.cpp` | Bionic stack guard `%fs:0x28` → `%gs:0x28` (`NT_TIB.ArbitraryUserPointer`) |
| `src/mini_bionic.cpp` | First slice of the Windows Bionic shim (sysv_abi wrappers over the UCRT) |
| `tools/elfrun_main.cpp` | `winapk-elfrun`: loads `.so` files and runs the PoC checks |
| `test/guest/poc_guest.c` | The Android x86_64 test library |
| `include/winapk/abi_thunk.h`, `src/abi_thunk.cpp` | Runtime machine-code thunks for the JNI bridge: Microsoft→System V native-method thunks (per Java descriptor) and the System V variadic `va_list` entry stub (phase 2 groundwork) |
| `tools/gen_jni_slots.py`, `src/jni_slots.inc` | Generated `JNINativeInterface_` slot table (235 slots) the guest `JNIEnv` wrapper is built from |
| `include/winapk/guest_jni.h`, `src/guest_jni.cpp` | The guest `JNIEnv`/`JavaVM` wrapper: per-thread guest env, a generic System V forwarder into HotSpot's table for every non-variadic slot, and `GetEnv`/`Attach`/`Detach`. Needs a JDK's `jni.h` at build time (`-DWINAPK_JNI_INCLUDE=$JAVA_HOME/include`) |
| `include/winapk/jni_variadic.h`, `src/jni_variadic.cpp` | The variadic `Call<T>Method` / `...V` / `NewObject` bridge (31 families, one template keyed on the `...A` slot): reads the argument stream against the method signature and repacks it into the `jvalue[]` of HotSpot's A-form. Needs a `SignatureProvider` (JVMTI-backed in the real host, a map in tests) |

What the PoC checks: int/double/stack arguments across the ABI boundary, RELATIVE,
`R_X86_64_64`, GLOB_DAT/JUMP_SLOT, RELR and Android APS2 relocations, `DT_INIT_ARRAY`,
guest → host shim calls, host → guest callbacks, and stack-protected guest code
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

CI: `.github/workflows/windows-poc.yml` builds the guests on Ubuntu and runs the
checks on a real `windows-latest` runner.

Toolchain: clang only. MSVC cannot emit System V calls, and mingw GCC 13 emits
broken SEH unwind data for `sysv_abi` functions.

## Known limits of the PoC (on purpose, for now)

- **Variadic calls** (`printf`, `__android_log_print`, JNI `Call*Method`): the
  System V and Windows `va_list` layouts differ, so these need dedicated bridges.
- **ELF TLS** (`PT_TLS`, TLS relocations) and `%fs:0` (`__get_tls`): refused with
  a clear error. They need a decoder-driven rewrite (Zydis) instead of the 9-byte
  pattern match used for the stack guard.
- **`DT_NEEDED`** is reported, not followed. A global symbol scope across guest
  libraries comes next.
- **Signals**: none yet. Mapping Windows exceptions (VEH) to Android
  `ucontext_t` also has to account for the System V red zone, which Windows does
  not preserve when it delivers an exception.
- **Guest thread stacks**: Android code does not probe the stack, so guest
  threads will need fully committed stacks.

## Roadmap

- [x] **Phase 0**: repository, CI on a real Windows runner, upstream kept intact
- [ ] **Phase 1**: loader proof of concept (this directory). Done under Wine;
      the CI run on Windows decides
- [ ] **Phase 2**: Java-only apps: JVM host, window/input (`NativeCocoa` → Win32),
      ANGLE on D3D11, WASAPI audio, paths under `%LOCALAPPDATA%`
- [ ] **Phase 3**: x86_64 native apps: full Bionic shim (fd table, epoll/eventfd,
      pthread, mmap, signals over VEH), JNI bridge (System V `JNIEnv`), variadic
      bridges, ELF TLS, Vulkan, NativeActivity, picking the x86_64 split from
      `.apkm`/`.xapk`/`.apks`
- [ ] **Phase 4**: system services: WebView2, Media Foundation, location,
      gamepads, clipboard, file pickers, Google sign-in in the browser
- [ ] **Phase 5**: control panel, installer, Start-menu shortcuts per app,
      `.apk` file association, code signing
- [ ] **Phase 6** (research): ARM-only apps through dynarmic

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

## License

AGPL-3.0, like Mac.apk (see `LICENSE`). The Mac.apk name and logo belong to the
upstream project (see `TRADEMARKS.md`).
