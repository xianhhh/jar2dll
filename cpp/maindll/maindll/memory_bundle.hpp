#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace j2d::memory {

struct BundleModuleView final { std::string name; const unsigned char* data = nullptr; std::size_t size = 0; };

inline std::uint32_t readU32(const unsigned char* value) noexcept {
    return static_cast<std::uint32_t>(value[0]) | (static_cast<std::uint32_t>(value[1]) << 8U)
        | (static_cast<std::uint32_t>(value[2]) << 16U) | (static_cast<std::uint32_t>(value[3]) << 24U);
}

inline bool isValidModuleName(std::string_view name) noexcept {
    if (name.empty()) return false;
    for (unsigned char character : name) {
        if (character < 0x20 || character == ':' || character == '/' || character == '\\') return false;
    }
    return true;
}

inline bool equalModuleName(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) != std::tolower(static_cast<unsigned char>(right[index]))) return false;
    }
    return true;
}

inline bool parseNativeBundle(const unsigned char* bytes, std::size_t length, std::vector<BundleModuleView>& modules, std::string& error) {
    modules.clear();
    if (bytes == nullptr || length < 9 || std::memcmp(bytes, "N2DM\x01", 5) != 0) { error = "invalid native bundle header"; return false; }
    std::size_t position = 5;
    const std::uint32_t count = readU32(bytes + position); position += 4;
    if (count > (length - position) / 8) { error = "native bundle entry count exceeds data"; return false; }
    modules.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (length - position < 4) { error = "native bundle name length is missing"; return false; }
        const std::uint32_t nameLength = readU32(bytes + position); position += 4;
        if (nameLength == 0 || nameLength > length - position) { error = "native bundle name is invalid"; return false; }
        std::string name(reinterpret_cast<const char*>(bytes + position), nameLength); position += nameLength;
        if (!isValidModuleName(name)) { error = "native bundle module name is unsafe"; return false; }
        for (const auto& module : modules) {
            if (equalModuleName(module.name, name)) { error = "native bundle has duplicate module name"; return false; }
        }
        if (length - position < 4) { error = "native bundle payload length is missing"; return false; }
        const std::uint32_t dataLength = readU32(bytes + position); position += 4;
        if (dataLength == 0 || dataLength > length - position) { error = "native bundle payload exceeds data"; return false; }
        modules.push_back({std::move(name), bytes + position, dataLength}); position += dataLength;
    }
    if (position != length) { error = "native bundle has trailing data"; return false; }
    error.clear(); return true;
}

} // namespace j2d::memory
