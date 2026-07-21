#include <jni.h>

extern "C" __declspec(dllimport) int j2d_fixture_scale(int);

extern "C" JNIEXPORT jint JNICALL
Java_xh_xianhhh_test_nativebridge_DependentNative_scale(JNIEnv*, jclass, jint value) {
    return j2d_fixture_scale(value);
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_8; }
