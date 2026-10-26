// host_natives.h -- the native methods of upstream's Java runtime, on Windows.
//
// Upstream's Java code calls about a thousand native methods in 39 classes
// (NativeCocoa, NativeAudio, NativeOs, GLES20, ...). On the Mac they live in
// runtime/src. The Windows host implements them a class at a time; until a
// method has a Windows implementation, it is bound to a stub that returns
// 0 / false / null and reports its name the first time it is called. That
// keeps the boot going as far as it can, and the log names the next native to
// write instead of stopping at an UnsatisfiedLinkError.

#pragma once

#include <cstddef>
#include <string>

#include <jni.h>
#include <jvmti.h>

namespace winapk::host {

// One Windows implementation of a host native method.
struct HostMethod {
    const char* clazz;  // binary name, "com.macapk.loader.NativeLog"
    JNINativeMethod method;
};

struct HostMethods {
    const HostMethod* methods;
    size_t count;
};

#define WINAPK_HOST_NATIVE(clazz, name, sig, fn) \
    {clazz, {const_cast<char*>(name), const_cast<char*>(sig), reinterpret_cast<void*>(&fn)}}

// The implementations, one file per Java class.
HostMethods native_log_methods();    // native_log.cpp
HostMethods native_cocoa_methods();  // native_cocoa.cpp (headless for now)
HostMethods native_linker_methods(); // guest_libs.cpp
HostMethods native_os_methods();     // native_os.cpp
HostMethods native_image_methods();  // native_image.cpp
HostMethods native_looper_methods(); // native_looper.cpp
HostMethods native_audio_methods();  // native_audio.cpp
HostMethods native_clipboard_methods(); // native_clipboard.cpp
HostMethods native_wallpaper_methods(); // native_wallpaper.cpp
HostMethods native_picker_methods();    // native_picker.cpp
HostMethods native_gamepad_methods();   // native_gamepad.cpp
HostMethods native_icu_methods();       // native_icu.cpp
HostMethods native_audio_input_methods(); // native_audio_input.cpp
HostMethods native_photos_methods();    // native_photos.cpp
HostMethods native_gl_methods();        // gl/gl_bridges.cpp
HostMethods native_browser_tab_methods(); // native_browser_tab.cpp
HostMethods native_webview_methods();   // native_webview.cpp

// The app's directory under the data directory, where Boot renders the
// launcher icon (icon.png) that becomes the window's icon. native_cocoa.cpp.
void set_app_dir(const std::wstring& dir);

// A URL the host hands to the app (NativeCocoa.takeHostUrls): queued, then
// HostUrlIntake.macapkOnHostUrl is called on `env`'s thread so the intake
// drains and delivers it. prepare_host_url_queue resolves that method; call
// it once on a Java thread first. native_cocoa.cpp.
void prepare_host_url_queue(JNIEnv* env);
void queue_host_url(JNIEnv* env, const std::wstring& url);

// The phone the app sees now, in dp (turned with the app). native_cocoa.cpp.
void phone_size_dp(int* w, int* h);

// The phone the app starts with, in dp: WINAPK_PHONE_SIZE, else 360x800.
// native_cocoa.cpp.
void launch_phone_size(int* w, int* h);

struct NativeStats {
    int classes = 0;        // host classes found
    int implemented = 0;    // methods bound to a Windows implementation
    int stubbed = 0;        // methods bound to a stub
};

// Binds every native method of the host classes. `jvmti` is used to read each
// method's signature. Classes that are missing from the classpath are skipped
// with a warning. Returns false only if the JNI calls themselves fail.
bool register_host_natives(JNIEnv* env, jvmtiEnv* jvmti, NativeStats* stats);

}  // namespace winapk::host
