# Win.apk porting notes and roadmap

This document records what has been learned while porting Mac.apk to Windows
x64, the decisions that follow from it, the state of the code under `windows/`,
and the work that remains before a production release. It is meant for whoever
continues the work, so it prefers concrete facts and file names over narrative.
`windows/README.md` covers building and running; this file covers the why and
the what next.

Contents

1. Goal and scope
2. How Mac.apk works and what changes on Windows x64
3. Decisions
4. Current state
5. Technical findings
6. Upstream behavior the port has to preserve
7. Known gaps and risks
8. Roadmap to production
9. Development workflow
10. Reference

---

## 1. Goal and scope

The goal is to run Android apps on Windows 10 and 11 on x64 (Intel and AMD) as
ordinary Windows processes, with no emulator and no virtual machine, by porting
the Mac.apk runtime. Win.apk is unofficial and is not affiliated with the
Mac.apk project. It is licensed under AGPL-3.0, like upstream.

Not in scope for now:

- Windows on ARM. It would be closer to the Mac model, since arm64 app code
  could run natively there, but the target hardware is x64.
- 32-bit Windows and 32-bit (x86) Android libraries.

## 2. How Mac.apk works and what changes on Windows x64

The fork was taken from upstream version 1.0.2238. Upstream has four parts:

| Part | Location | Approx. size | Role |
|---|---|---|---|
| Android framework reimplementation, DEX-to-JVM converter, boot loader | `runtime/java/` | 570K lines of Java | Runs app Java code on a stock HotSpot JVM |
| Native runtime | `runtime/src/` | 54K lines of C++ and Objective-C++ | JVM host, JNI implementations of framework natives (window, audio, media, camera, WebView), Cocoa window, ANGLE glue, APK reader |
| Native-code layer | `runtime/translator/` | 150K lines of C++ | arm64 ELF loader, Bionic libc shim (about 75K lines), ARM32-to-arm64 recompiler through LLVM IR |
| Launcher | `launcher/` | 18K lines of Swift | Control panel, per-app `.app` bundle generator, install helper |

On Apple Silicon an app's arm64 `.so` runs directly. Apple's arm64 calling
convention and Android's AAPCS64 agree on register arguments, so HotSpot calls
guest native methods through thin Mach-O trampolines, and guest code receives
HotSpot's own `JNIEnv`. The Bionic shim maps Android libc calls onto Darwin.

None of this carries over directly to x64:

1. An Intel or AMD CPU cannot execute arm64 code. Apps fall into three groups:
   - Apps with only Java or Kotlin code need nothing beyond the host side
     (window, input, graphics, audio).
   - Apps that ship `lib/x86_64/*.so` can run their native code directly,
     loaded by a new x86_64 ELF loader. Many games ship x86-64 builds because
     Google Play Games for PC prefers them.
   - Apps that ship only ARM libraries need binary translation. That is a
     separate research track (milestone M10).
2. Android x86_64 code uses the System V AMD64 calling convention. Windows uses
   the Microsoft x64 convention. Every call between guest code and the host has
   to be bridged, including every JNI call in both directions.
3. The Bionic shim is written against POSIX and Darwin (kqueue, Mach, fork,
   signals, pthread). Windows needs its own implementation of most of it.
4. Mac services (Cocoa, Metal, AVFoundation, VideoToolbox, WKWebView,
   CoreLocation, GameController) need Windows equivalents.

What ports as is: almost all of `runtime/java`, including the DEX converter.
Only a small set of things there is Mac-specific: paths under `~/Library`, and
calls to macOS tools and APIs such as `plutil`, `lsregister`, `xattr`,
`NSWorkspace`, `open`, `pmset`, `lp` and `security`. Upstream's loader and shim
remain useful as a behavioral reference even where the code itself cannot be
reused.

## 3. Decisions

D1. Target Windows x64 with an x86_64 guest ABI. ARM-only apps come later, if
at all.

D2. Leave the upstream tree alone. All Windows code lives under `windows/`. The
only upstream files changed so far are `.github/workflows/cla.yml` (the CLA job
now runs only in `kksimp/Mac.apk`) and `README.md` (a Win.apk section at the
top). Every future change to an upstream file must be listed in
`windows/README.md`. This keeps `git merge upstream/main` cheap.

D3. Use clang only. MSVC cannot emit System V calls. MinGW GCC 13 accepts
`__attribute__((sysv_abi))` but emits broken SEH unwind directives for such
functions (`.seh_handlerdata used outside of .seh_proc block`). CI uses MSYS2
CLANG64. On Linux the code is cross-compiled with clang, lld and the mingw-w64
headers, and tested under Wine.

D4. Bridge the calling conventions with one rule: every host function that
guest code can call is declared `WINAPK_GUEST_ABI` (`sysv_abi`), and every guest
function pointer the host calls has a `WINAPK_GUEST_ABI` type. Clang then emits
the right prologues, including saving rdi, rsi and xmm6 to xmm15 where needed.
Calls whose shape is only known at runtime go through generated thunks
(`src/abi_thunk.cpp`).

D5. Load ELF segments by copying them into one `VirtualAlloc` region.
`MapViewOfFile` cannot be used, because its file offsets must be multiples of
the 64 KiB allocation granularity while ELF segments are aligned to 4 KiB or
16 KiB. This costs some memory, which is acceptable.

D6. Give guest code its own `JNIEnv` and `JavaVM`. Upstream patches HotSpot's
JNI function table in place. That cannot work here, because HotSpot's entries
use the Microsoft convention while guest code calls them with System V. The
guest table is generated from `jni.h`.

D7. Ship in small pull requests, each one green on a real `windows-latest`
runner before it is merged. Merge with rebase to keep history linear.

D8. Get x86_64 apps working end to end first (Java-only, then apps with
x86_64 libraries); ARM support (M10) comes after that. To keep that door open
without building anything for it now, guest code enters the system in exactly
three places: the loader (`ElfImage`, which refuses arm64 libraries with a
clear message), the linker's image registry (`GuestLinker`) and native-method
binding (`jni_bind`). An ARM backend would plug in there and nowhere else, so
new code must not call guest functions or inspect guest images around them.
An APK that ships only ARM libraries has to be reported as such to the user,
not fail somewhere later.

## 4. Current state

Everything in this section is merged on `main` and tested in CI on
`windows-latest`. Paths are relative to `windows/`.

### Components

| File | What it does |
|---|---|
| `include/winapk/elf64_defs.h` | ELF64 and x86-64 psABI structures and constants (Windows has no `<elf.h>`) |
| `include/winapk/guest_abi.h` | `WINAPK_GUEST_ABI` and a table of System V versus Microsoft x64 differences |
| `src/elf_loader.cpp` | Loads Android x86_64 shared objects: copies PT_LOAD segments; applies RELA, RELR (`DT_RELR` and `DT_ANDROID_RELR`) and Android APS2 packed relocations; sets per-page protections; resolves IRELATIVE; seals PT_GNU_RELRO; runs `DT_INIT` and `DT_INIT_ARRAY`; looks up exports and the symbol covering an address. The map, link and initialize phases can run separately |
| `src/linker.cpp` | The guest linker: loads a library with its `DT_NEEDED` tree in bionic's order (map all, link all, constructors dependencies first), answers system libraries from the shim, keeps the registry of loaded images, and provides the guest's `dlopen`, `android_dlopen_ext`, `dlsym` (with `RTLD_DEFAULT` and `RTLD_NEXT`), `dlclose`, `dlerror` and `dladdr` |
| `src/fs_tls_patch.cpp` | Rewrites Bionic stack-guard loads (`%fs:0x28`) to `%gs:0x28` |
| `src/mini_bionic.cpp` | First slice of the Windows Bionic shim: malloc family, string functions, `puts`, `__android_log_write`, `__stack_chk_fail`, `__errno`, `__cxa_atexit`, `__cxa_finalize`, and `qsort` calling a guest comparator |
| `src/abi_thunk.cpp` | Small x86-64 code emitter. Produces native-method thunks (Microsoft x64 caller to System V guest, built from a Java signature, swapping in the guest `JNIEnv*`) and System V variadic entry stubs |
| `tools/gen_jni_slots.py`, `src/jni_slots.inc` | Generated list of the 235 `JNINativeInterface_` slots, each tagged as plain, variadic or `va_list`, with the matching `...A` slot for the variadic ones |
| `src/guest_jni.cpp` | Guest `JNIEnv` and `JavaVM`: one shared System V table that forwards into HotSpot, a guest env per thread, and `GetEnv`, `AttachCurrentThread` and `DetachCurrentThread` |
| `src/jni_variadic.cpp` | The `Call<T>Method`, `Call<T>MethodV` and `NewObject` families. Reads the argument stream against the method signature and calls HotSpot's `Call<T>MethodA` form |
| `src/jni_bind.cpp` | Binds guest natives into HotSpot: the guest `RegisterNatives` slot wraps each function in a thunk, and a JVMTI `NativeMethodBind` hook swaps guest addresses for thunks. Thunks are cached per function and signature. Also the cached JVMTI signature provider |
| `host/runtime_main.cpp` | `winapk-runtime`, the host that runs an APK: converts its DEX with upstream's converter (cached), creates the JVM in-process with upstream's options, binds the host natives and calls `Boot.run` |
| `host/host_natives.cpp` | Binds every native method of upstream's 39 host classes: the Windows implementation where there is one, otherwise a stub that returns zero and logs its name on first call |
| `host/native_log.cpp`, `host/native_cocoa.cpp` | `NativeLog`, and a headless `NativeCocoa`: fixed screen metrics, a 60 Hz render tick driving the app's renderer, frames optionally written to a BMP |
| `scripts/build_java.sh`, `scripts/build_framework_res.sh`, `scripts/stage_runtime.sh` | Build upstream's Java runtime with a stock JDK, the platform resource APK with upstream's script and Google's `aapt2`, and stage `bin/` and `lib/` into a runnable folder |
| `tools/elfrun_main.cpp` | `winapk-elfrun`, the loader and ABI test runner |
| `tools/jvmrun_main.cpp` | `winapk-jvm-run`, the real-JVM end-to-end test |

### Tests

| Binary | What it shows |
|---|---|
| `winapk-elfrun` | Two builds of an Android test library (plain RELA, and APS2 plus RELR) load and run: int, double and stack-passed arguments; RELATIVE and `R_X86_64_64` relocations; constructors; shim calls; stack-protected code on the main thread and on new threads; a guest comparator called from host `qsort`; a host callback called from guest code; and the `gs:[0x28]` alias |
| `winapk-thunk-test` | The native-method thunk handles mixed, stack-overflowing and narrow arguments and swaps the env; the variadic entry stub builds a correct `va_list`; the tagged stub returns zero for any return type |
| `winapk-jni-test` | The guest `JNIEnv` forwards correctly against a mock JVM, for several return types and argument shapes, and `JavaVM::GetEnv` hands back a guest env |
| `winapk-jni-variadic-test` | The variadic bridge works for all four call kinds (virtual, static, nonvirtual, `NewObject`), for int, long, object, float and double arguments, through both `...` and `va_list` |
| `winapk-linker-test` | A four-library tree (root, a `DT_NEEDED` cycle between two libraries, and a plugin opened at runtime) loads through the linker: constructors run dependencies first, symbols resolve across the cycle, `dlsym` on a library finds its dependencies' symbols, the registry tells guest from host addresses, and the guest's `dlopen`, `dlsym`, `dlerror` and `dladdr` behave like bionic's |
| `winapk-jvm-run` | With a real HotSpot JVM: a library loaded through the linker runs its `JNI_OnLoad`, which registers `nativeMix` through the guest `RegisterNatives`; `nativeAdd` is handed to HotSpot as a raw guest address and the `NativeMethodBind` hook has to replace it. Both are called from Java, and `nativeAdd` calls back into Java through a variadic `CallStaticIntMethod`. `nativeAdd(3, 4)` must return 19 |
| Headless boot (CI) | `winapk-runtime` boots the F-Droid Sudoku APK on `windows-latest`: DEX conversion, `Application.onCreate`, the first activity resumed, and 30 frames presented. The log and the last frame are a CI artifact |

### Not there yet

A Java-only app boots and draws, but there is no window yet: frames only go
to a file. Input, audio, SQLite and most other host natives are stubs. The
libc shim covers about twenty functions plus libdl, and HotSpot cannot yet
find a guest library through `System.loadLibrary` (see the stub DLL note in
section 6).

## 5. Technical findings

These were measured or discovered during the work. They are written down so
nobody has to find them again.

### Loader and ABI

- The Android x86_64 stack protector loads its guard with
  `mov rax, qword ptr fs:[0x28]` (`64 48 8b 04 25 28 00 00 00`). 64-bit
  Windows gives user mode no way to set the FS base. Replacing the FS prefix
  (0x64) with the GS prefix (0x65) keeps the instruction length and makes the
  load read `NT_TIB.ArbitraryUserPointer` (TEB offset 0x28). The value only has
  to stay constant between a function's prologue and epilogue on one thread.
  This was verified on real Windows, including threads where the slot was never
  written. On the test library, the 28 patched sites matched the count reported
  by `objdump` exactly.
- x64 `NT_TIB` layout: 0x00 ExceptionList, 0x08 StackBase, 0x10 StackLimit,
  0x18 SubSystemTib, 0x20 FiberData, 0x28 ArbitraryUserPointer, 0x30 Self.
- lld emits Android APS2 relocations with `--pack-dyn-relocs=android+relr`.
  With `--use-android-relr-tags` it writes `DT_ANDROID_RELR` (0x6fffe000)
  instead of `DT_RELR`. The loader handles both. APS2 decoding mirrors bionic's
  `packed_reloc_iterator`.
- IRELATIVE resolvers are guest code. They can only run after the text segment
  is executable, and must run before RELRO is sealed.
- Test libraries can be built without the NDK:
  `clang --target=x86_64-linux-android24 -nostdlib -shared -fuse-ld=lld`, with
  libc functions declared by hand. LLVM evaluates simple constructors at compile
  time and deletes them, so a test that depends on `DT_INIT_ARRAY` needs a
  constructor that calls an external function.

### Toolchain

- clang rejects `va_start` in a `sysv_abi` function on Windows targets
  (`'va_start' used in System V ABI function`). Variadic entry points that
  guest code calls need a machine-code stub that builds the `va_list` block
  itself (`make_sysv_variadic_entry`).
- `std::call_once` does not link with `-static` on the mingw-w64 setup used
  here (undefined `std::__once_callable`). One-time initialization at startup
  uses a plain flag instead.
- A template parameter pack followed by more parameters is a non-deduced
  context. To deduce from a field of type `R(JNIEnv*, Args..., const jvalue*)`,
  the template deduces the whole tail as `Args...` and splits off the last
  element with an index sequence (`VarBridge` in `src/jni_variadic.cpp`).

### JNI

- JDK 21's `JNINativeInterface_` has 235 slots: 4 reserved, 169 with fixed
  arguments, 31 C-variadic and 31 taking a `va_list`. The table only ever grows
  at the end across JDK versions.
- The Windows `jni_md.h` defines `JNICALL` as `__stdcall` (ignored on x64),
  `jint` as `int` and `jlong` as `long long`.
- Guest code compiled against the JDK's `jni.h` needs an Android `jni_md.h`
  (empty `JNICALL`, visibility-default `JNIEXPORT`) and a stub `stdio.h`,
  because `jni.h` includes `<stdio.h>` and there is no Android sysroot without
  the NDK. `test/build_e2e.sh` sets this up.
- Variadic JNI arguments follow C promotion rules. `boolean`, `byte`, `char`
  and `short` arrive as `int` in an integer register slot. `float` arrives as
  `double` in an SSE slot.
- JNI has no call that returns a method's signature from a `jmethodID`. JVMTI
  `GetMethodName` does. Its name and generic-signature outputs may be null
  when only the signature is wanted.
- JVMTI `NativeMethodBind` also fires for `RegisterNatives`, with the address
  being registered. The hook therefore has to recognize addresses that are
  already thunks; checking "is this inside a guest image" does that for free.
- A raw System V guest address bound in HotSpot crashes on the first call
  (`EXCEPTION_ACCESS_VIOLATION` in the guest), so a binding path that skips
  the thunk fails loudly rather than returning wrong values.

### Linker

- Bionic's dlopen flag values on LP64: `RTLD_LAZY` 1, `RTLD_NOW` 2,
  `RTLD_NOLOAD` 4, `RTLD_GLOBAL` 0x100, `RTLD_NODELETE` 0x1000;
  `RTLD_DEFAULT` is 0 and `RTLD_NEXT` is -1. `Dl_info` has the glibc layout.
- When a library is linked, bionic searches the global group (the
  executable's own dependencies, which include libc, then `RTLD_GLOBAL`
  libraries) before the local group (the dlopen'ed root and its tree,
  breadth-first). `dlsym` on a handle searches that handle's tree only.
- Bionic's `dladdr` only names a symbol whose `[value, value + size)` covers
  the address. A symbol of size 0 is never reported.
- Guest paths are UTF-8. On Windows a `std::filesystem::path` built from a
  `std::string` is read in the ANSI code page, so the linker uses `u8path`.
  This matters for user names outside ASCII, which end up in
  `%LOCALAPPDATA%`.

### JVM hosting

- A Windows HotSpot (Temurin 21) runs in-process under Wine.
  `JNI_CreateJavaVM`, `FindClass`, the `CallStatic*` functions and
  `RegisterNatives` all work. Wine prints harmless
  `Attempt to get process group affinity failed` warnings.
- `winapk-jvm-run` loads `jvm.dll` from `PATH`. CI puts `$JAVA_HOME/bin/server`
  and `$JAVA_HOME/bin` in front. `winapk-runtime` instead calls
  `SetDllDirectory` on the JDK's `bin` (jvm.dll needs the VC runtime there)
  and loads `bin\server\jvm.dll` by full path.
- HotSpot reads JVM option strings in the ANSI code page, so a classpath under
  a user name in, say, Hebrew would break. The host passes such paths in their
  8.3 short form, which is ASCII.
- Under Wine 9, `java.net.NetworkInterface.getAll` dies with `0xc06d007f` (a
  delay-loaded function Wine lacks). The JDK's default `SecureRandom` seeds
  itself through it, so the first `Files.createTempFile` kills the JVM. Real
  Windows is fine. `test/run_app_wine.sh` puts the SunMSCAPI provider first,
  which avoids the path.

### Upstream's Java runtime on Windows

- `runtime/java` (3,000 files) compiles unchanged with a stock JDK 21 on Linux
  and Windows in under 30 seconds. The DEX converter (`com.macapk.dex.Main`)
  also runs unchanged: the Sudoku APK converts 7,723 classes in about 5 s.
- `Boot.run` reaches `Activity.onResume` with almost every host native
  stubbed. What it really needs from the host before that: `NativeLog.print`,
  `NativeCocoa.init` returning true, the phone size, and a renderer tick for
  anything to be drawn.
- The renderer contract (`cocoa_bridge.mm`): once a window registers a
  `MetalSurfaceCallbacks` with `setRenderer`, the host calls
  `macapkSurfaceChanged(w, h)` once with the size in pixels (phone points
  times `DisplayMetrics.macapkDeviceDensity()`), then `macapkDrawFrame()` every
  vsync. Frames come back through `presentFrame` (the default) or
  `presentBitmap`.
- The platform resource APK is required: without it every `@android:`
  reference fails. Upstream's `scripts/build_framework_res.sh` builds it on
  Linux and Windows unchanged given `aapt2` 9.x; 8.x rejects
  `--feature-flags`. Without apktool's table as a reference, internal
  resource ids are assigned by `aapt2` rather than matching upstream's.
- The runtime finds jars and the resource APK relative to each other
  (`<lib>/../bin/tools/framework-res.apk`), and the Android file system under
  the `macapk.android.root` property, which the host points at
  `%LOCALAPPDATA%\Win.apk\android`.
- Upstream gap, not Windows-specific: `Resources.getIntArray` on an
  `<array>` whose items are color references returns an empty array (the
  Sudoku tutorial's dot colors). Worth reporting upstream.

### GitHub

- On a new fork, Actions stay disabled until they are enabled in the Actions
  tab.
- Workflows triggered by `pull_request_target` run the workflow file from the
  base branch. A fix to such a workflow takes effect only after it is merged.

## 6. Upstream behavior the port has to preserve

This comes from a read-through of upstream's library-loading and JNI code.
Classes and functions are named rather than line numbers, so the references
stay valid after upstream merges.

### Library loading and native binding

- An app's `System.loadLibrary` goes through stock JDK code to
  `com.macapk.loader.AppClassLoader.findLibrary`. That method is the single
  funnel for native libraries. It returns null when the APK does not ship the
  library, so the app gets `UnsatisfiedLinkError` just as on Android.
- HotSpot loads whatever `findLibrary` returns and calls `JNI_OnLoad` itself,
  passing its own `JavaVM`. On Windows HotSpot calls `LoadLibraryW`, so
  `findLibrary` has to return a real DLL. The plan is one small stub DLL per
  guest library, each at its own path (HotSpot keys loaded libraries by path,
  and a second load of the same path skips `JNI_OnLoad`). The stub's exported
  `JNI_OnLoad` loads the ELF image and calls the guest `JNI_OnLoad` with the
  guest `JavaVM`. Because this happens inside HotSpot's own library-load call,
  the class context for `FindClass` stays correct.
- HotSpot finds both `JNI_OnLoad` and lazily bound `Java_*` natives with
  `GetProcAddress` on that DLL (`ClassLoader.findNative` ->
  `NativeLibraries.findEntry0` -> `os::dll_lookup`). A fixed stub that only
  exports `JNI_OnLoad` is therefore not enough: name-bound natives would never
  be found. Either the stub is generated per library at install time with one
  export per guest `Java_*` symbol (each a tiny forwarder whose target is
  filled in at load time; `NativeMethodBind` then swaps in the real thunk), or
  the stub's `JNI_OnLoad` registers every `Java_*` export eagerly, which needs
  each method's signature from reflection and can run class initializers
  earlier than Android does. The generated export table looks like the safer
  choice and is the open design question for the next step.
- `Java_*` natives are bound lazily. HotSpot looks them up through
  `ClassLoader.findNative`, which only searches libraries registered with the
  declaring class's loader. Upstream registers libraries with the app class
  loader (`NativeLoaderShim`) and orders the search by path, like ART
  (`dalvik.system.ArtJniLibraryOrder`, which needs
  `--add-opens java.base/jdk.internal.loader`).
- The signature of a name-bound `Java_*` native is only known in the JVMTI
  `NativeMethodBind` callback (upstream: `macapk_NativeMethodBind` in
  `bionic_shim.cpp`). The Windows port should always replace the bound address
  there with a `make_jni_native_thunk` built from that signature.
- Guest `RegisterNatives` has to wrap every guest function pointer in a thunk
  before calling HotSpot's `RegisterNatives`. `NativeMethodBind` then has to
  skip addresses that are not guest code, which needs a registry of loaded
  images with an address lookup.

### JNI semantics where ART and HotSpot differ

- `FindClass` tries the app class loader without initializing the class, then
  the `JNI_OnLoad` context loader without initializing, then HotSpot's
  `FindClass`, then the app loader again. HotSpot's `FindClass` runs static
  initializers and ART's does not; upstream hit a real re-entrancy bug because
  of that difference.
- `Get*ID` with a null class returns null. A call with a null receiver
  returns 0.
- `Delete*Ref` on a detached thread is a no-op.
- `DT_NEEDED` dependencies: Android runs only their constructors, not their
  `JNI_OnLoad`. Upstream loads them through `System.load`, so their
  `JNI_OnLoad` does run. The Windows loader should follow Android.
- A dependency's `Java_*` symbols are found through the library that depends
  on it, because bionic's `dlsym` walks the `DT_NEEDED` tree.
- Guest `dlopen` and `NativeActivity` never run `JNI_OnLoad`. A guest
  `dlsym("JNI_OnLoad")` or `dlsym("Java_...")` must return the raw System V
  address, since some engines call another library's `JNI_OnLoad` directly
  (Unity's `libmain` does this with `libunity`).
- `NativeActivity` resolves its library through `findLibrary` and then calls
  `ANativeActivity_onCreate`. `ANativeActivity.vm` and `.env` must be the guest
  wrappers.
- Native threads created by guest code are not attached to the JVM
  automatically. Guest code attaches them itself.

### JVM startup

- Upstream's order (`runtime/src/main.cpp`, `jvm_loader.cpp`): create the JVM,
  register host natives, initialize the shim with the `JavaVM`, install
  `NativeMethodBind`, and only then allow the first `findLibrary`.
- Upstream's JVM options include SerialGC, `-Xss16m`, bytecode verification
  off, `--patch-module java.base` and
  `--add-opens java.base/jdk.internal.loader`.
- Differences on Windows: the classpath separator is `;`, not `:`. `-Xss` does
  not apply to the thread that creates the VM, so the VM should be created on a
  thread with a large stack. Any vectored exception handler the port installs
  must pass on exceptions it does not own, so HotSpot's own handling keeps
  working. `System.mapLibraryName("x")` returns `x.dll`, which matters for
  apps that use SoLoader.

### Not yet confirmed

These came up once and should be checked when the related code is written:
whether an unsupported version returned by `JNI_OnLoad` should be replaced
with JNI 1.6, and whether upstream's `FileDescriptor` descriptor-to-fd alias
applies on Windows, where `FileDescriptor` holds a `handle`.

## 7. Known gaps and risks

### Loader and shim

- ELF TLS (`PT_TLS` and TLS relocations) is refused. `%fs:0` (`__get_tls`) and
  other FS-relative accesses are not rewritten. This needs a decoder-driven
  pass (Zydis, MIT license). The current scanner only matches the exact
  stack-guard encodings and could also match data bytes.
- `dlclose` never unloads; libraries stay mapped until exit, as if loaded
  with `RTLD_NODELETE`. Linker namespaces and `android_dlopen_ext`'s
  `extinfo` (loading from a file descriptor or straight from the APK) are
  ignored.
- A library's references to symbols it defines itself are bound directly, so
  another library cannot interpose them. Bionic allows that unless the library
  was linked with `-Bsymbolic`.
- Constructors run with the linker lock held. Bionic does the same, so a
  constructor that waits on a thread calling `dlopen` deadlocks on both.
- Variadic libc functions (the `printf` family, `__android_log_print`) are not
  bridged yet.
- There are no signals. Mapping Windows exceptions to an Android `ucontext_t`
  has to account for the System V red zone, because Windows does not preserve
  the 128 bytes below rsp when it dispatches an exception.
- Android code does not probe the stack, so guest threads need fully committed
  stacks.
- `errno` values are Linux numbers. Every shim function that sets `errno` has
  to translate.

### Thunks and JNI

- The thunk pool is mapped read-write-execute. It should move to a dual-mapped
  section so no page is writable and executable at once.
- Thunks have no unwind information. They should be registered with
  `RtlAddFunctionTable` so stack walks and crash dumps work through them.
- Guest `JNIEnv` forwarders take the host env from the guest env they were
  called through. Upstream's ARM32 path takes it from the calling thread
  instead, which tolerates engines that cache one env for all threads. Switch
  if such an app shows up.
- A thread's guest env is not cleaned up when the thread exits. This needs a
  fiber-local-storage callback.

### Product

- Many APKs ship only ARM libraries. Users will need x86_64 variants, which
  APK mirrors often publish, and the installer has to pick the x86_64 split out
  of `.apkm`, `.xapk` and `.apks` bundles.
- Windows Defender and SmartScreen may flag a process that writes and then runs
  code. Builds need code signing and testing against Defender.
- Upstream commits almost daily. Merging at least monthly keeps conflicts
  small.

## 8. Roadmap to production

Production here means a signed installer; Android apps installed as Windows
apps, each with a Start menu entry, its own window and a taskbar icon; the
Java-only and x86_64 apps on a published compatibility list working; logs that
users can attach to bug reports; and a source offer for every release, as the
AGPL requires.

The milestones are in the recommended order. Each has deliverables and an exit
criterion. The durations are rough estimates for one experienced developer and
will move as the work reveals more.

### M1. Windows host runtime (2 to 4 weeks)

Status: the exit criterion is met in CI (the Sudoku APK boots headless and
draws). Left over: the host-platform interface for the macOS tool calls in the
Java code (they fail harmlessly for now), and real `NativeOs` and
`NativeLooper` implementations, which move to M3 with the other services.

- Port `runtime/src/main.cpp` and `jvm_loader.cpp` into a Windows host
  executable that loads `jvm.dll`, creates the VM with upstream's options
  (adjusted per section 6), and runs upstream's boot code with an APK.
- Build the framework jars on Windows. Upstream's `runtime/scripts/build.sh`
  assumes macOS, so this needs either a portable script or a CMake/Java step.
- Add stub implementations of every host native class (`NativeCocoa`,
  `NativeAudio`, `NativeMedia`, `NativeOs` and the rest) so boot can run
  without a window.
- Put app data and caches under `%LOCALAPPDATA%\Win.apk`.
- Hide the macOS tool calls in the Java code behind one host-platform
  interface, with a Windows implementation.

Exit: a Java-only APK boots through `Application.onCreate` and
`Activity.onResume` without a window, with logs.

### M2. Window, input and rendering (4 to 8 weeks)

- Implement the 54 natives of `NativeCocoa` on Win32: window creation, DPI
  scaling, resizing and orientation, mouse, keyboard, touchpad and touch input,
  the back button, dark mode, vsync (DXGI waitable swap chain) and the present
  paths (`presentBitmap`, `presentFrame`).
- Build ANGLE for Windows with the D3D11 backend (Vulkan optional). Port the
  relevant patches from `thirdparty/angle-patches`. Patch 0003 is
  Metal-specific and does not apply.
- Create EGL surfaces on the Win32 window.

Exit: Java-only apps from the compatibility list draw and respond to input
(for example a calculator, a sudoku game and the F-Droid client).

### M3. Audio, storage and basic services (3 to 5 weeks)

- `NativeAudio` (40 natives) on WASAPI, `NativeSqlite`, `NativeOs`, clipboard,
  the document picker (`IFileOpenDialog`), and the basic parts of
  `NativeMedia`.
- Per-app log files.

Exit: Java-only apps play sound and keep their data across restarts. A first
public preview limited to Java-only apps is possible after this milestone.

### M4. Native library integration (4 to 6 weeks)

Done:

- A registry of loaded guest images with an address lookup (`GuestLinker`).
- JVMTI `NativeMethodBind` that installs thunks for `Java_*` natives.
- A guest `RegisterNatives` slot that wraps every function pointer in a thunk.
- `DT_NEEDED` loading (constructors only) and bionic's symbol scopes.
- Guest `dlopen`, `dlsym`, `dladdr` and `dlerror` backed by the registry.
- A JVMTI signature provider with a cache.

Remaining:

- Per-library stub DLLs returned by `findLibrary`, with a host `JNI_OnLoad`
  that loads the image through the linker and calls the guest's, and an
  export per `Java_*` symbol (section 6).
- The Java side: `findLibrary` returning the stub's path.
- `FindClass` with the semantics from section 6.

Exit: an app with small, self-contained x86_64 JNI libraries works through the
normal `System.loadLibrary` path.

### M5. Bionic shim (3 to 5 months; the largest milestone)

- One file-descriptor table for files, sockets and pipes.
- File semantics: POSIX-style delete and rename of open files, path
  translation, case handling.
- Sockets on Winsock. `epoll`, `eventfd` and `timerfd` on IOCP or `WSAPoll`.
- pthread, including keys, thread-exit destructors and condition variables.
- `mmap`, `mprotect` and `madvise` on `VirtualAlloc` and friends.
- Time and clocks, `errno` translation, `dlopen` and `dlsym`.
- Signals on vectored exception handling (`sigaction`, the SIGSEGV handlers
  that Unity and Mono rely on, and chaining with HotSpot's handler).
- `fork` failing with `EAGAIN`.
- `/proc` emulation for the common reads (`self/maps`, `cpuinfo`).
- Locale, liblog including its variadic functions, libm and libz.
- ELF TLS and FS-relative rewriting with a decoder.

Upstream's `bionic_shim.cpp` is the behavioral reference. It records many
problems found in real apps as general fixes.

Exit: `libc++_shared.so` and real-world libraries, such as an x86_64 game
engine runtime, load and initialize.

### M6. Native graphics, audio and app types (6 to 10 weeks)

- Guest EGL and GLES bridged to ANGLE, using upstream's generated GLES bridge
  tables as the starting point.
- Vulkan through `VK_KHR_win32_surface`.
- `NativeActivity` and `GameActivity`.
- OpenSL ES and AAudio on WASAPI.
- Sensors, including upstream's keyboard-driven tilt control.

Exit: x86_64 games from the compatibility list are playable.

### M7. System services (6 to 10 weeks, can overlap with M5 and M6)

- WebView2 behind `android.webkit`.
- Media Foundation for video decoding and the camera, libvpx for VP8 and VP9.
- `Windows.Devices.Geolocation` for location.
- GameInput or XInput for gamepads.
- Google sign-in through the system browser.
- Accent color for Material You theming.

Exit: apps that only partly work today reach parity with Mac.apk.

### M8. Packaging and Windows integration (4 to 6 weeks)

- A control panel (WinUI 3 or WPF).
- An installer (MSIX or Inno Setup).
- Each installed app gets a folder, a small stub executable and a Start menu
  shortcut with its own AppUserModelID and icon.
- App data lives in a separate per-app directory, so removing an app's
  shortcut never deletes its saved data.
- File associations for `.apk`, `.apkm`, `.xapk` and `.apks`, picking the
  x86_64 split from bundles.
- App updates, uninstall, and an update check for the runtime itself.

Exit: a user without technical knowledge installs Win.apk and then an APK by
double-clicking it.

### M9. Hardening and release (4 to 8 weeks)

- W^X thunk pool and unwind tables (section 7).
- A crash handler that writes a minidump and the app log.
- Code signing and Defender/SmartScreen testing.
- A compatibility list and a regression corpus of open-source APKs run in CI.
- A security review. Apps run with the user's rights and without a sandbox,
  and that has to be stated clearly to users.
- A release pipeline that produces signed artifacts and the matching source
  archive.
- A fixed schedule for upstream merges.

Exit: version 1.0.

### M10. ARM-only apps (optional research, 6 to 12 months or more)

- dynarmic (0BSD license) for A64 and A32 code on x64, sharing the address
  space with the host.
- Generated host-call thunks per function signature for libc, GLES, EGL,
  Vulkan, JNI and the NDK libraries; host-to-guest callbacks.
- Code cache invalidation for guest code that generates code at runtime, and
  guest signal handling.
- QEMU and Unicorn are not options: they are GPLv2-only and cannot be combined
  with AGPLv3 code.

Exit: one ARM-only game is playable at an acceptable speed.

## 9. Development workflow

### Local setup on Linux

```sh
sudo apt install clang lld cmake ninja-build mingw-w64 wine64
```

You also need a Windows JDK (for example the Temurin 21 zip) for `jvm.dll`,
`jni.h` and `jvmti.h`, and any JDK 21 or later on the host for `javac`.

- Loader tests: `windows/test/run_poc_wine.sh`
- JNI targets: configure with
  `-DCMAKE_TOOLCHAIN_FILE=windows/cmake/clang-mingw-x86_64.cmake` and
  `-DWINAPK_JNI_INCLUDE=<windows-jdk>/include`, then run
  `winapk-thunk-test.exe`, `winapk-jni-test.exe` and
  `winapk-jni-variadic-test.exe` under Wine.
- Real-JVM test:
  `WINAPK_WIN_JDK=<windows-jdk> windows/test/run_e2e_wine.sh`
- Boot an APK headless:
  `WINAPK_WIN_JDK=<windows-jdk> windows/test/run_app_wine.sh app.apk`
  (also needs `curl` and `unzip` for the first resource build)

### Local setup on Windows

Install MSYS2, open the CLANG64 shell, run
`pacman -S mingw-w64-clang-x86_64-{clang,cmake,ninja}`, and install a JDK.
Build with CMake as described in `windows/README.md`.

### CI

`.github/workflows/windows-poc.yml` has two jobs. The Ubuntu job builds the
guest test libraries and the end-to-end inputs. The Windows job builds
everything with MSYS2 CLANG64 and Temurin 21, runs every test on
`windows-latest`, then stages the runtime and boots an APK headless. The
platform resource APK is cached between runs.

### Pull requests

One topic per pull request, CI green on Windows before merging, merged with
rebase. Commit messages are in English and explain why a change was made.

### Upstream sync

```sh
git remote add upstream https://github.com/kksimp/Mac.apk.git   # once
git fetch upstream
git merge upstream/main
```

Keep the list of changed upstream files in `windows/README.md` up to date.

### Code conventions

These follow upstream's `CONTRIBUTING.md`:

- Behave like Android, and check AOSP when in doubt.
- No app-specific code.
- Comments explain why. For ABI work, show the two layouts side by side.
- Prefer dependencies under BSD, MIT, Apache-2.0 or similar licenses. Upstream
  requires this so it can also sell commercial licenses. The fork only has to
  follow it if changes are ever meant to go back upstream.

### Licensing

The code is AGPL-3.0. Anyone who distributes a build, or offers a modified
version over a network, has to provide the complete corresponding source. The
Mac.apk name and logo belong to the upstream project, and Win.apk must not
present itself as official. Upstream's CLA only covers contributions sent to
upstream.

## 10. Reference

### Calling conventions

| | System V AMD64 (Android guest) | Microsoft x64 (Windows host) |
|---|---|---|
| Integer arguments | rdi, rsi, rdx, rcx, r8, r9 | rcx, rdx, r8, r9 |
| Floating-point arguments | xmm0 to xmm7, numbered separately | xmm0 to xmm3, sharing positions with the integer arguments |
| Shadow space | none | 32 bytes above the return address |
| Callee-saved registers | rbx, rbp, r12 to r15 | rbx, rbp, rdi, rsi, r12 to r15, xmm6 to xmm15 |
| Red zone | 128 bytes below rsp | none |
| `va_list` | 24-byte struct (gp_offset, fp_offset, overflow area, register save area) | `char*` |
| Return value | rax or xmm0 | rax or xmm0 |

### Terms

- Guest: Android code loaded from an APK (here, x86_64 `.so` files).
- Host: the Windows process, the JVM and the runtime around them.
- Shim: host functions that implement Android's libc (Bionic) for guest code.
- Thunk: generated code that converts a call from one calling convention to
  the other.
- Slot: one entry in the JNI function table.
- A-form: the JNI functions that take arguments as a `jvalue` array, such as
  `CallIntMethodA`.
