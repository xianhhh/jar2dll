#include "../maindll/maindll/memory_pe.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: j2d_memory_pe_test <x64-dll> [export-name]\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.empty()) { std::cerr << "Could not read test DLL\n"; return 3; }

    j2d::memory::PeImageInfo info;
    std::string error;
    if (!j2d::memory::inspectPe64(bytes.data(), bytes.size(), info, error)) {
        std::cerr << "PE preflight failed: " << error << '\n'; return 4;
    }
    j2d::memory::MappedImage image;
    if (!j2d::memory::mapRelocatedImage(bytes.data(), bytes.size(), image, error)) {
        std::cerr << "PE mapping failed: " << error << '\n'; return 5;
    }
    if (!j2d::memory::resolveSystemImports(image, error)) {
        std::cerr << "Import resolution failed: " << error << '\n'; return 6;
    }
    if (!j2d::memory::protectMappedImage(image, error)) {
        std::cerr << "Section protection failed: " << error << '\n'; return 7;
    }
    if (!j2d::memory::registerExceptionTable(image, error)) {
        std::cerr << "Exception table registration failed: " << error << '\n'; return 8;
    }
    if (argc == 3 && j2d::memory::findMappedExport(image, argv[2], error) == nullptr) {
        std::cerr << "Export lookup failed: " << error << '\n'; return 9;
    }
    std::cout << "J2D_MEMORY_PE_MAP_IMPORT_OK sections=" << info.sectionCount << '\n';
    return 0;
}
