#include "../maindll/maindll/memory_pe.hpp"
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iostream>
#include <vector>
static std::vector<unsigned char> read(const char* p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) return 2;
    auto providerBytes = read(argv[1]), consumerBytes = read(argv[2]); std::string error;
    const int requestedRounds = argc == 4 ? std::atoi(argv[3]) : 1;
    const int rounds = requestedRounds > 0 ? requestedRounds : 1;
    if (providerBytes.empty() || consumerBytes.empty()) return 3;
    for (int round = 0; round < rounds; ++round) {
        j2d::memory::MappedImage provider, consumer;
        if (!j2d::memory::mapRelocatedImage(providerBytes.data(), providerBytes.size(), provider, error)) return 4;
        if (!j2d::memory::mapRelocatedImage(consumerBytes.data(), consumerBytes.size(), consumer, error)) return 5;
        if (!j2d::memory::resolveOneMappedImport(consumer, "j2d_fixture_provider.dll", provider, error)) { std::cerr << error << '\n'; return 6; }
    }
    std::cout << "J2D_MEMORY_INTERNAL_IMPORT_OK rounds=" << rounds << '\n';
}
