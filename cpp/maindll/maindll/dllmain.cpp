#include "pch.h"
#include "jar2dll.hpp"
#include "jni_exports.hpp"
#include "memory_bundle.hpp"
#include "memory_pe.hpp"

#include <windows.h>
#include <jni.h>
#include <jvmti.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "jvm.lib")

namespace {

namespace fs = std::filesystem;

JavaVM* g_jvm = nullptr;
jvmtiEnv* g_jvmti = nullptr;
HMODULE g_module = nullptr;

// Registered JNI function pointers remain valid only while both this DLL and
// every mapped native image stay resident. The loader thread pins this DLL for
// the process lifetime. Published memory state is immutable and read through a
// shared snapshot so ClassPrepare never observes a partially initialized registry.
std::mutex g_memoryStateMutex;
std::mutex g_nativeRegistrationMutex;
std::shared_ptr<j2d::memory::ModuleRegistry> g_memoryModules;
std::shared_ptr<const std::unordered_set<std::string>> g_memoryNativeClasses;

struct MemoryStateSnapshot {
    std::shared_ptr<j2d::memory::ModuleRegistry> modules;
    std::shared_ptr<const std::unordered_set<std::string>> nativeClasses;
};

MemoryStateSnapshot memoryStateSnapshot() {
    std::lock_guard lock(g_memoryStateMutex);
    return {g_memoryModules, g_memoryNativeClasses};
}

void publishMemoryState(
    std::shared_ptr<j2d::memory::ModuleRegistry> modules,
    std::shared_ptr<const std::unordered_set<std::string>> nativeClasses) {
    std::lock_guard lock(g_memoryStateMutex);
    g_memoryModules = std::move(modules);
    g_memoryNativeClasses = std::move(nativeClasses);
}

enum EmbeddedResourceId : int {
    RESOURCE_J2DF = 101,
    RESOURCE_MAIN_CLASS = 102,
    RESOURCE_MAIN_METHOD = 103,
    RESOURCE_MAIN_SIGNATURE = 104,
    RESOURCE_MAIN_ARGUMENT = 105,
    RESOURCE_HAS_ARGUMENT = 106,
    RESOURCE_CLASS_LOADERS = 107
};

struct ResourceView {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
    explicit operator bool() const noexcept { return data != nullptr; }
};

ResourceView getEmbeddedResource(int id) {
    const HRSRC resource = FindResourceW(g_module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (resource == nullptr) return {};
    const HGLOBAL loaded = LoadResource(g_module, resource);
    const DWORD size = SizeofResource(g_module, resource);
    const auto* data = static_cast<const unsigned char*>(LockResource(loaded));
    return data == nullptr ? ResourceView{} : ResourceView{data, size};
}

std::size_t j2dfPayloadLength(const ResourceView& resource) {
    // The payload may be followed by optional dump/run metadata.  Do not scan for a
    // magic sequence: arbitrary class or compressed resource bytes can contain one.
    // Instead, walk the self-describing J2DF container once and return its exact end.
    if (resource.size < 9 || std::memcmp(resource.data, "J2DF\x01", 5) != 0) {
        return resource.size;
    }
    const auto read32 = [](const unsigned char* value) noexcept -> std::size_t {
        return static_cast<std::size_t>(value[0])
            | (static_cast<std::size_t>(value[1]) << 8U)
            | (static_cast<std::size_t>(value[2]) << 16U)
            | (static_cast<std::size_t>(value[3]) << 24U);
    };
    std::size_t position = 5;
    const std::size_t count = read32(resource.data + position);
    position += 4;
    if (count > (resource.size - position) / 9) return resource.size;
    for (std::size_t index = 0; index < count; ++index) {
        if (resource.size - position < 5) return resource.size;
        ++position; // entry type
        const std::size_t nameLength = read32(resource.data + position);
        position += 4;
        if (nameLength > resource.size - position) return resource.size;
        position += nameLength;
        if (resource.size - position < 4) return resource.size;
        const std::size_t dataLength = read32(resource.data + position);
        position += 4;
        if (dataLength > resource.size - position) return resource.size;
        position += dataLength;
    }
    return position;
}

/**
 * JNI 局部引用的轻量 RAII 包装。
 *
 * 局部引用必须先于 DetachCurrentThread 释放。用对象生命周期管理后，无论函数从
 * 哪个错误分支 return，都不会漏掉 DeleteLocalRef。
 */
template <typename T>
class LocalRef final {
public:
    LocalRef() = default;
    LocalRef(JNIEnv* env, T value) noexcept : env_(env), value_(value) {}
    ~LocalRef() { reset(); }

    LocalRef(const LocalRef&) = delete;
    LocalRef& operator=(const LocalRef&) = delete;

    T get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

    T release() noexcept {
        T value = value_;
        value_ = nullptr;
        return value;
    }

    void reset(T value = nullptr) noexcept {
        if (env_ != nullptr && value_ != nullptr) {
            env_->DeleteLocalRef(value_);
        }
        value_ = value;
    }

private:
    JNIEnv* env_ = nullptr;
    T value_ = nullptr;
};

/**
 * JNI 全局引用包装。该对象只在 RunBootstrap 的附加线程内使用并销毁，确保
 * DeleteGlobalRef 发生在恢复上下文类加载器之后、DetachCurrentThread 之前。
 */
class GlobalRef final {
public:
    GlobalRef(JNIEnv* env, jobject local) noexcept
        : env_(env), value_(local == nullptr ? nullptr : env->NewGlobalRef(local)) {
    }

    ~GlobalRef() {
        if (env_ != nullptr && value_ != nullptr) {
            env_->DeleteGlobalRef(value_);
        }
    }

    GlobalRef(const GlobalRef&) = delete;
    GlobalRef& operator=(const GlobalRef&) = delete;

    jobject get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    JNIEnv* env_ = nullptr;
    jobject value_ = nullptr;
};

// 负责释放 parseBlob 创建的 JarEntry。它修复了“找不到 ClassLoader 时直接 return”
// 导致整个 JAR 条目集合泄漏的问题，同时不改变原来的裸指针解析接口和处理顺序。
class EntryListGuard final {
public:
    explicit EntryListGuard(std::vector<j2d::JarEntry*>& entries) noexcept : entries_(entries) {}
    ~EntryListGuard() { j2d::detail::deleteEntries(entries_); }

    EntryListGuard(const EntryListGuard&) = delete;
    EntryListGuard& operator=(const EntryListGuard&) = delete;

private:
    std::vector<j2d::JarEntry*>& entries_;
};

class AttachedThreadGuard final {
public:
    explicit AttachedThreadGuard(JavaVM* vm) noexcept : vm_(vm) {
        if (vm_ != nullptr) {
            status_ = vm_->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env_), nullptr);
        }
        if (status_ == JNI_OK) {
            attached_ = true;
        }
    }

    ~AttachedThreadGuard() {
        if (attached_) {
            vm_->DetachCurrentThread();
        }
    }

    AttachedThreadGuard(const AttachedThreadGuard&) = delete;
    AttachedThreadGuard& operator=(const AttachedThreadGuard&) = delete;

    JNIEnv* env() const noexcept { return env_; }
    jint status() const noexcept { return status_; }
    explicit operator bool() const noexcept { return attached_ && env_ != nullptr; }

private:
    JavaVM* vm_ = nullptr;
    JNIEnv* env_ = nullptr;
    jint status_ = JNI_ERR;
    bool attached_ = false;
};

void logError(const std::string& message) {
    std::cerr << "[jar2dll] " << message << std::endl;
    const std::string debuggerMessage = "[jar2dll] " + message + "\n";
    OutputDebugStringA(debuggerMessage.c_str());
}

// Java 异常挂起时不能继续执行普通 JNI 调用。报告并清除异常后，清理路径才能继续。
std::string takePendingJavaException(JNIEnv* env, bool describe = false) {
    if (env == nullptr || !env->ExceptionCheck()) return {};

    jthrowable exception = env->ExceptionOccurred();
    if (describe) env->ExceptionDescribe();
    env->ExceptionClear();
    if (exception == nullptr) return "unknown Java exception";

    std::string detail = "Java exception";
    jclass exceptionClass = env->GetObjectClass(exception);
    if (exceptionClass != nullptr && !env->ExceptionCheck()) {
        const jmethodID toString = env->GetMethodID(exceptionClass, "toString", "()Ljava/lang/String;");
        if (toString != nullptr && !env->ExceptionCheck()) {
            jstring text = static_cast<jstring>(env->CallObjectMethod(exception, toString));
            if (text != nullptr && !env->ExceptionCheck()) {
                const char* chars = env->GetStringUTFChars(text, nullptr);
                if (chars != nullptr) {
                    detail.assign(chars);
                    env->ReleaseStringUTFChars(text, chars);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            if (text != nullptr) env->DeleteLocalRef(text);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (exceptionClass != nullptr) env->DeleteLocalRef(exceptionClass);
    env->DeleteLocalRef(exception);
    return detail;
}

bool reportAndClearException(JNIEnv* env, const char* context, bool describe = true) {
    if (env == nullptr || !env->ExceptionCheck()) return false;
    const std::string detail = takePendingJavaException(env, describe);
    logError(std::string(context == nullptr ? "JNI operation" : context) + " failed: " + detail);
    return true;
}

// 获取 DLL 所在目录。保留原来“资源位于 DLL/resources”的路径规则，只支持长路径。
fs::path getDllDirectory() {
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        const DWORD length = GetModuleFileNameW(
            g_module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            break;
        }
        if (length < buffer.size() - 1) {
            return fs::path(buffer.data(), buffer.data() + length).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }

    std::error_code error;
    const fs::path current = fs::current_path(error);
    return error ? fs::path(L".") : current;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

std::wstring safePathComponent(std::wstring value) {
    for (wchar_t& character : value) {
        if (character < 32 || wcschr(L"<>:\"/\\|?*", character) != nullptr) character = L'_';
    }
    return value.empty() ? L"module" : value;
}

fs::path getRuntimeResourceDirectory() {
    std::vector<wchar_t> tempBuffer(512);
    DWORD length = GetTempPathW(static_cast<DWORD>(tempBuffer.size()), tempBuffer.data());
    if (length >= tempBuffer.size()) {
        tempBuffer.resize(static_cast<std::size_t>(length) + 1);
        length = GetTempPathW(static_cast<DWORD>(tempBuffer.size()), tempBuffer.data());
    }
    fs::path tempRoot;
    if (length > 0 && length < tempBuffer.size()) {
        tempRoot = fs::path(tempBuffer.data(), tempBuffer.data() + length);
    }
    else {
        std::error_code error;
        tempRoot = fs::temp_directory_path(error);
        if (error) tempRoot = getDllDirectory();
    }

    std::wostringstream instance;
    instance << GetCurrentProcessId() << L'-' << std::hex
        << reinterpret_cast<std::uintptr_t>(g_module);
    const std::wstring moduleName = safePathComponent(getDllDirectory().filename().wstring());
    return tempRoot / L"jar2dll" / moduleName / instance.str() / L"resources";
}

bool setJavaSystemProperty(JNIEnv* env, const char* name, const std::string& value) {
    LocalRef<jclass> systemClass(env, env->FindClass("java/lang/System"));
    if (!systemClass || reportAndClearException(env, "Resolving java.lang.System for resource properties")) return false;
    const jmethodID setProperty = env->GetStaticMethodID(
        systemClass.get(), "setProperty", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (setProperty == nullptr || reportAndClearException(env, "Resolving System.setProperty")) return false;
    LocalRef<jstring> key(env, env->NewStringUTF(name));
    LocalRef<jstring> propertyValue(env, env->NewStringUTF(value.c_str()));
    if (!key || !propertyValue || reportAndClearException(env, "Creating resource property strings")) return false;
    LocalRef<jstring> previous(
        env,
        static_cast<jstring>(env->CallStaticObjectMethod(
            systemClass.get(), setProperty, key.get(), propertyValue.get())));
    return !reportAndClearException(env, "Setting Java resource property");
}

bool publishRuntimeResourceDirectory(JNIEnv* env, const fs::path& resourceDirectory) {
    const std::string utf8Path = wideToUtf8(resourceDirectory.wstring());
    if (utf8Path.empty()) {
        logError("Could not convert runtime resource directory to UTF-8");
        return false;
    }
    SetEnvironmentVariableW(L"JAR2DLL_RESOURCE_DIR", resourceDirectory.c_str());
    const bool customProperty = setJavaSystemProperty(env, "jar2dll.resource.dir", utf8Path);
    const bool userDirectory = setJavaSystemProperty(env, "user.dir", utf8Path);
    if (customProperty) {
        std::cerr << "[jar2dll] Runtime resources: " << utf8Path << std::endl;
    }
    return customProperty && userDirectory;
}

// 保留原配置清洗行为：移除所有不可打印字符和空白字符。
std::string cleanConfig(const unsigned char* data, std::size_t length) {
    if (data == nullptr || length == static_cast<std::size_t>(-1)) {
        return {};
    }

    std::string value(reinterpret_cast<const char*>(data), length);
    value.erase(
        std::remove_if(
            value.begin(),
            value.end(),
            [](unsigned char character) {
                return !std::isprint(character) || std::isspace(character);
            }),
        value.end());
    return value;
}

/**
 * 读取 ClassLoader 候选列表。
 *
 * 构建脚本使用分号保存多个类名；同时接受逗号、竖线和换行，便于手工生成配置。
 * 单个类名内部仍沿用 cleanConfig 的清洗规则。空列表具有明确含义：不扫描其它
 * 线程，直接使用 Thread.currentThread().getContextClassLoader()。
 */
std::vector<std::string> readClassLoaderCandidates(
    const unsigned char* data,
    std::size_t length) {
    std::vector<std::string> candidates;
    if (data == nullptr || length == 0) {
        return candidates;
    }

    const std::string raw(reinterpret_cast<const char*>(data), length);
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= raw.size(); ++index) {
        const bool atEnd = index == raw.size();
        const bool delimiter = !atEnd
            && (raw[index] == ';' || raw[index] == ',' || raw[index] == '|'
                || raw[index] == '\r' || raw[index] == '\n');
        if (!atEnd && !delimiter) {
            continue;
        }

        if (index > begin) {
            const auto* tokenData = reinterpret_cast<const unsigned char*>(raw.data() + begin);
            std::string candidate = cleanConfig(tokenData, index - begin);
            if (!candidate.empty()
                && std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) {
                candidates.push_back(std::move(candidate));
            }
        }
        begin = index + 1;
    }
    return candidates;
}

std::string joinClassLoaderCandidates(const std::vector<std::string>& candidates) {
    std::string result;
    for (const std::string& candidate : candidates) {
        if (!result.empty()) {
            result += ", ";
        }
        result += candidate;
    }
    return result;
}

// 当 -cld 为空时执行该回退。返回值是当前 JNI 帧中的局部引用，由调用者负责释放。
jobject getCurrentThreadContextClassLoader(JNIEnv* env) {
    LocalRef<jclass> threadClass(env, env->FindClass("java/lang/Thread"));
    const bool classException = reportAndClearException(
        env, "Resolving Thread for current context ClassLoader");
    if (!threadClass || classException) {
        return nullptr;
    }

    const jmethodID currentThread = env->GetStaticMethodID(
        threadClass.get(), "currentThread", "()Ljava/lang/Thread;");
    const jmethodID getContextClassLoader = env->GetMethodID(
        threadClass.get(), "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    const bool methodException = reportAndClearException(
        env, "Resolving current context ClassLoader methods");
    if (currentThread == nullptr || getContextClassLoader == nullptr || methodException) {
        return nullptr;
    }

    LocalRef<jobject> thread(env, env->CallStaticObjectMethod(threadClass.get(), currentThread));
    const bool currentThreadException = reportAndClearException(env, "Thread.currentThread");
    if (!thread || currentThreadException) {
        return nullptr;
    }

    jobject loader = env->CallObjectMethod(thread.get(), getContextClassLoader);
    if (reportAndClearException(env, "Thread.getContextClassLoader")) {
        return nullptr;
    }
    return loader;
}

jobject getSystemClassLoader(JNIEnv* env) {
    LocalRef<jclass> loaderClass(env, env->FindClass("java/lang/ClassLoader"));
    if (!loaderClass || reportAndClearException(env, "FindClass(java/lang/ClassLoader)")) return nullptr;
    const jmethodID method = env->GetStaticMethodID(loaderClass.get(), "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    if (method == nullptr || reportAndClearException(env, "ClassLoader.getSystemClassLoader")) return nullptr;
    jobject loader = env->CallStaticObjectMethod(loaderClass.get(), method);
    if (reportAndClearException(env, "Calling ClassLoader.getSystemClassLoader")) return nullptr;
    return loader;
}

/**
 * 按原逻辑遍历 Thread.getAllStackTraces().keySet()，比较每个线程上下文
 * ClassLoader 的实际类名。
 *
 * 每轮使用局部引用帧，匹配成功时 PopLocalFrame(loader) 会把结果安全转移到
 * 外层帧；因此无需创建“临时全局引用再转回局部引用”。
 */
jobject findTargetClassLoader(
    JNIEnv* env,
    const std::vector<std::string>& targetLoaderClassNames) {
    LocalRef<jclass> threadClass(env, env->FindClass("java/lang/Thread"));
    LocalRef<jclass> mapClass(env, env->FindClass("java/util/Map"));
    LocalRef<jclass> setClass(env, env->FindClass("java/util/Set"));
    LocalRef<jclass> objectClass(env, env->FindClass("java/lang/Object"));
    LocalRef<jclass> classClass(env, env->FindClass("java/lang/Class"));
    const bool classLookupException = reportAndClearException(
        env, "Resolving ClassLoader lookup classes");
    if (!threadClass || !mapClass || !setClass || !objectClass || !classClass
        || classLookupException) {
        return nullptr;
    }

    const jmethodID getAllStackTraces = env->GetStaticMethodID(
        threadClass.get(), "getAllStackTraces", "()Ljava/util/Map;");
    const jmethodID keySet = env->GetMethodID(mapClass.get(), "keySet", "()Ljava/util/Set;");
    const jmethodID toArray = env->GetMethodID(setClass.get(), "toArray", "()[Ljava/lang/Object;");
    const jmethodID getContextClassLoader = env->GetMethodID(
        threadClass.get(), "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    const jmethodID getClass = env->GetMethodID(objectClass.get(), "getClass", "()Ljava/lang/Class;");
    const jmethodID getName = env->GetMethodID(classClass.get(), "getName", "()Ljava/lang/String;");
    const bool methodLookupException = reportAndClearException(
        env, "Resolving ClassLoader lookup methods");
    if (getAllStackTraces == nullptr || keySet == nullptr || toArray == nullptr
        || getContextClassLoader == nullptr || getClass == nullptr || getName == nullptr
        || methodLookupException) {
        return nullptr;
    }

    LocalRef<jobject> map(env, env->CallStaticObjectMethod(threadClass.get(), getAllStackTraces));
    LocalRef<jobject> set(env, map ? env->CallObjectMethod(map.get(), keySet) : nullptr);
    LocalRef<jobjectArray> threads(
        env,
        set ? static_cast<jobjectArray>(env->CallObjectMethod(set.get(), toArray)) : nullptr);
    const bool threadCollectionException = reportAndClearException(env, "Collecting JVM threads");
    if (!map || !set || !threads || threadCollectionException) {
        return nullptr;
    }

    const jsize length = env->GetArrayLength(threads.get());
    for (jsize index = 0; index < length; ++index) {
        if (env->ExceptionCheck()) {
            reportAndClearException(env, "Unexpected pending exception before PushLocalFrame");
            return nullptr;
        }
        if (env->PushLocalFrame(16) != JNI_OK) {
            if (!reportAndClearException(env, "PushLocalFrame while scanning ClassLoaders")) {
                logError("PushLocalFrame failed while scanning ClassLoaders without a pending Java exception");
            }
            return nullptr;
        }

        jobject thread = env->GetObjectArrayElement(threads.get(), index);
        jobject loader = thread == nullptr
            ? nullptr
            : env->CallObjectMethod(thread, getContextClassLoader);

        bool matched = false;
        const char* loaderNameChars = nullptr;
        jstring loaderName = nullptr;
        if (!reportAndClearException(env, "Reading candidate thread context ClassLoader", false) && loader != nullptr) {
            jobject loaderClass = env->CallObjectMethod(loader, getClass);
            loaderName = loaderClass == nullptr
                ? nullptr
                : static_cast<jstring>(env->CallObjectMethod(loaderClass, getName));
            if (!reportAndClearException(env, "Reading candidate ClassLoader type", false) && loaderName != nullptr) {
                loaderNameChars = env->GetStringUTFChars(loaderName, nullptr);
                if (loaderNameChars == nullptr) {
                    reportAndClearException(env, "Reading ClassLoader class name");
                }
                else {
                    matched = std::find(
                        targetLoaderClassNames.begin(),
                        targetLoaderClassNames.end(),
                        loaderNameChars) != targetLoaderClassNames.end();
                }
            }
        }

        // UTF 字符必须在弹出局部帧（会删除 loaderName）之前归还。
        if (loaderNameChars != nullptr) {
            env->ReleaseStringUTFChars(loaderName, loaderNameChars);
        }
        if (matched) {
            return env->PopLocalFrame(loader);
        }
        env->PopLocalFrame(nullptr);
    }
    return nullptr;
}

bool isSafeRelativeResourcePath(const fs::path& relative) {
    if (relative.empty() || relative.is_absolute()
        || relative.has_root_name() || relative.has_root_directory()) {
        return false;
    }
    for (const fs::path& component : relative) {
        if (component == L"..") {
            return false;
        }
    }
    return true;
}

// 清理发生在普通工作线程，而不是 DLL_PROCESS_DETACH 的 loader lock 中。
// 正常情况下，上一次运行已无进程使用这些文件；当前运行生成的资源保持原使用周期。
bool cleanupPreviousResources(const fs::path& resourceDirectory) {
    std::error_code error;
    if (fs::exists(resourceDirectory, error) && !error) {
        fs::remove_all(resourceDirectory, error);
        if (error) {
            logError("Could not clean this process's resource directory: " + error.message());
            return false;
        }
    }
    fs::create_directories(resourceDirectory, error);
    if (error) {
        logError("Could not create this process's resource directory: " + error.message());
        return false;
    }
    return true;
}

bool writeResources(const std::vector<j2d::JarEntry*>& entries, const fs::path& resourceDirectory) {
    bool success = true;
    for (j2d::JarEntry* entry : entries) {
        if (entry->getType() != j2d::TYPE_RESOURCE) {
            continue;
        }

        const std::string& entryName = entry->getName();
        if (entryName.empty()) {
            continue;
        }
        // Keep the original resource available as a JVM compatibility fallback.
        // Class initializers still execute System.load/loadLibrary after native
        // registration, so suppressing this file would make otherwise supported
        // JARs fail during <clinit>. The same DLL is also mapped from N2DM.

        // 保留项目原来的过滤规则，不改变最终会释放到磁盘的资源集合。
        if (entryName.find("pack.mcmeta") != std::string::npos
            || entryName.find("META-INF") != std::string::npos) {
            continue;
        }

        const fs::path relative = fs::path(entryName).lexically_normal();
        if (!isSafeRelativeResourcePath(relative)) {
            logError("Rejected unsafe resource path: " + entryName);
            success = false;
            continue;
        }

        const fs::path filePath = resourceDirectory / relative;
        const fs::path parentDirectory = filePath.parent_path();
        std::error_code error;
        if (!parentDirectory.empty()) {
            fs::create_directories(parentDirectory, error);
        }
        if (error) {
            logError("Could not create resource directory for " + entryName + ": " + error.message());
            success = false;
            continue;
        }

        std::ofstream output(filePath, std::ios::binary | std::ios::trunc);
        if (!output) {
            logError("Could not open resource file: " + entryName);
            success = false;
            continue;
        }
        if (entry->getSize() > 0) {
            output.write(entry->getData(), static_cast<std::streamsize>(entry->getSize()));
        }
        output.close();
        if (!output) {
            logError("Could not fully write resource file: " + entryName);
            success = false;
        }
    }
    return success;
}

bool isClassEntry(const j2d::JarEntry* entry) {
    return entry != nullptr
        && entry->getType() == j2d::TYPE_CLASS
        && entry->getName().size() > 6;
}

std::string className(const j2d::JarEntry* entry) {
    return entry->getName().substr(0, entry->getName().size() - 6);
}

jclass loadClassFromTargetLoader(JNIEnv* env, jobject loader, const std::string& internalName) {
    if (env == nullptr) {
        logError("loadClassFromTargetLoader received a null JNIEnv for " + internalName);
        return nullptr;
    }
    if (loader == nullptr) {
        logError("loadClassFromTargetLoader received a null ClassLoader for " + internalName);
        return nullptr;
    }
    LocalRef<jclass> loaderClass(env, env->GetObjectClass(loader));
    if (!loaderClass || reportAndClearException(env, ("Getting target ClassLoader class for " + internalName).c_str())) {
        return nullptr;
    }
    const jmethodID loadClass = env->GetMethodID(
        loaderClass.get(), "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (loadClass == nullptr || reportAndClearException(env, ("Resolving ClassLoader.loadClass for " + internalName).c_str())) {
        return nullptr;
    }
    std::string binaryName = internalName;
    std::replace(binaryName.begin(), binaryName.end(), '/', '.');
    LocalRef<jstring> name(env, env->NewStringUTF(binaryName.c_str()));
    if (!name || reportAndClearException(env, ("Creating class name for " + internalName).c_str())) return nullptr;
    jclass result = static_cast<jclass>(env->CallObjectMethod(loader, loadClass, name.get()));
    if (reportAndClearException(env, ("Loading class " + binaryName + " through target ClassLoader").c_str(), false)) {
        return nullptr;
    }
    return result;
}

const j2d::JarEntry* findMemoryBundleEntry(const std::vector<j2d::JarEntry*>& entries) {
    for (const j2d::JarEntry* entry : entries) {
        if (entry != nullptr && entry->getType() == j2d::TYPE_RESOURCE
            && entry->getName() == "META-INF/jar2dll/memory-modules.n2dm") return entry;
    }
    return nullptr;
}

FARPROC findMemoryExport(
    const std::shared_ptr<j2d::memory::ModuleRegistry>& modules,
    const char* name) {
    if (!modules || name == nullptr) return nullptr;
    std::string ignored;
    FARPROC result = nullptr;
    std::string provider;
    for (const auto& module : modules->modules()) {
        FARPROC candidate = j2d::memory::findMappedExport(module.image, name, ignored);
        if (candidate == nullptr) continue;
        if (result != nullptr && candidate != result) {
            logError("Ambiguous mapped export " + std::string(name) + " in modules "
                + provider + " and " + module.name);
            return nullptr;
        }
        result = candidate;
        provider = module.name;
    }
    return result;
}

bool mapMemoryModules(
    const std::vector<j2d::JarEntry*>& entries,
    std::shared_ptr<j2d::memory::ModuleRegistry>& mappedModules) {
    mappedModules.reset();
    const j2d::JarEntry* bundle = findMemoryBundleEntry(entries);
    if (bundle == nullptr) return true; // Stable mode has no N2DM entry.

    std::vector<j2d::memory::BundleModuleView> modules;
    std::string error;
    if (!j2d::memory::parseNativeBundle(reinterpret_cast<const unsigned char*>(bundle->getData()), bundle->getSize(), modules, error)) {
        logError("Memory native bundle is invalid: " + error); return false;
    }
    auto registry = std::make_shared<j2d::memory::ModuleRegistry>();
    for (const auto& module : modules) {
        std::cerr << "[memory] Mapping " << module.name << " (" << module.size << " bytes)" << std::endl;
        j2d::memory::MappedImage image;
        if (!j2d::memory::mapRelocatedImage(module.data, module.size, image, error)) {
            logError("Could not map memory module " + module.name + ": " + error); return false;
        }
        if (!registry->add(module.name, std::move(image))) {
            logError("Duplicate memory module name: " + module.name); return false;
        }
    }
    for (auto& module : registry->modules()) {
        std::cerr << "[memory] Resolving imports for " << module.name << std::endl;
        if (!registry->resolveImports(module, error) || !j2d::memory::protectMappedImage(module.image, error)
            || !j2d::memory::registerExceptionTable(module.image, error)) {
            logError("Could not prepare memory module " + module.name + ": " + error); return false;
        }
    }
    // JNI_OnLoad is the supported initialization mechanism for a mapped native
    // library.  Invoke it only after every module's imports are available, and in
    // dependency order so providers are ready before their consumers.
    if (!registry->invokeJniOnLoadInDependencyOrder(g_jvm, error)) {
        logError("Could not initialize memory module JNI_OnLoad: " + error); return false;
    }
    // Calling arbitrary third-party DllMain manually is unsafe: normal CRT DLLs
    // require Windows loader state. JNI exports that need it remain rejected by the
    // later compatibility gate rather than being executed in an invalid state.
    mappedModules = std::move(registry);
    std::cerr << "[memory] Mapped " << modules.size() << " embedded native module(s)" << std::endl;
    return true;
}

bool registerMemoryNativesForClass(
    JNIEnv* env,
    jclass klass,
    const std::string& internalName,
    const std::shared_ptr<j2d::memory::ModuleRegistry>& modules) {
    if (env == nullptr || klass == nullptr) {
        logError("Cannot register memory natives for " + internalName + ": invalid JNI class/environment");
        return false;
    }
    std::lock_guard registrationLock(g_nativeRegistrationMutex);
    jvmtiEnv* const jvmti = g_jvmti;
    if (jvmti == nullptr || !modules) return true;
    jint count = 0;
    jmethodID* methods = nullptr;
    const jvmtiError methodsResult = jvmti->GetClassMethods(klass, &count, &methods);
    if (methodsResult != JVMTI_ERROR_NONE) {
        // Classes without native declarations need no registration. Some JVMs
        // report INVALID_CLASS for a just-defined, not-yet-prepared class; leave
        // it alone and continue with the remaining JAR classes.
        std::cerr << "[memory] Skipping JVMTI method scan for " << internalName
            << ": " << methodsResult << std::endl;
        return true;
    }
    struct Candidate { std::string name; std::string descriptor; void* address = nullptr; };
    std::vector<Candidate> candidates;
    std::unordered_map<std::string, int> nameCounts;
    for (jint index = 0; index < count; ++index) {
        jint modifiers = 0;
        const jvmtiError modifiersResult = jvmti->GetMethodModifiers(methods[index], &modifiers);
        if (modifiersResult != JVMTI_ERROR_NONE) {
            logError("JVMTI GetMethodModifiers failed for " + internalName + ": " + std::to_string(modifiersResult));
            continue;
        }
        if ((modifiers & 0x0100) == 0) continue;
        char* name = nullptr; char* descriptor = nullptr;
        const jvmtiError nameResult = jvmti->GetMethodName(methods[index], &name, &descriptor, nullptr);
        if (nameResult != JVMTI_ERROR_NONE || name == nullptr || descriptor == nullptr) {
            logError("JVMTI GetMethodName failed for " + internalName + ": " + std::to_string(nameResult));
            if (name) jvmti->Deallocate(reinterpret_cast<unsigned char*>(name));
            if (descriptor) jvmti->Deallocate(reinterpret_cast<unsigned char*>(descriptor));
            continue;
        }
        candidates.push_back({name, descriptor}); ++nameCounts[candidates.back().name];
        jvmti->Deallocate(reinterpret_cast<unsigned char*>(name));
        jvmti->Deallocate(reinterpret_cast<unsigned char*>(descriptor));
    }
    if (methods) jvmti->Deallocate(reinterpret_cast<unsigned char*>(methods));
    for (auto& candidate : candidates) {
        const std::string shortName = j2d::jni::shortExportName(internalName, candidate.name);
        const std::string longName = j2d::jni::longExportName(internalName, candidate.name, candidate.descriptor);
        const bool overloaded = nameCounts[candidate.name] > 1;
        std::string selectedExport;
        if (overloaded) {
            if (!longName.empty()) {
                candidate.address = reinterpret_cast<void*>(findMemoryExport(modules, longName.c_str()));
                if (candidate.address != nullptr) selectedExport = longName;
            }
        }
        else {
            candidate.address = reinterpret_cast<void*>(findMemoryExport(modules, shortName.c_str()));
            if (candidate.address != nullptr) selectedExport = shortName;
            if (candidate.address == nullptr && !longName.empty()) {
                candidate.address = reinterpret_cast<void*>(findMemoryExport(modules, longName.c_str()));
                if (candidate.address != nullptr) selectedExport = longName;
            }
        }
        if (candidate.address == nullptr) {
            // A JAR can contain native methods provided by an optional or
            // independently loaded library. Leave those unregistered instead of
            // aborting the entire bootstrap; JNI_OnLoad may have registered them.
            std::cerr << "[memory] No mapped JNI export for " << internalName << "."
                << candidate.name << candidate.descriptor << "; leaving it to JVM resolution" << std::endl;
            continue;
        }
        std::cerr << "[memory] JNI " << internalName << "." << candidate.name
            << candidate.descriptor << " <- " << selectedExport << std::endl;
    }
    if (candidates.empty()) return true;
    std::vector<JNINativeMethod> nativeMethods;
    nativeMethods.reserve(candidates.size());
    for (auto& candidate : candidates) {
        if (candidate.address != nullptr) nativeMethods.push_back({candidate.name.data(), candidate.descriptor.data(), candidate.address});
    }
    if (nativeMethods.empty()) return true;
    if (env->RegisterNatives(klass, nativeMethods.data(), static_cast<jint>(nativeMethods.size())) != JNI_OK) {
        reportAndClearException(env, "RegisterNatives(memory)"); return false;
    }
    std::cerr << "[memory] Registered " << nativeMethods.size() << " native method(s) for " << internalName << std::endl;
    return true;
}

bool registerMemoryNatives(JNIEnv* env, jobject loader, const std::vector<j2d::JarEntry*>& entries) {
    const MemoryStateSnapshot state = memoryStateSnapshot();
    if (!state.modules) return true;
    for (j2d::JarEntry* entry : entries) {
        if (!isClassEntry(entry)) continue;
        LocalRef<jclass> klass(env, loadClassFromTargetLoader(env, loader, className(entry)));
        if (!klass) continue;
        if (!registerMemoryNativesForClass(env, klass.get(), className(entry), state.modules)) return false;
    }
    return true;
}

void JNICALL onMemoryClassPrepare(jvmtiEnv* jvmti, JNIEnv* env, jthread, jclass klass) {
    if (jvmti == nullptr || env == nullptr || klass == nullptr) return;
    const MemoryStateSnapshot state = memoryStateSnapshot();
    if (!state.modules || !state.nativeClasses) return;
    char* signature = nullptr;
    if (jvmti->GetClassSignature(klass, &signature, nullptr) != JVMTI_ERROR_NONE || signature == nullptr) return;
    std::string internalName(signature);
    jvmti->Deallocate(reinterpret_cast<unsigned char*>(signature));
    if (internalName.size() < 3 || internalName.front() != 'L' || internalName.back() != ';') return;
    internalName = internalName.substr(1, internalName.size() - 2);
    if (!state.nativeClasses->contains(internalName)) return;
    if (!registerMemoryNativesForClass(env, klass, internalName, state.modules)) {
        logError("Could not register prepared memory-mode JNI class " + internalName);
    }
}

void redefineLoadedClasses(JNIEnv* env, jobject loader, const std::vector<j2d::JarEntry*>& entries) {
    jvmtiEnv* const jvmti = g_jvmti;
    if (jvmti == nullptr) return;

    // Each JVMTI call reaches a VM safepoint. Small batches dramatically reduce pauses
    // for large JARs while retaining bounded JNI local-reference usage.
    constexpr std::size_t batchSize = 1;
    std::vector<jvmtiClassDefinition> batch;
    std::vector<jclass> localReferences;
    std::vector<std::vector<unsigned char>> mutableClassBytes;
    batch.reserve(batchSize);
    localReferences.reserve(batchSize);
    mutableClassBytes.reserve(batchSize);

    auto flush = [&] {
        if (batch.empty()) return;
        const jvmtiError result = jvmti->RedefineClasses(
            static_cast<jint>(batch.size()), batch.data());
        if (result != JVMTI_ERROR_NONE) {
            char* errorName = nullptr;
            const jvmtiError nameResult = jvmti->GetErrorName(result, &errorName);
            const char* reason = nameResult == JVMTI_ERROR_NONE && errorName != nullptr ? errorName : "unknown";
            std::cerr << "Redefine class batch failed: " << result << " Cause: " << reason << std::endl;
            if (errorName != nullptr) jvmti->Deallocate(reinterpret_cast<unsigned char*>(errorName));
        }
        for (jclass reference : localReferences) env->DeleteLocalRef(reference);
        batch.clear();
        localReferences.clear();
        mutableClassBytes.clear();
        };

    for (j2d::JarEntry* entry : entries) {
        if (!isClassEntry(entry)
            || entry->getSize() > static_cast<std::size_t>((std::numeric_limits<jint>::max)())) {
            continue;
        }
        const std::string name = className(entry);
        jclass loaded = loadClassFromTargetLoader(env, loader, name);
        if (loaded == nullptr) {
            continue;
        }
        mutableClassBytes.emplace_back(
            reinterpret_cast<const unsigned char*>(entry->getData()),
            reinterpret_cast<const unsigned char*>(entry->getData()) + entry->getSize());
        jvmtiClassDefinition definition{};
        definition.klass = loaded;
        definition.class_byte_count = static_cast<jint>(entry->getSize());
        definition.class_bytes = mutableClassBytes.back().data();
        batch.push_back(definition);
        localReferences.push_back(loaded);
        if (batch.size() == batchSize) flush();
    }

    flush();
}

bool enableMemoryClassPrepareEvents();

void runBootstrap(JNIEnv* env) {
    std::vector<j2d::JarEntry*> entries;
    EntryListGuard entryGuard(entries);

    const ResourceView j2df = getEmbeddedResource(RESOURCE_J2DF);
    const ResourceView mainClassConfig = getEmbeddedResource(RESOURCE_MAIN_CLASS);
    const ResourceView mainMethodConfig = getEmbeddedResource(RESOURCE_MAIN_METHOD);
    const ResourceView mainSignatureConfig = getEmbeddedResource(RESOURCE_MAIN_SIGNATURE);
    const ResourceView mainArgumentConfig = getEmbeddedResource(RESOURCE_MAIN_ARGUMENT);
    const ResourceView hasArgumentConfig = getEmbeddedResource(RESOURCE_HAS_ARGUMENT);
    const ResourceView classLoadersConfig = getEmbeddedResource(RESOURCE_CLASS_LOADERS);
    if (!j2df || !mainClassConfig || !mainMethodConfig || !mainSignatureConfig
        || !mainArgumentConfig || !hasArgumentConfig || !classLoadersConfig) {
        logError("Embedded DLL resources are incomplete");
        return;
    }

    std::string parseError;
    if (!j2d::parseBlob(
            reinterpret_cast<const char*>(j2df.data),
            j2dfPayloadLength(j2df),
            entries,
            &parseError)) {
        logError("Could not parse embedded JAR data: " + parseError);
        return;
    }

    auto memoryNativeClasses = std::make_shared<std::unordered_set<std::string>>();
    for (const j2d::JarEntry* entry : entries) {
        if (isClassEntry(entry)) memoryNativeClasses->insert(className(entry));
    }
    std::shared_ptr<j2d::memory::ModuleRegistry> mappedModules;
    if (!mapMemoryModules(entries, mappedModules)) {
        return;
    }
    publishMemoryState(mappedModules, memoryNativeClasses);
    if (mappedModules && g_jvmti == nullptr) {
        logError("Memory modules were mapped, but JVMTI is unavailable; direct JNI export registration may be incomplete");
    }
    else if (mappedModules) {
        enableMemoryClassPrepareEvents();
    }

    // Resources are isolated by process and module instance under the system
    // temporary directory. This avoids deleting a shared DLL-adjacent directory
    // while another process is using it. Java code can read the absolute root from
    // jar2dll.resource.dir or JAR2DLL_RESOURCE_DIR.
    const fs::path resourceDirectory = getRuntimeResourceDirectory();
    if (!cleanupPreviousResources(resourceDirectory)
        || !writeResources(entries, resourceDirectory)
        || !publishRuntimeResourceDirectory(env, resourceDirectory)) {
        logError("Runtime resources could not be prepared completely");
        return;
    }

    const std::vector<std::string> loaderCandidates = readClassLoaderCandidates(
        classLoadersConfig.data, classLoadersConfig.size);
    LocalRef<jobject> rawLoader(
        env,
        loaderCandidates.empty()
            ? getCurrentThreadContextClassLoader(env)
            : findTargetClassLoader(env, loaderCandidates));
    if (env->ExceptionCheck()) {
        reportAndClearException(env, "Resolving target ClassLoader");
        rawLoader.reset();
    }
    if (!rawLoader && loaderCandidates.empty()) {
        rawLoader.reset(getSystemClassLoader(env));
    }
    if (!rawLoader) {
        if (loaderCandidates.empty()) {
            logError("Current thread context ClassLoader is null or unavailable");
        }
        else {
            logError("Could not find any configured ClassLoader: "
                + joinClassLoaderCandidates(loaderCandidates));
        }
        return;
    }

    GlobalRef loader(env, rawLoader.get());
    const bool globalReferenceException = reportAndClearException(env, "NewGlobalRef(ClassLoader)");
    if (!loader || globalReferenceException) {
        return;
    }

    std::vector<j2d::JarEntry*> pendingDefinitions;
    std::vector<j2d::JarEntry*> loadedBeforeDefinition;
    for (j2d::JarEntry* entry : entries) {
        if (!isClassEntry(entry)) continue;
        LocalRef<jclass> existing(env, loadClassFromTargetLoader(env, loader.get(), className(entry)));
        if (!existing) {
            pendingDefinitions.push_back(entry);
        }
        else {
            loadedBeforeDefinition.push_back(entry);
        }
    }

    // Define only classes still unresolved. A successful class leaves the queue, so a
    // large JAR no longer performs 10*N FindClass calls or repeatedly touches old classes.
    std::unordered_map<std::string, std::string> definitionErrors;
    for (int round = 0; round < 18 && !pendingDefinitions.empty(); ++round) {
        bool madeProgress = false;
        std::vector<j2d::JarEntry*> next;
        next.reserve(pendingDefinitions.size());
        for (j2d::JarEntry* entry : pendingDefinitions) {
            const std::string name = className(entry);
            if (entry->getSize() > static_cast<std::size_t>((std::numeric_limits<jsize>::max)())) {
                logError("Class file is too large for JNI: " + name);
                continue;
            }
            LocalRef<jclass> defined(env, env->DefineClass(nullptr, loader.get(),
                reinterpret_cast<const jbyte*>(entry->getData()), static_cast<jsize>(entry->getSize())));
            if (defined) {
                madeProgress = true;
                definitionErrors.erase(name);
            }
            else {
                std::string reason = takePendingJavaException(env, false);
                definitionErrors[name] = reason.empty()
                    ? "DefineClass returned null without a Java exception"
                    : std::move(reason);
                next.push_back(entry);
            }
        }
        pendingDefinitions.swap(next);
        if (!madeProgress) break;
    }
    if (!pendingDefinitions.empty()) {
        logError("Could not define " + std::to_string(pendingDefinitions.size()) + " class(es) after dependency retries");
        for (const j2d::JarEntry* entry : pendingDefinitions) {
            const std::string name = className(entry);
            const auto error = definitionErrors.find(name);
            logError("  " + name + ": "
                + (error == definitionErrors.end() ? "unknown definition failure" : error->second));
        }
    }

    // New classes are already current. Only redefine classes that existed before this DLL
    // was loaded, and do it after the definition phase in bounded JVMTI batches.

    //redefineLoadedClasses(env, loader.get(), loadedBeforeDefinition);
    if (!registerMemoryNatives(env, loader.get(), entries)) {
        logError("Could not complete memory-mode native registration for loaded classes");
        return;
    }
    LocalRef<jclass> threadClass(env, env->FindClass("java/lang/Thread"));
    const bool threadClassException = reportAndClearException(env, "FindClass(java/lang/Thread)");
    if (!threadClass || threadClassException) {
        return;
    }

    const jmethodID currentThreadMethod = env->GetStaticMethodID(
        threadClass.get(), "currentThread", "()Ljava/lang/Thread;");
    const jmethodID getContextMethod = env->GetMethodID(
        threadClass.get(), "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    const jmethodID setContextMethod = env->GetMethodID(
        threadClass.get(), "setContextClassLoader", "(Ljava/lang/ClassLoader;)V");
    const bool contextMethodException = reportAndClearException(
        env, "Resolving Thread context ClassLoader methods");
    if (currentThreadMethod == nullptr || getContextMethod == nullptr || setContextMethod == nullptr
        || contextMethodException) {
        return;
    }

    LocalRef<jobject> currentThread(
        env, env->CallStaticObjectMethod(threadClass.get(), currentThreadMethod));
    LocalRef<jobject> previousLoader(
        env,
        currentThread ? env->CallObjectMethod(currentThread.get(), getContextMethod) : nullptr);
    const bool currentThreadException = reportAndClearException(
        env, "Reading current Thread context ClassLoader");
    if (!currentThread || currentThreadException) {
        return;
    }

    env->CallVoidMethod(currentThread.get(), setContextMethod, loader.get());
    if (reportAndClearException(env, "Setting target context ClassLoader")) {
        return;
    }

    const std::string targetClass = cleanConfig(mainClassConfig.data, mainClassConfig.size);
    const std::string targetMethod = cleanConfig(mainMethodConfig.data, mainMethodConfig.size);
    const std::string targetSignature = cleanConfig(mainSignatureConfig.data, mainSignatureConfig.size);
    const bool hasArgument = cleanConfig(hasArgumentConfig.data, hasArgumentConfig.size) == "true";
    std::string jniPath = targetClass;
    std::replace(jniPath.begin(), jniPath.end(), '.', '/');

    // 保留原调用逻辑：先 FindClass，失败后再通过目标 ClassLoader.loadClass 回退。
    LocalRef<jclass> mainClass(env, env->FindClass(jniPath.c_str()));
    if (env->ExceptionCheck() || !mainClass) {
        reportAndClearException(env, ("FindClass(" + jniPath + ")").c_str(), false);
        mainClass.reset(loadClassFromTargetLoader(env, loader.get(), jniPath));
    }

    if (mainClass) {
        if (targetSignature != "()V" && targetSignature != "([Ljava/lang/String;)V") {
            logError("Unsupported configured main signature: " + targetSignature
                + "; expected ()V or ([Ljava/lang/String;)V");
        }
        else {
        const jmethodID method = env->GetStaticMethodID(
            mainClass.get(), targetMethod.c_str(), targetSignature.c_str());
        const bool methodException = reportAndClearException(
            env, "Resolving configured main method");
        if (method != nullptr && !methodException) {
            if (targetSignature == "()V") {
                env->CallStaticVoidMethod(mainClass.get(), method);
            }
            else if (targetSignature == "([Ljava/lang/String;)V") {
                LocalRef<jclass> stringClass(env, env->FindClass("java/lang/String"));
                LocalRef<jobjectArray> arguments(
                    env,
                    stringClass ? env->NewObjectArray(hasArgument ? 1 : 0, stringClass.get(), nullptr) : nullptr);
                if (!stringClass || !arguments || reportAndClearException(env, "Creating main(String[]) arguments")) {
                    logError("Could not create arguments for configured main(String[]) method");
                }
                else {
                    if (hasArgument) {
                        const std::string argument = std::string(
                            reinterpret_cast<const char*>(mainArgumentConfig.data), mainArgumentConfig.size);
                        LocalRef<jstring> value(env, env->NewStringUTF(argument.c_str()));
                        if (!value) {
                            logError("Could not set main(String[]) argument");
                        }
                        else {
                            env->SetObjectArrayElement(arguments.get(), 0, value.get());
                            if (reportAndClearException(env, "Setting main(String[]) argument")) {
                                logError("Could not set main(String[]) argument");
                            }
                            else {
                                env->CallStaticVoidMethod(mainClass.get(), method, arguments.get());
                            }
                        }
                    }
                    else {
                        env->CallStaticVoidMethod(mainClass.get(), method, arguments.get());
                    }
                }
            }
            reportAndClearException(env, "Executing configured main method");
        }
        }
    }
    else {
        logError("Configured main class could not be loaded: " + targetClass);
    }

    // 释放优先级：目标方法结束/异常清除 -> 恢复原 ClassLoader -> 删除 JNI 引用
    // -> runBootstrap 返回 -> DetachCurrentThread。不能先删 loader 全局引用再恢复上下文。
    env->CallVoidMethod(currentThread.get(), setContextMethod, previousLoader.get());
    reportAndClearException(env, "Restoring previous context ClassLoader");
}

bool acquireJvmti(JavaVM* vm) {
    if (vm == nullptr) {
        return false;
    }

    jvmtiEnv* environment = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&environment), JVMTI_VERSION_1_0) != JNI_OK
        || environment == nullptr) {
        g_jvmti = nullptr;
        return false;
    }

    jvmtiCapabilities capabilities{};
    capabilities.can_redefine_classes = 1;
    if (environment->AddCapabilities(&capabilities) != JVMTI_ERROR_NONE) {
        g_jvmti = nullptr;
        return false;
    }

    jvmtiEventCallbacks callbacks{};
    callbacks.ClassPrepare = onMemoryClassPrepare;
    if (environment->SetEventCallbacks(&callbacks, sizeof(callbacks)) != JVMTI_ERROR_NONE) {
        g_jvmti = nullptr;
        return false;
    }

    g_jvmti = environment;
    return true;
}

bool enableMemoryClassPrepareEvents() {
    if (g_jvmti == nullptr) return false;
    const jvmtiError result = g_jvmti->SetEventNotificationMode(
        JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, nullptr);
    if (result != JVMTI_ERROR_NONE) {
        logError("Could not enable JVMTI ClassPrepare notifications: " + std::to_string(result));
        return false;
    }
    return true;
}

DWORD WINAPI loaderThread(LPVOID) {
    // Java may retain registered native pointers indefinitely. Pinning prevents
    // external FreeLibrary calls from unloading this code while the JVM is alive.
    HMODULE pinnedModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&loaderThread),
            &pinnedModule)) {
        logError("Could not pin jar2dll for the process lifetime; error "
            + std::to_string(GetLastError()));
        return 1;
    }

    DWORD exitCode = 0;
    jsize vmCount = 0;
    if (JNI_GetCreatedJavaVMs(&g_jvm, 1, &vmCount) == JNI_OK && vmCount > 0) {
        // 该作用域结束时，runBootstrap 中的引用已经全部释放，随后才 DetachCurrentThread。
        AttachedThreadGuard attached(g_jvm);
        if (attached) {
            if (!acquireJvmti(g_jvm)) {
                logError("Could not acquire JVMTI; class redefinition and memory JNI registration are unavailable");
            }
            runBootstrap(attached.env());
        }
        else {
            logError("AttachCurrentThreadAsDaemon failed with JNI status "
                + std::to_string(attached.status()));
            exitCode = 2;
        }
    }
    else {
        logError("No created JVM was available to jar2dll");
        exitCode = 3;
    }

    return exitCode;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);

        // DllMain 持有 Windows loader lock，因此这里只创建线程，所有 JVM、文件系统和
        // C++ 容器操作都放到 loaderThread 中执行。
        HANDLE thread = CreateThread(nullptr, 0, loaderThread, nullptr, 0, nullptr);
        if (thread == nullptr) {
            const DWORD error = GetLastError();
            std::ostringstream message;
            message << "[jar2dll] CreateThread failed during DLL initialization; error " << error << "\n";
            OutputDebugStringA(message.str().c_str());
            return FALSE;
        }

        // CloseHandle 只释放“线程句柄”这一内核资源，不会终止刚创建的线程。
        CloseHandle(thread);
    }

    // No filesystem or JVM work is performed under the loader lock. Runtime
    // resources live in a process/module-isolated temporary directory.
    return TRUE;
}
