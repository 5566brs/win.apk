// WinApkJniTest -- the Java side of the real-JVM end-to-end test.
//
// A real HotSpot JVM binds nativeAdd to a native method inside an Android
// x86_64 .so, loaded and bridged by Win.apk. The native implementation adds its
// two arguments and also calls back into Java (javaMultiply) through the guest
// JNIEnv, so one call exercises host->guest (the native-method thunk), the
// guest JNIEnv wrapper, and guest->host (a variadic CallStaticIntMethod).
public class WinApkJniTest {
    // Implemented in libwinapk_jni_guest.so (Java_WinApkJniTest_nativeAdd).
    public static native int nativeAdd(int a, int b);

    // Registered by the library's JNI_OnLoad through RegisterNatives.
    public static native double nativeMix(int a, double x, long b, float y);

    // Called back from the native method through the guest JNIEnv.
    public static int javaMultiply(int a, int b) {
        return a * b;
    }
}
