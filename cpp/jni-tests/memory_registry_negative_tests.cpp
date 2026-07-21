#include "../maindll/maindll/memory_pe.hpp"
#include <fstream>
#include <iterator>
#include <iostream>
#include <vector>
static std::vector<unsigned char> r(const char* p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char**argv){
    if(argc!=3)return 2; std::string e; auto p=r(argv[1]), c=r(argv[2]);
    j2d::memory::MappedImage provider, consumer, duplicate;
    if(!j2d::memory::mapRelocatedImage(p.data(),p.size(),provider,e)||!j2d::memory::mapRelocatedImage(c.data(),c.size(),consumer,e)||!j2d::memory::mapRelocatedImage(p.data(),p.size(),duplicate,e))return 3;
    j2d::memory::ModuleRegistry registry;
    if(!registry.add("j2d_fixture_provider.dll",std::move(provider)))return 4;
    if(registry.add("J2D_FIXTURE_PROVIDER.DLL",std::move(duplicate)))return 5;
    if(j2d::memory::resolveOneMappedImport(consumer,"missing_provider.dll",registry.find("j2d_fixture_provider.dll")->image,e))return 6;
    std::cout<<"J2D_MEMORY_REGISTRY_NEGATIVE_OK\n";
}
