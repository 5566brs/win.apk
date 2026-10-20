// jni_guest.c -- the Android x86_64 JNI library for the end-to-end test.
//
// Built for x86_64-linux-android with a real jni.h (see android_jni_md.h), so
// it uses the JNIEnv exactly as a real Android app would: the C form, where
// (*env)->Fn(env, ...) calls through the function table with the System V ABI.
// Win.apk hands it a guest JNIEnv whose slots forward into HotSpot.
//
// Java_WinApkJniTest_nativeAdd(env, clazz, a, b) returns (a + b) plus the
// result of calling WinApkJniTest.javaMultiply(a, b) back through JNI -- so it
// drives the native-method thunk (host->guest), the guest JNIEnv, and a
// variadic CallStaticIntMethod (guest->host).

#include <jni.h>

JNIEXPORT jint JNICALL Java_WinApkJniTest_nativeAdd(JNIEnv* env, jclass clazz, jint a, jint b) {
    jint sum = a + b;
    jmethodID mul = (*env)->GetStaticMethodID(env, clazz, "javaMultiply", "(II)I");
    if (!mul) return sum;  // exception pending; let Java see it
    jint product = (*env)->CallStaticIntMethod(env, clazz, mul, a, b);
    return sum + product;
}
