#include <jni.h>
extern "C" __declspec(dllimport) int j2d_chain_step(int);
extern "C" JNIEXPORT jint JNICALL Java_xh_xianhhh_test_nativebridge_ChainNative_stress(JNIEnv*, jclass, jint rounds) {
    int value = 1;
    for (int index = 0; index < rounds; ++index) value = j2d_chain_step(value);
    return value;
}
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_8; }
