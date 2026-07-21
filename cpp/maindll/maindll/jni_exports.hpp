#pragma once

#include <string>
#include <string_view>

namespace j2d::jni {

inline std::string escape(std::string_view value) {
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) result += static_cast<char>(c);
        else if (c == '_') result += "_1";
        else if (c == '.' || c == '/') result += '_';
        else if (c == ';') result += "_2";
        else if (c == '[') result += "_3";
        else { constexpr char hex[] = "0123456789abcdef"; const unsigned int code = c; result += "_0"; result += hex[(code >> 12) & 15]; result += hex[(code >> 8) & 15]; result += hex[(code >> 4) & 15]; result += hex[code & 15]; }
    }
    return result;
}

inline std::string shortExportName(std::string_view className, std::string_view methodName) {
    return "Java_" + escape(className) + "_" + escape(methodName);
}

inline std::string longExportName(std::string_view className, std::string_view methodName, std::string_view descriptor) {
    const std::size_t begin = descriptor.find('(');
    const std::size_t end = descriptor.find(')', begin == std::string_view::npos ? 0 : begin);
    if (begin == std::string_view::npos || end == std::string_view::npos) return {};
    return shortExportName(className, methodName) + "__" + escape(descriptor.substr(begin + 1, end - begin - 1));
}

} // namespace j2d::jni
