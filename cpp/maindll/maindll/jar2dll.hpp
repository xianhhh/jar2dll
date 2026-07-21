#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace j2d {

// 与 Java 侧 JarBlobBuilder 保持一致：1 表示 class，2 表示普通资源。
enum EntryType : std::uint8_t {
    TYPE_CLASS = 1,
    TYPE_RESOURCE = 2
};

/**
 * J2DF 条目的基类。
 *
 * J2DF 数据已嵌入 DLL 并在整个引导期间保持有效，因此条目只保存只读视图，
 * 不再为每个 class/resource 复制一份数据。这样大型 JAR 不会在加载时产生第二份
 * 完整内存副本；析构函数仍为 virtual，保证通过 JarEntry* 删除派生对象时行为正确。
 */
class JarEntry {
public:
    JarEntry(const std::string& name, const char* bytes, std::size_t size)
        : name_(name), data_(bytes), size_(size) {}

    virtual ~JarEntry() = default;

    const std::string& getName() const noexcept { return name_; }
    const char* getData() const noexcept { return data_; }
    std::size_t getSize() const noexcept { return size_; }
    virtual EntryType getType() const noexcept = 0;

private:
    std::string name_;
    const char* data_ = nullptr;
    std::size_t size_ = 0;
};

class ClassFile final : public JarEntry {
public:
    ClassFile(const std::string& name, const char* bytes, std::size_t size)
        : JarEntry(name, bytes, size) {
    }

    EntryType getType() const noexcept override { return TYPE_CLASS; }
};

class ResourceFile final : public JarEntry {
public:
    ResourceFile(const std::string& name, const char* bytes, std::size_t size)
        : JarEntry(name, bytes, size) {
    }

    EntryType getType() const noexcept override { return TYPE_RESOURCE; }
};

namespace detail {

inline std::uint32_t readUInt32LE(const unsigned char* bytes) noexcept {
    // 逐字节读取，避免未对齐访问，并明确匹配 Java 侧的小端格式。
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

inline void deleteEntries(std::vector<JarEntry*>& entries) noexcept {
    for (JarEntry* entry : entries) {
        delete entry;
    }
    entries.clear();
}

} // namespace detail

/**
 * 按原有 J2DF 格式解析条目。
 *
 * 函数保留原来的输出类型和条目分类逻辑，只补充完整的 magic/version/长度检查。
 * 解析失败时会清掉本次已经创建的对象，调用者不会接收到半截数据。
 */
inline bool parseBlob(
    const char* blob,
    std::size_t length,
    std::vector<JarEntry*>& out,
    std::string* error = nullptr) {
    constexpr unsigned char kMagic[] = {'J', '2', 'D', 'F'};
    constexpr unsigned char kVersion = 1;

    auto fail = [&](const std::string& message) {
        detail::deleteEntries(out);
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    detail::deleteEntries(out);
    if (blob == nullptr || length < 9) {
        return fail("J2DF header is incomplete");
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(blob);
    if (std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0) {
        return fail("J2DF magic is invalid");
    }
    if (bytes[4] != kVersion) {
        return fail("Unsupported J2DF version");
    }

    std::size_t position = 5;
    const std::uint32_t count = detail::readUInt32LE(bytes + position);
    position += 4;

    // 一个空条目也至少包含 type、nameLen、dataLen 共 9 字节。
    if (count > (length - position) / 9) {
        return fail("J2DF entry count exceeds available data");
    }

    out.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (length - position < 5) {
            return fail("J2DF entry header is incomplete");
        }

        const std::uint8_t type = bytes[position++];
        const std::uint32_t nameLength = detail::readUInt32LE(bytes + position);
        position += 4;
        if (nameLength > length - position) {
            return fail("J2DF entry name exceeds available data");
        }

        const std::string name(blob + position, nameLength);
        position += nameLength;

        if (length - position < 4) {
            return fail("J2DF entry data length is missing");
        }
        const std::uint32_t dataLength = detail::readUInt32LE(bytes + position);
        position += 4;
        if (dataLength > length - position) {
            return fail("J2DF entry data exceeds available data");
        }

        const char* data = dataLength == 0 ? nullptr : blob + position;
        if (type == TYPE_CLASS) {
            out.push_back(new ClassFile(name, data, dataLength));
        }
        else if (type == TYPE_RESOURCE) {
            out.push_back(new ResourceFile(name, data, dataLength));
        }
        else {
            return fail("J2DF contains an unknown entry type");
        }
        position += dataLength;
    }

    if (position != length) {
        return fail("J2DF contains trailing data");
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

} // namespace j2d
