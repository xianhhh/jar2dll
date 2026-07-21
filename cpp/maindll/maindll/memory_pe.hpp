#pragma once

// Experimental memory-mode PE preflight.  Kept header-only until the mapper is
// enabled so the stable DLL has no new runtime path or state.
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace j2d::memory {

// IMAGE_DELAYLOAD_DESCRIPTOR is not exposed consistently by every Windows SDK.
struct DelayImportDescriptor final {
    DWORD attributes;
    DWORD name;
    DWORD moduleHandle;
    DWORD importAddressTable;
    DWORD importNameTable;
    DWORD boundImportAddressTable;
    DWORD unloadImportAddressTable;
    DWORD timestamp;
};

struct PeImageInfo final {
    const IMAGE_NT_HEADERS64* headers = nullptr;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    std::uint16_t sectionCount = 0;
};

struct MappedImage final {
    unsigned char* base = nullptr;
    std::size_t size = 0;
    PRUNTIME_FUNCTION functionTable = nullptr;
    DWORD functionCount = 0;
    bool protectedImage = false;
    bool importsResolved = false;
    bool initialized = false;
    bool jniOnLoadInvoked = false;
    ~MappedImage() { if (functionTable != nullptr) RtlDeleteFunctionTable(functionTable); if (base != nullptr) VirtualFree(base, 0, MEM_RELEASE); }
    MappedImage() = default;
    MappedImage(const MappedImage&) = delete;
    MappedImage& operator=(const MappedImage&) = delete;
    MappedImage(MappedImage&& other) noexcept : base(other.base), size(other.size), functionTable(other.functionTable), functionCount(other.functionCount), protectedImage(other.protectedImage), importsResolved(other.importsResolved), initialized(other.initialized), jniOnLoadInvoked(other.jniOnLoadInvoked) { other.base = nullptr; other.functionTable = nullptr; }
    MappedImage& operator=(MappedImage&& other) noexcept { if (this != &other) { if (functionTable) RtlDeleteFunctionTable(functionTable); if (base) VirtualFree(base, 0, MEM_RELEASE); base = other.base; size = other.size; functionTable = other.functionTable; functionCount = other.functionCount; protectedImage = other.protectedImage; importsResolved = other.importsResolved; initialized = other.initialized; jniOnLoadInvoked = other.jniOnLoadInvoked; other.base = nullptr; other.functionTable = nullptr; other.functionCount = 0; } return *this; }
};

inline bool inspectPe64(const unsigned char* bytes, std::size_t length, PeImageInfo& result, std::string& error) {
    result = {};
    if (bytes == nullptr || length < sizeof(IMAGE_DOS_HEADER)) { error = "truncated DOS header"; return false; }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) { error = "invalid DOS header"; return false; }
    const std::size_t ntOffset = static_cast<std::size_t>(dos->e_lfanew);
    if (ntOffset > length || length - ntOffset < sizeof(IMAGE_NT_HEADERS64)) { error = "truncated NT header"; return false; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + ntOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { error = "invalid PE signature"; return false; }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) { error = "only x64 PE images are supported"; return false; }
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->OptionalHeader.SizeOfImage == 0
        || nt->FileHeader.NumberOfSections == 0) { error = "not a PE32+ image"; return false; }
    const std::size_t sectionOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt->FileHeader.SizeOfOptionalHeader;
    const std::size_t sectionBytes = static_cast<std::size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (sectionOffset > length || sectionBytes > length - sectionOffset) { error = "truncated section table"; return false; }
    if (nt->OptionalHeader.SizeOfHeaders < sectionOffset || nt->OptionalHeader.SizeOfHeaders > nt->OptionalHeader.SizeOfImage) {
        error = "invalid image header size"; return false;
    }
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(bytes + sectionOffset);
    for (std::uint16_t index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        if (section.SizeOfRawData != 0 && (section.PointerToRawData > length || section.SizeOfRawData > length - section.PointerToRawData)) {
            error = "section raw data exceeds input"; return false;
        }
        const std::size_t mappedSize = (std::max)(static_cast<std::size_t>(section.Misc.VirtualSize), static_cast<std::size_t>(section.SizeOfRawData));
        if (section.VirtualAddress > nt->OptionalHeader.SizeOfImage || mappedSize > nt->OptionalHeader.SizeOfImage - section.VirtualAddress) {
            error = "section exceeds image"; return false;
        }
    }
    result = {nt, sections, nt->FileHeader.NumberOfSections};
    error.clear();
    return true;
}

inline bool validateMapperCompatibility(const PeImageInfo& image, std::string& error) {
    const auto& directories = image.headers->OptionalHeader.DataDirectory;
    if (directories[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].Size != 0) { error = "managed PE images are not supported"; return false; }
    error.clear(); return true;
}

inline bool rvaRange(std::size_t imageSize, std::size_t rva, std::size_t length) noexcept {
    return rva <= imageSize && length <= imageSize - rva;
}

inline bool rvaCString(const unsigned char* base, std::size_t imageSize, std::size_t rva, const char*& value) noexcept {
    if (!rvaRange(imageSize, rva, 1)) return false;
    value = reinterpret_cast<const char*>(base + rva);
    return std::memchr(value, 0, imageSize - rva) != nullptr;
}

inline bool validateImportDirectory(const unsigned char* base, std::size_t imageSize, const IMAGE_DATA_DIRECTORY& directory, std::string& error) {
    if (directory.VirtualAddress == 0 && directory.Size == 0) return true;
    if (directory.VirtualAddress == 0 || !rvaRange(imageSize, directory.VirtualAddress, directory.Size)
        || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) { error = "invalid import directory"; return false; }
    const std::size_t end = static_cast<std::size_t>(directory.VirtualAddress) + directory.Size;
    bool terminated = false;
    for (std::size_t cursor = directory.VirtualAddress; rvaRange(end, cursor, sizeof(IMAGE_IMPORT_DESCRIPTOR)); cursor += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + cursor);
        if (descriptor->Name == 0 && descriptor->FirstThunk == 0) { terminated = true; break; }
        const char* ignoredName = nullptr;
        if (!rvaCString(base, imageSize, descriptor->Name, ignoredName) || descriptor->FirstThunk == 0) { error = "invalid import descriptor"; return false; }
        const std::size_t lookupRva = descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk;
        for (std::size_t index = 0;; ++index) {
            if (index > imageSize / sizeof(IMAGE_THUNK_DATA64)
                || !rvaRange(imageSize, lookupRva + index * sizeof(IMAGE_THUNK_DATA64), sizeof(IMAGE_THUNK_DATA64))
                || !rvaRange(imageSize, static_cast<std::size_t>(descriptor->FirstThunk) + index * sizeof(IMAGE_THUNK_DATA64), sizeof(IMAGE_THUNK_DATA64))) {
                error = "unterminated import thunk table"; return false;
            }
            const auto* lookup = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + lookupRva + index * sizeof(IMAGE_THUNK_DATA64));
            if (lookup->u1.AddressOfData == 0) break;
            if (!IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) {
                const std::size_t nameRva = static_cast<std::size_t>(lookup->u1.AddressOfData);
                const char* ignoredImport = nullptr;
                if (!rvaRange(imageSize, nameRva, sizeof(WORD) + 1) || !rvaCString(base, imageSize, nameRva + sizeof(WORD), ignoredImport)) {
                    error = "invalid import-by-name entry"; return false;
                }
            }
        }
    }
    if (!terminated) { error = "unterminated import descriptor table"; return false; }
    return true;
}

inline bool validateExportDirectory(const unsigned char* base, std::size_t imageSize, const IMAGE_DATA_DIRECTORY& directory, std::string& error) {
    if (directory.VirtualAddress == 0 && directory.Size == 0) return true;
    if (directory.VirtualAddress == 0 || !rvaRange(imageSize, directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) { error = "invalid export directory"; return false; }
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    if ((exports->NumberOfFunctions != 0 && !rvaRange(imageSize, exports->AddressOfFunctions, static_cast<std::size_t>(exports->NumberOfFunctions) * sizeof(DWORD)))
        || (exports->NumberOfNames != 0 && (!rvaRange(imageSize, exports->AddressOfNames, static_cast<std::size_t>(exports->NumberOfNames) * sizeof(DWORD))
            || !rvaRange(imageSize, exports->AddressOfNameOrdinals, static_cast<std::size_t>(exports->NumberOfNames) * sizeof(WORD))))) {
        error = "export table exceeds image"; return false;
    }
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const char* ignoredName = nullptr;
        if (ordinals[index] >= exports->NumberOfFunctions || !rvaCString(base, imageSize, names[index], ignoredName)
            || functions[ordinals[index]] >= imageSize) { error = "invalid export entry"; return false; }
    }
    return true;
}

inline bool mapRelocatedImage(const unsigned char* bytes, std::size_t length, MappedImage& mapped, std::string& error) {
    PeImageInfo info;
    if (!inspectPe64(bytes, length, info, error)) return false;
    if (!validateMapperCompatibility(info, error)) return false;
    auto* preferred = reinterpret_cast<void*>(static_cast<std::uintptr_t>(info.headers->OptionalHeader.ImageBase));
    auto* base = static_cast<unsigned char*>(VirtualAlloc(preferred, info.headers->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) base = static_cast<unsigned char*>(VirtualAlloc(nullptr, info.headers->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) { error = "VirtualAlloc failed"; return false; }
    MappedImage result;
    result.base = base;
    result.size = info.headers->OptionalHeader.SizeOfImage;
    if (info.headers->OptionalHeader.SizeOfHeaders > length) { error = "headers exceed input"; return false; }
    std::memcpy(base, bytes, info.headers->OptionalHeader.SizeOfHeaders);
    for (std::uint16_t index = 0; index < info.sectionCount; ++index) {
        const auto& section = info.sections[index];
        if (section.VirtualAddress > result.size || section.Misc.VirtualSize > result.size - section.VirtualAddress
            || section.SizeOfRawData > result.size - section.VirtualAddress) { error = "section exceeds image"; return false; }
        if (section.SizeOfRawData != 0) std::memcpy(base + section.VirtualAddress, bytes + section.PointerToRawData, section.SizeOfRawData);
    }
    const auto delta = reinterpret_cast<std::intptr_t>(base) - static_cast<std::intptr_t>(info.headers->OptionalHeader.ImageBase);
    const auto& directory = info.headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory.VirtualAddress == 0 && directory.Size != 0) { error = "invalid relocation directory"; return false; }
    if (delta != 0 && directory.Size == 0) { error = "image requires relocation but has no relocation directory"; return false; }
    if (delta != 0 && directory.Size != 0) {
        if (directory.VirtualAddress > result.size || directory.Size > result.size - directory.VirtualAddress) { error = "relocation directory exceeds image"; return false; }
        std::size_t cursor = directory.VirtualAddress;
        const std::size_t end = cursor + directory.Size;
        while (cursor < end) {
            if (end - cursor < sizeof(IMAGE_BASE_RELOCATION)) { error = "truncated relocation block"; return false; }
            auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(base + cursor);
            if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock > end - cursor) { error = "invalid relocation block"; return false; }
            const auto count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            auto* entries = reinterpret_cast<WORD*>(block + 1);
            for (std::size_t item = 0; item < count; ++item) {
                const WORD type = entries[item] >> 12U;
                const std::size_t rva = block->VirtualAddress + (entries[item] & 0x0FFFU);
                if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
                if (type != IMAGE_REL_BASED_DIR64 || rva > result.size || sizeof(std::uint64_t) > result.size - rva) { error = "unsupported or invalid relocation"; return false; }
                *reinterpret_cast<std::uint64_t*>(base + rva) += static_cast<std::uint64_t>(delta);
            }
            cursor += block->SizeOfBlock;
        }
    }
    const auto& importDirectory = info.headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto& exportDirectory = info.headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!validateImportDirectory(base, result.size, importDirectory, error)
        || !validateExportDirectory(base, result.size, exportDirectory, error)) return false;
    FlushInstructionCache(GetCurrentProcess(), base, result.size);
    mapped = std::move(result);
    error.clear();
    return true;
}

inline bool resolveSystemImports(MappedImage& mapped, std::string& error) {
    if (mapped.base == nullptr || mapped.size < sizeof(IMAGE_DOS_HEADER)) { error = "image is not mapped"; return false; }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mapped.base);
    if (dos->e_lfanew < 0 || static_cast<std::size_t>(dos->e_lfanew) > mapped.size - sizeof(IMAGE_NT_HEADERS64)) { error = "mapped NT header is invalid"; return false; }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(mapped.base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.Size == 0) return true;
    if (directory.VirtualAddress > mapped.size || directory.Size > mapped.size - directory.VirtualAddress) { error = "import directory exceeds image"; return false; }
    const std::size_t end = directory.VirtualAddress + directory.Size;
    for (std::size_t cursor = directory.VirtualAddress; cursor + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= end; cursor += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(mapped.base + cursor);
        if (descriptor->Name == 0 && descriptor->FirstThunk == 0) break;
        if (descriptor->Name >= mapped.size || descriptor->FirstThunk >= mapped.size) { error = "import descriptor has invalid RVA"; return false; }
        const char* moduleName = reinterpret_cast<const char*>(mapped.base + descriptor->Name);
        HMODULE module = LoadLibraryA(moduleName);
        if (module == nullptr) { error = std::string("could not load import: ") + moduleName; return false; }
        auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA64*>(mapped.base + (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk));
        auto* address = reinterpret_cast<IMAGE_THUNK_DATA64*>(mapped.base + descriptor->FirstThunk);
        for (; lookup->u1.AddressOfData != 0; ++lookup, ++address) {
            FARPROC target = nullptr;
            if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) target = GetProcAddress(module, reinterpret_cast<const char*>(IMAGE_ORDINAL64(lookup->u1.Ordinal)));
            else {
                if (lookup->u1.AddressOfData >= mapped.size) { error = "import-by-name RVA exceeds image"; return false; }
                auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(mapped.base + lookup->u1.AddressOfData);
                target = GetProcAddress(module, reinterpret_cast<const char*>(name->Name));
            }
            if (target == nullptr) { error = std::string("could not resolve import from ") + moduleName; return false; }
            address->u1.Function = reinterpret_cast<ULONGLONG>(target);
        }
    }
    error.clear();
    return true;
}

inline bool protectMappedImage(MappedImage& mapped, std::string& error) {
    if (mapped.protectedImage) return true;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mapped.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mapped.base + dos->e_lfanew);
    DWORD ignored = 0;
    if (!VirtualProtect(mapped.base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &ignored)) { error = "could not protect PE headers"; return false; }
    const auto* sections = IMAGE_FIRST_SECTION(const_cast<IMAGE_NT_HEADERS64*>(nt));
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const DWORD flags = sections[index].Characteristics;
        const bool execute = (flags & IMAGE_SCN_MEM_EXECUTE) != 0;
        const bool write = (flags & IMAGE_SCN_MEM_WRITE) != 0;
        DWORD protection = execute ? (write ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (write ? PAGE_READWRITE : PAGE_READONLY);
        const SIZE_T size = sections[index].Misc.VirtualSize == 0 ? sections[index].SizeOfRawData : sections[index].Misc.VirtualSize;
        if (size != 0 && !VirtualProtect(mapped.base + sections[index].VirtualAddress, size, protection, &ignored)) { error = "could not protect PE section"; return false; }
    }
    mapped.protectedImage = true; error.clear(); return true;
}

inline bool registerExceptionTable(MappedImage& mapped, std::string& error) {
    if (mapped.functionTable != nullptr) return true;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mapped.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mapped.base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (dir.Size == 0) return true;
    if (dir.VirtualAddress > mapped.size || dir.Size > mapped.size - dir.VirtualAddress || dir.Size % sizeof(RUNTIME_FUNCTION) != 0) { error = "invalid x64 exception directory"; return false; }
    auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(mapped.base + dir.VirtualAddress);
    const DWORD count = dir.Size / sizeof(RUNTIME_FUNCTION);
    if (!RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(mapped.base))) { error = "RtlAddFunctionTable failed"; return false; }
    mapped.functionTable = table; mapped.functionCount = count; error.clear(); return true;
}

inline bool initializeMappedImage(MappedImage& mapped, std::string& error) {
    if (mapped.initialized) return true;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mapped.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mapped.base + dos->e_lfanew);
    const auto& tlsDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tlsDir.VirtualAddress != 0) {
        if (tlsDir.VirtualAddress > mapped.size || sizeof(IMAGE_TLS_DIRECTORY64) > mapped.size - tlsDir.VirtualAddress) { error = "TLS directory exceeds image"; return false; }
        const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(mapped.base + tlsDir.VirtualAddress);
        const auto* callbacks = reinterpret_cast<const PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
        if (callbacks != nullptr) for (; *callbacks != nullptr; ++callbacks) (*callbacks)(mapped.base, DLL_PROCESS_ATTACH, nullptr);
    }
    if (nt->OptionalHeader.AddressOfEntryPoint == 0) { mapped.initialized = true; return true; }
    if (nt->OptionalHeader.AddressOfEntryPoint >= mapped.size) { error = "entry point exceeds image"; return false; }
    using EntryPoint = BOOL (WINAPI*)(HINSTANCE, DWORD, LPVOID);
    if (!reinterpret_cast<EntryPoint>(mapped.base + nt->OptionalHeader.AddressOfEntryPoint)(reinterpret_cast<HINSTANCE>(mapped.base), DLL_PROCESS_ATTACH, nullptr)) { error = "DLL_PROCESS_ATTACH returned false"; return false; }
    mapped.initialized = true; error.clear(); return true;
}

inline FARPROC findMappedExport(const MappedImage& mapped, const char* symbol, std::string& error) {
    if (mapped.base == nullptr || symbol == nullptr) { error = "image or symbol is null"; return nullptr; }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mapped.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mapped.base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0 || directory.VirtualAddress > mapped.size || sizeof(IMAGE_EXPORT_DIRECTORY) > mapped.size - directory.VirtualAddress) { error = "export directory is missing"; return nullptr; }
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(mapped.base + directory.VirtualAddress);
    if (exports->AddressOfNames >= mapped.size || exports->AddressOfNameOrdinals >= mapped.size || exports->AddressOfFunctions >= mapped.size) { error = "export table exceeds image"; return nullptr; }
    const auto* names = reinterpret_cast<const DWORD*>(mapped.base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(mapped.base + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(mapped.base + exports->AddressOfFunctions);
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        if (names[index] >= mapped.size || ordinals[index] >= exports->NumberOfFunctions) continue;
        const char* name = reinterpret_cast<const char*>(mapped.base + names[index]);
        if (std::strcmp(name, symbol) != 0) continue;
        const DWORD rva = functions[ordinals[index]];
        if (rva >= mapped.size) { error = "export RVA exceeds image"; return nullptr; }
        error.clear();
        return reinterpret_cast<FARPROC>(mapped.base + rva);
    }
    error = std::string("export not found: ") + symbol;
    return nullptr;
}

inline bool invokeJniOnLoad(MappedImage& mapped, void* javaVm, std::string& error) {
    if (mapped.jniOnLoadInvoked) return true;
    FARPROC exportAddress = findMappedExport(mapped, "JNI_OnLoad", error);
    if (exportAddress == nullptr) {
        if (error.rfind("export not found:", 0) == 0) { mapped.jniOnLoadInvoked = true; error.clear(); return true; }
        return false;
    }
    using JniOnLoad = int (WINAPI*)(void*, void*);
    const int version = reinterpret_cast<JniOnLoad>(exportAddress)(javaVm, nullptr);
    if (version < 0x00010001 || version > 0x0001000A) { error = "JNI_OnLoad returned an unsupported JNI version"; return false; }
    mapped.jniOnLoadInvoked = true; error.clear(); return true;
}

inline bool resolveOneMappedImport(MappedImage& consumer, const char* moduleName, const MappedImage& provider, std::string& error) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(consumer.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(consumer.base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (std::size_t cursor = dir.VirtualAddress; cursor + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= dir.VirtualAddress + dir.Size; cursor += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(consumer.base + cursor);
        if (desc->Name == 0 && desc->FirstThunk == 0) break;
        const char* name = reinterpret_cast<const char*>(consumer.base + desc->Name);
        if (_stricmp(name, moduleName) != 0) continue;
        auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA64*>(consumer.base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* address = reinterpret_cast<IMAGE_THUNK_DATA64*>(consumer.base + desc->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++address) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) { error = "internal ordinal imports are not supported"; return false; }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(consumer.base + lookup->u1.AddressOfData);
            FARPROC target = findMappedExport(provider, reinterpret_cast<const char*>(import->Name), error);
            if (!target) return false;
            address->u1.Function = reinterpret_cast<ULONGLONG>(target);
        }
        error.clear(); return true;
    }
    error = std::string("internal import module not found: ") + moduleName; return false;
}

struct MappedModule final { std::string name; MappedImage image; };
class ModuleRegistry final {
public:
    ~ModuleRegistry() { for (auto it = systemModules_.rbegin(); it != systemModules_.rend(); ++it) FreeLibrary(*it); }
    bool add(std::string name, MappedImage&& image) { if (find(name) != nullptr) return false; modules_.push_back({std::move(name), std::move(image)}); return true; }
    MappedModule* find(const std::string& name) { for (auto& module : modules_) if (_stricmp(module.name.c_str(), name.c_str()) == 0) return &module; return nullptr; }
    bool resolveKnownImports(MappedModule& consumer, std::string& error) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(consumer.image.base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(consumer.image.base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (std::size_t cursor = dir.VirtualAddress; cursor + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= dir.VirtualAddress + dir.Size; cursor += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(consumer.image.base + cursor);
            if (desc->Name == 0 && desc->FirstThunk == 0) break;
            const char* name = reinterpret_cast<const char*>(consumer.image.base + desc->Name);
            if (auto* provider = find(name); provider != nullptr && !resolveOneMappedImport(consumer.image, name, provider->image, error)) return false;
        }
        error.clear(); return true;
    }
    bool resolveImports(MappedModule& consumer, std::string& error) {
        if (consumer.image.importsResolved) return true;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(consumer.image.base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(consumer.image.base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.Size == 0) return true;
        if (dir.VirtualAddress > consumer.image.size || dir.Size > consumer.image.size - dir.VirtualAddress) { error = "import directory exceeds image"; return false; }
        for (std::size_t cursor = dir.VirtualAddress; cursor + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= dir.VirtualAddress + dir.Size; cursor += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(consumer.image.base + cursor);
            if (desc->Name == 0 && desc->FirstThunk == 0) break;
            if (desc->Name >= consumer.image.size || desc->FirstThunk >= consumer.image.size) { error = "import descriptor has invalid RVA"; return false; }
            const char* moduleName = reinterpret_cast<const char*>(consumer.image.base + desc->Name);
            MappedModule* internal = find(moduleName);
            HMODULE system = nullptr;
            if (internal == nullptr) {
                system = LoadLibraryA(moduleName);
                if (!system) { error = std::string("could not load import: ") + moduleName; return false; }
                systemModules_.push_back(system);
            }
            auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA64*>(consumer.image.base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
            auto* address = reinterpret_cast<IMAGE_THUNK_DATA64*>(consumer.image.base + desc->FirstThunk);
            for (; lookup->u1.AddressOfData; ++lookup, ++address) {
                FARPROC target = nullptr;
                if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) {
                    if (internal) { error = "internal ordinal imports are not supported"; return false; }
                    target = GetProcAddress(system, reinterpret_cast<const char*>(IMAGE_ORDINAL64(lookup->u1.Ordinal)));
                } else {
                    if (lookup->u1.AddressOfData >= consumer.image.size) { error = "import-by-name RVA exceeds image"; return false; }
                    auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(consumer.image.base + lookup->u1.AddressOfData);
                    target = internal ? findMappedExport(internal->image, reinterpret_cast<const char*>(imported->Name), error)
                                      : GetProcAddress(system, reinterpret_cast<const char*>(imported->Name));
                }
                if (!target) { if (error.empty()) error = std::string("could not resolve import from ") + moduleName; return false; }
                address->u1.Function = reinterpret_cast<ULONGLONG>(target);
            }
        }
        const auto& delay = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (delay.Size != 0) {
            if (delay.VirtualAddress == 0 || delay.VirtualAddress > consumer.image.size || delay.Size > consumer.image.size - delay.VirtualAddress) { error = "delay import directory exceeds image"; return false; }
            const std::size_t end = static_cast<std::size_t>(delay.VirtualAddress) + delay.Size;
            for (std::size_t cursor = delay.VirtualAddress; cursor + sizeof(DelayImportDescriptor) <= end; cursor += sizeof(DelayImportDescriptor)) {
                const auto* descriptor = reinterpret_cast<const DelayImportDescriptor*>(consumer.image.base + cursor);
                if (descriptor->name == 0 && descriptor->importAddressTable == 0) break;
                if ((descriptor->attributes & 1U) == 0 || descriptor->name >= consumer.image.size || descriptor->importAddressTable >= consumer.image.size) { error = "unsupported delay import descriptor"; return false; }
                const char* moduleName = reinterpret_cast<const char*>(consumer.image.base + descriptor->name);
                MappedModule* internal = find(moduleName);
                HMODULE system = nullptr;
                if (internal == nullptr) {
                    system = LoadLibraryA(moduleName);
                    if (!system) { error = std::string("could not load delayed import: ") + moduleName; return false; }
                    systemModules_.push_back(system);
                }
                const std::size_t lookupRva = descriptor->importNameTable ? descriptor->importNameTable : descriptor->importAddressTable;
                for (std::size_t index = 0;; ++index) {
                    const std::size_t offset = index * sizeof(IMAGE_THUNK_DATA64);
                    if (lookupRva > consumer.image.size || offset > consumer.image.size - lookupRva || sizeof(IMAGE_THUNK_DATA64) > consumer.image.size - lookupRva - offset
                        || descriptor->importAddressTable > consumer.image.size || offset > consumer.image.size - descriptor->importAddressTable || sizeof(IMAGE_THUNK_DATA64) > consumer.image.size - descriptor->importAddressTable - offset) { error = "unterminated delay import thunk table"; return false; }
                    auto* lookup = reinterpret_cast<IMAGE_THUNK_DATA64*>(consumer.image.base + lookupRva + offset);
                    auto* address = reinterpret_cast<IMAGE_THUNK_DATA64*>(consumer.image.base + descriptor->importAddressTable + offset);
                    if (lookup->u1.AddressOfData == 0) break;
                    FARPROC target = nullptr;
                    if (IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal)) {
                        if (internal) { error = "internal delay ordinal imports are not supported"; return false; }
                        target = GetProcAddress(system, reinterpret_cast<const char*>(IMAGE_ORDINAL64(lookup->u1.Ordinal)));
                    } else {
                        if (lookup->u1.AddressOfData >= consumer.image.size) { error = "delay import-by-name RVA exceeds image"; return false; }
                        auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(consumer.image.base + lookup->u1.AddressOfData);
                        target = internal ? findMappedExport(internal->image, reinterpret_cast<const char*>(imported->Name), error)
                                          : GetProcAddress(system, reinterpret_cast<const char*>(imported->Name));
                    }
                    if (!target) { if (error.empty()) error = std::string("could not resolve delayed import from ") + moduleName; return false; }
                    address->u1.Function = reinterpret_cast<ULONGLONG>(target);
                }
            }
        }
        consumer.image.importsResolved = true; error.clear(); return true;
    }
    bool dependencyOrder(std::vector<MappedModule*>& order, std::string& error) {
        order.clear(); std::vector<MappedModule*> visiting;
        auto visit = [&](auto&& self, MappedModule& module) -> bool {
            if (std::find(order.begin(), order.end(), &module) != order.end()) return true;
            if (std::find(visiting.begin(), visiting.end(), &module) != visiting.end()) { error = "cyclic internal DLL dependency"; return false; }
            visiting.push_back(&module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.image.base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.image.base + dos->e_lfanew);
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            for (std::size_t cursor = dir.VirtualAddress; cursor + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= dir.VirtualAddress + dir.Size; cursor += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
                const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(module.image.base + cursor);
                if (desc->Name == 0 && desc->FirstThunk == 0) break;
                if (auto* dependency = find(reinterpret_cast<const char*>(module.image.base + desc->Name)); dependency != nullptr && !self(self, *dependency)) return false;
            }
            visiting.pop_back(); order.push_back(&module); return true;
        };
        for (auto& module : modules_) if (!visit(visit, module)) return false;
        return true;
    }
    bool initializeInDependencyOrder(std::string& error) {
        std::vector<MappedModule*> order;
        if (!dependencyOrder(order, error)) return false;
        for (MappedModule* module : order) {
            if (!resolveImports(*module, error) || !protectMappedImage(module->image, error)
                || !registerExceptionTable(module->image, error) || !initializeMappedImage(module->image, error)) return false;
        }
        return true;
    }
    bool invokeJniOnLoadInDependencyOrder(void* javaVm, std::string& error) {
        std::vector<MappedModule*> order;
        if (!dependencyOrder(order, error)) return false;
        for (MappedModule* module : order) {
            if (!invokeJniOnLoad(module->image, javaVm, error)) return false;
        }
        return true;
    }
    std::vector<MappedModule>& modules() noexcept { return modules_; }
    const std::vector<MappedModule>& modules() const noexcept { return modules_; }
private: std::vector<MappedModule> modules_; std::vector<HMODULE> systemModules_;
};

} // namespace j2d::memory
