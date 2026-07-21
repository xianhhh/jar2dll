#include "../maindll/maindll/memory_pe.hpp"
#include <fstream>
#include <iterator>
#include <iostream>
#include <vector>
static std::vector<unsigned char> r(const char* p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int c,char**v){if(c!=4)return 2;std::string e;j2d::memory::ModuleRegistry x;for(int i=1;i<4;i++){auto b=r(v[i]);j2d::memory::MappedImage m;if(!j2d::memory::mapRelocatedImage(b.data(),b.size(),m,e))return 3;x.add(i==1?"j2d_chain_provider.dll":i==2?"j2d_chain_bridge.dll":"j2d_chain_jni.dll",std::move(m));}if(!x.resolveKnownImports(*x.find("j2d_chain_bridge.dll"),e)||!x.resolveKnownImports(*x.find("j2d_chain_jni.dll"),e)){std::cerr<<e;return 4;}std::vector<j2d::memory::MappedModule*> order;if(!x.dependencyOrder(order,e)||order.size()!=3||order[0]->name!="j2d_chain_provider.dll"||order[1]->name!="j2d_chain_bridge.dll"||order[2]->name!="j2d_chain_jni.dll")return 5;std::cout<<"J2D_MEMORY_REGISTRY_CHAIN_OK\n";}
