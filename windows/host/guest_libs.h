// guest_libs.h -- an app's own x86_64 native libraries, loaded into the runtime.
//
// Upstream's class loader hands each lib<name>.so of the app to a translator
// that writes a host library the JVM can System.load(); on the Mac that is a
// Mach-O wrapper (runtime/java/com/macapk/loader/AppClassLoader.java,
// findLibrary). The Windows translator (host/translator_main.cpp) writes a copy
// of one small stub DLL (host/guest_stub.cpp) with a sidecar file naming the
// .so. Each copy is a distinct module, so the JVM keeps one NativeLibrary per
// app library, as on a device. From there:
//
//   * System.load() calls the stub's JNI_OnLoad, which calls
//     winapk_guest_library_loaded: the .so is loaded with its DT_NEEDED tree
//     through the GuestLinker and its own JNI_OnLoad runs with the guest
//     JavaVM, as System.loadLibrary does on Android.
//   * A native method found by NAME: HotSpot asks
//     jdk.internal.loader.NativeLibraries.findEntry0(handle, name) for each
//     loaded library. That native is replaced here: for a stub module it is
//     dlsym on the guest library (Android's lookup), and the NativeMethodBind
//     hook (jni_bind.h) then puts a thunk built from the method's signature in
//     place of the guest address. Every other module gets the JDK's own answer.
//   * NativeLinker.nativeLinkOnly maps a dependency without its JNI_OnLoad,
//     as the dynamic linker does on Android.
//   * A system function the shim does not have yet is bound to a stub that
//     reports its name on the first call and returns 0, so a library loads and
//     the report names the next function to write (the compatibility sweep
//     ranks them).

#pragma once

#include <jni.h>
#include <jvmti.h>

#include "host_natives.h"

namespace winapk::host {

// Call once, right after the JVM is created and before any app code runs.
// Returns false if the JVM lacks something the hooks need.
bool guest_libs_init(JNIEnv* env, jvmtiEnv* jvmti);

// com.macapk.loader.NativeLinker.
HostMethods native_linker_methods();

}  // namespace winapk::host

// Called by a stub DLL's JNI_OnLoad with its own module and path. Returns the
// JNI version the guest's JNI_OnLoad returned (JNI_VERSION_1_6 if it has
// none), or JNI_ERR if the library cannot be loaded, which makes System.load
// throw UnsatisfiedLinkError as on Android.
extern "C" __declspec(dllexport) jint winapk_guest_library_loaded(JavaVM* vm, void* module,
                                                                  const wchar_t* stub_path);
