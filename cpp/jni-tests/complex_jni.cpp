#include <jni.h>
#include <algorithm>
#include <cstdint>
#include <vector>

extern "C" JNIEXPORT jintArray JNICALL
Java_xh_xianhhh_test_nativebridge_ComplexNative_normalizeAndSort(JNIEnv* env, jclass, jintArray input) {
    if (input == nullptr) {
        jclass error = env->FindClass("java/lang/IllegalArgumentException");
        if (error != nullptr) env->ThrowNew(error, "input must not be null");
        return nullptr;
    }
    const jsize length = env->GetArrayLength(input);
    std::vector<jint> values(static_cast<std::size_t>(length));
    if (length > 0) env->GetIntArrayRegion(input, 0, length, values.data());
    std::sort(values.begin(), values.end());
    for (jint& value : values) value = value < 0 ? -value : value;
    std::sort(values.begin(), values.end());
    jintArray result = env->NewIntArray(length);
    if (result != nullptr && length > 0) env->SetIntArrayRegion(result, 0, length, values.data());
    return result;
}

extern "C" JNIEXPORT jlong JNICALL
Java_xh_xianhhh_test_nativebridge_ComplexNative_fnv1a64(JNIEnv* env, jclass, jbyteArray input) {
    if (input == nullptr) return 0;
    const jsize length = env->GetArrayLength(input);
    std::vector<jbyte> bytes(static_cast<std::size_t>(length));
    if (length > 0) env->GetByteArrayRegion(input, 0, length, bytes.data());
    std::uint64_t hash = 14695981039346656037ULL;
    for (jbyte value : bytes) { hash ^= static_cast<unsigned char>(value); hash *= 1099511628211ULL; }
    return static_cast<jlong>(hash);
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_8; }
