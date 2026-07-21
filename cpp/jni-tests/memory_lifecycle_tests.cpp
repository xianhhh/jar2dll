#include "../maindll/maindll/memory_pe.hpp"
#include <fstream>
#include <iterator>
#include <iostream>
#include <vector>
#include <cstdlib>
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    const int requested = argc == 3 ? std::atoi(argv[2]) : 1;
    const int rounds = requested > 0 ? requested : 1;
    for (int round = 0; round < rounds; ++round) {
        std::string error; j2d::memory::MappedImage image;
        if (!j2d::memory::mapRelocatedImage(bytes.data(), bytes.size(), image, error)) { std::cerr << error; return 3; }
        if (!j2d::memory::resolveSystemImports(image, error)) return 4;
        j2d::memory::ModuleRegistry registry;
        if (!registry.add("j2d_lifecycle_fixture.dll", std::move(image)) || !registry.initializeInDependencyOrder(error)) { std::cerr << error; return 5; }
        if (!registry.initializeInDependencyOrder(error)) { std::cerr << error; return 6; }
        if (!registry.invokeJniOnLoadInDependencyOrder(nullptr, error)) { std::cerr << error; return 6; }
        if (!registry.invokeJniOnLoadInDependencyOrder(nullptr, error)) { std::cerr << error; return 7; }
        using Attached = int(*)();
        auto attached = reinterpret_cast<Attached>(j2d::memory::findMappedExport(registry.find("j2d_lifecycle_fixture.dll")->image, "j2d_lifecycle_attached", error));
        if (attached == nullptr || attached() != 1) return 8;
    }
    std::cout << "J2D_MEMORY_LIFECYCLE_OK rounds=" << rounds << '\n';
}
