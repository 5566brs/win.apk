// OnDemandStubFixture.c -- stands in for winapk-guest-stub.dll in
// OnDemandNativeLoadTest, which runs on a plain JVM: the real stub hands its
// ELF to winapk-runtime.exe's guest linker, which java.exe does not have.
// The test copies macapk-translator.exe next to this DLL, so the real
// translator makes every stub, .source sidecar included, as a copy of it.
//
// JNI_OnLoad counts the loads of ALL copies in a system property (each copy is
// its own module with its own statics), so the test can tell one module per
// ELF from two. OnDemandGuest.ping() resolves only through a library that is
// registered with the guest class's own loader. The test also loads this DLL
// itself, for com.macapk.loader.NativeLog.print, which winapk-runtime.exe
// provides in a real run: it prints each line and keeps the loader's
// stub-making lines in another property for the test to check.
//
//   clang -shared -O2 -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/win32" \
//         -o winapk-guest-stub.dll windows/test/java/OnDemandStubFixture.c

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// System.setProperty(key, System.getProperty(key, "") + line + "\n").
static void append_property(JNIEnv* env, const char* key_utf, jstring line) {
    jclass sys = (*env)->FindClass(env, "java/lang/System");
    jclass str = (*env)->FindClass(env, "java/lang/String");
    jmethodID get = (*env)->GetStaticMethodID(env, sys, "getProperty",
                                              "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID set = (*env)->GetStaticMethodID(env, sys, "setProperty",
                                              "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID concat = (*env)->GetMethodID(env, str, "concat", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!get || !set || !concat) return;
    jstring key = (*env)->NewStringUTF(env, key_utf);
    jobject old = (*env)->CallStaticObjectMethod(env, sys, get, key, (*env)->NewStringUTF(env, ""));
    jobject with = (*env)->CallObjectMethod(env, old, concat, line);
    jobject done = (*env)->CallObjectMethod(env, with, concat, (*env)->NewStringUTF(env, "\n"));
    (*env)->CallStaticObjectMethod(env, sys, set, key, done);
}

JNIEXPORT void JNICALL Java_com_macapk_loader_NativeLog_print(JNIEnv* env, jclass cls, jint level, jstring tag,
                                                              jstring msg) {
    (void)cls;
    const char* t = tag ? (*env)->GetStringUTFChars(env, tag, NULL) : NULL;
    const char* m = msg ? (*env)->GetStringUTFChars(env, msg, NULL) : NULL;
    fprintf(stderr, "[%d] %s: %s\n", (int)level, t ? t : "", m ? m : "");
    fflush(stderr);
    int keep = m && (strncmp(m, "on-demand native:", 17) == 0 || strncmp(m, "running translator:", 19) == 0);
    if (t) (*env)->ReleaseStringUTFChars(env, tag, t);
    if (m) (*env)->ReleaseStringUTFChars(env, msg, m);
    if (keep) append_property(env, "ondemand.fixture.log", msg);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass sys = (*env)->FindClass(env, "java/lang/System");
    jmethodID get = (*env)->GetStaticMethodID(env, sys, "getProperty",
                                              "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jmethodID set = (*env)->GetStaticMethodID(env, sys, "setProperty",
                                              "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (!get || !set) return JNI_ERR;
    jstring key = (*env)->NewStringUTF(env, "ondemand.fixture.loads");
    jstring old = (jstring)(*env)->CallStaticObjectMethod(env, sys, get, key, (*env)->NewStringUTF(env, "0"));
    const char* text = (*env)->GetStringUTFChars(env, old, NULL);
    char next[16];
    snprintf(next, sizeof next, "%d", atoi(text) + 1);
    (*env)->ReleaseStringUTFChars(env, old, text);
    (*env)->CallStaticObjectMethod(env, sys, set, key, (*env)->NewStringUTF(env, next));
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL Java_OnDemandGuest_ping(JNIEnv* env, jclass cls) {
    (void)env;
    (void)cls;
    return 73;
}
