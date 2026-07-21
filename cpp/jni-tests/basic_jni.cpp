#include <jni.h>

extern "C" JNIEXPORT jint JNICALL
Java_xh_xianhhh_test_nativebridge_BasicNative_add(JNIEnv*, jclass, jint left, jint right) {
    return left + right;
}

extern "C" JNIEXPORT jstring JNICALL
Java_xh_xianhhh_test_nativebridge_BasicNative_identity(JNIEnv* env, jclass) {
    return env->NewStringUTF("basic-native-ok");
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    return JNI_VERSION_1_8;
}
