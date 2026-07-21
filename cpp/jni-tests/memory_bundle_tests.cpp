#include "../maindll/maindll/memory_bundle.hpp"
#include <iostream>
#include <vector>

static void u32(std::vector<unsigned char>& bytes, std::uint32_t value) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>(value >> (i * 8))); }
int main() {
    std::vector<unsigned char> bytes{'N','2','D','M',1}; u32(bytes, 2);
    u32(bytes, 5); bytes.insert(bytes.end(), {'a','.','d','l','l'}); u32(bytes, 2); bytes.insert(bytes.end(), {1,2});
    u32(bytes, 5); bytes.insert(bytes.end(), {'b','.','d','l','l'}); u32(bytes, 1); bytes.push_back(3);
    std::vector<j2d::memory::BundleModuleView> modules; std::string error;
    if (!j2d::memory::parseNativeBundle(bytes.data(), bytes.size(), modules, error)) return 1;
    if (modules.size() != 2 || modules[0].name != "a.dll" || modules[1].size != 1 || modules[1].data[0] != 3) return 2;
    bytes.pop_back();
    if (j2d::memory::parseNativeBundle(bytes.data(), bytes.size(), modules, error)) return 3;
    bytes = {'N','2','D','M',1}; u32(bytes, 2);
    u32(bytes, 5); bytes.insert(bytes.end(), {'a','.','d','l','l'}); u32(bytes, 1); bytes.push_back(1);
    u32(bytes, 5); bytes.insert(bytes.end(), {'A','.','D','L','L'}); u32(bytes, 1); bytes.push_back(2);
    if (j2d::memory::parseNativeBundle(bytes.data(), bytes.size(), modules, error)) return 4;
    bytes = {'N','2','D','M',1}; u32(bytes, 1);
    u32(bytes, 9); bytes.insert(bytes.end(), {'x','/','y','.','d','l','l','!','!'}); u32(bytes, 1); bytes.push_back(1);
    if (j2d::memory::parseNativeBundle(bytes.data(), bytes.size(), modules, error)) return 5;
    std::cout << "J2D_MEMORY_BUNDLE_OK\n";
}
