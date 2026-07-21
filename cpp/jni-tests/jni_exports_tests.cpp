#include "../maindll/maindll/jni_exports.hpp"
#include <iostream>

int main() {
    using namespace j2d::jni;
    if (shortExportName("xh.xianhhh.test.nativebridge.BasicNative", "add") != "Java_xh_xianhhh_test_nativebridge_BasicNative_add") return 1;
    if (longExportName("xh.xianhhh.test.nativebridge.OverloadNative", "describe", "(I)Ljava/lang/String;") != "Java_xh_xianhhh_test_nativebridge_OverloadNative_describe__I") return 2;
    if (longExportName("xh.xianhhh.test.nativebridge.OverloadNative", "describe", "(Ljava/lang/String;)Ljava/lang/String;") != "Java_xh_xianhhh_test_nativebridge_OverloadNative_describe__Ljava_lang_String_2") return 3;
    std::cout << "J2D_JNI_EXPORT_NAMES_OK\n";
}
