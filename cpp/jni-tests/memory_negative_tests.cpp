#include "../maindll/maindll/memory_pe.hpp"
#include <fstream>
#include <iterator>
#include <iostream>
#include <vector>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> valid((std::istreambuf_iterator<char>(file)), {});
    std::string error; j2d::memory::PeImageInfo info;
    if (valid.size() < sizeof(IMAGE_DOS_HEADER) || !j2d::memory::inspectPe64(valid.data(), valid.size(), info, error)) return 3;
    std::vector<unsigned char> truncated(valid.begin(), valid.begin() + sizeof(IMAGE_DOS_HEADER));
    if (j2d::memory::inspectPe64(truncated.data(), truncated.size(), info, error)) return 4;
    auto x86 = valid;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(x86.data());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(x86.data() + dos->e_lfanew);
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    if (j2d::memory::inspectPe64(x86.data(), x86.size(), info, error)) return 5;
    auto malformed = valid;
    auto* malformedDos = reinterpret_cast<IMAGE_DOS_HEADER*>(malformed.data());
    auto* malformedNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(malformed.data() + malformedDos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(malformedNt);
    section->PointerToRawData = static_cast<DWORD>(malformed.size()); section->SizeOfRawData = 1;
    if (j2d::memory::inspectPe64(malformed.data(), malformed.size(), info, error)) return 6;
    auto virtualOverflow = valid;
    auto* overflowDos = reinterpret_cast<IMAGE_DOS_HEADER*>(virtualOverflow.data());
    auto* overflowNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(virtualOverflow.data() + overflowDos->e_lfanew);
    auto* overflowSection = IMAGE_FIRST_SECTION(overflowNt);
    overflowSection->VirtualAddress = overflowNt->OptionalHeader.SizeOfImage;
    overflowSection->Misc.VirtualSize = 1;
    if (j2d::memory::inspectPe64(virtualOverflow.data(), virtualOverflow.size(), info, error)) return 7;
    j2d::memory::MappedImage mapped;
    auto invalidImports = valid;
    auto* importDos = reinterpret_cast<IMAGE_DOS_HEADER*>(invalidImports.data());
    auto* importNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(invalidImports.data() + importDos->e_lfanew);
    importNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = importNt->OptionalHeader.SizeOfImage;
    importNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = sizeof(IMAGE_IMPORT_DESCRIPTOR);
    if (j2d::memory::mapRelocatedImage(invalidImports.data(), invalidImports.size(), mapped, error)) return 8;
    auto delayed = valid;
    auto* delayedDos = reinterpret_cast<IMAGE_DOS_HEADER*>(delayed.data());
    auto* delayedNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(delayed.data() + delayedDos->e_lfanew);
    delayedNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].Size = 1;
    if (!j2d::memory::inspectPe64(delayed.data(), delayed.size(), info, error) || !j2d::memory::validateMapperCompatibility(info, error)) return 9;
    if (!j2d::memory::mapRelocatedImage(valid.data(), valid.size(), mapped, error)) return 10;
    auto* mappedDos = reinterpret_cast<IMAGE_DOS_HEADER*>(mapped.base);
    auto* mappedNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(mapped.base + mappedDos->e_lfanew);
    mappedNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = 1;
    if (j2d::memory::registerExceptionTable(mapped, error)) return 11;
    std::cout << "J2D_MEMORY_NEGATIVE_INPUT_OK\n";
}
