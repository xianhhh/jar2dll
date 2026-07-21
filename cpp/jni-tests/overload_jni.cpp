#include <jni.h>

extern "C" JNIEXPORT jstring JNICALL
Java_xh_xianhhh_test_nativebridge_OverloadNative_describe__I(
    JNIEnv* env, jclass, jint value) {
    const char* result = value == 7 ? "int:7" : "int:other";
    return env->NewStringUTF(result);
}

extern "C" JNIEXPORT jstring JNICALL
Java_xh_xianhhh_test_nativebridge_OverloadNative_describe__Ljava_lang_String_2(
    JNIEnv* env, jclass, jstring value) {
    const char* bytes = value == nullptr ? "null" : env->GetStringUTFChars(value, nullptr);
    jstring result = env->NewStringUTF(bytes == nullptr ? "string:error" : bytes);
    if (bytes != nullptr && value != nullptr) env->ReleaseStringUTFChars(value, bytes);
    return result;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    return JNI_VERSION_1_8;
}
