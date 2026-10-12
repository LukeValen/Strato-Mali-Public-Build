// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>
#include "compressed_storage.h"
using namespace skyline::vfs;
template<class T> void Put(std::vector<uint8_t> &v, size_t p, T x) { std::memcpy(v.data()+p,&x,sizeof(x)); }
template<class F> void Reject(F f) { bool threw=false; try {f();} catch(const std::runtime_error &) {threw=true;} assert(threw); }
int main(int argc,char **argv) {
    if(argc==5) {
        std::ifstream file(argv[1],std::ios::binary);
        std::vector<uint8_t> table((std::istreambuf_iterator<char>(file)),{});
        compression::Storage storage(table,std::stoul(argv[3]),std::stoull(argv[4]));
        std::ifstream data(argv[2],std::ios::binary);
        auto read=[&](uint8_t *p,size_t n,uint64_t o) {data.seekg(o);data.read(reinterpret_cast<char *>(p),n);if(size_t(data.gcount())!=n)throw std::runtime_error("short source");};
        std::vector<uint8_t> header(80);
        storage.Read(header.data(),header.size(),0,read);
        assert(patch::Value<uint64_t>(header,0)==80);
        for(size_t i=1;i<9;i+=2) {
            auto offset=patch::Value<uint64_t>(header,i*8), length=patch::Value<uint64_t>(header,(i+1)*8);
            assert(length<16*1024*1024);
            std::vector<uint8_t> out(length);
            storage.Read(out.data(),out.size(),offset,read);
            std::cout<<"RomFS metadata read: "<<offset<<" + "<<length<<"\n";
        }
        std::cout<<"PASS real compression table, header and all four metadata tables; virtual size "<<storage.Size()<<"\n";
        return 0;
    }
    std::vector<uint8_t> bytes(0x10000,0x53), physical(0x20000), table(0xc000);
    int compressed=LZ4_compress_default(reinterpret_cast<char *>(bytes.data()),reinterpret_cast<char *>(physical.data()+16),bytes.size(),physical.size()-16);
    assert(compressed>0);physical[0]=11;physical[1]=22;
    uint64_t limit=(uint64_t{1}<<34)+2;
    Put<uint32_t>(table,4,2);Put<uint64_t>(table,8,limit);Put<uint64_t>(table,16,0);Put<uint64_t>(table,24,2+bytes.size());
    Put<uint32_t>(table,0x4004,2);Put<uint64_t>(table,0x4008,2+bytes.size());
    Put<uint64_t>(table,0x4010,0); // raw two-byte range
    Put<uint64_t>(table,0x4028,2);Put<uint64_t>(table,0x4030,16);table[0x4038]=3;Put<int32_t>(table,0x403c,compressed);
    Put<uint32_t>(table,0x8004,1);Put<uint64_t>(table,0x8008,limit);Put<uint64_t>(table,0x8010,2+bytes.size());table[0x8020]=1;
    compression::Storage storage(table,3,physical.size());
    size_t reads=0;
    auto source=[&](uint8_t *p,size_t n,uint64_t o) {assert(o<=physical.size()&&n<=physical.size()-o);++reads;std::memcpy(p,physical.data()+o,n);};
    std::vector<uint8_t> out(bytes.size()+4,0xaa);
    storage.Read(out.data(),out.size(),0,source);
    assert(out[0]==11&&out[1]==22&&out[out.size()-1]==0&&out[out.size()-2]==0);
    assert(std::equal(bytes.begin(),bytes.end(),out.begin()+2));assert(reads==2);
    storage.Read(out.data(),20,3,source);assert(reads==2); // decoded block cache
    storage.Read(out.data(),1,limit-1,source);assert(out[0]==0&&reads==2);
    storage.Read(nullptr,0,limit,source);
    Reject([&]{storage.Read(out.data(),1,limit,source);});
    Reject([&]{storage.Read(out.data(),1,UINT64_MAX,source);});
    auto invalid=[&](auto mutate) {auto t=table;mutate(t);Reject([&]{compression::Storage bad(t,3,physical.size());});};
    invalid([](auto&t){t.resize(30);});
    invalid([](auto&t){Put<uint32_t>(t,4,2047);});
    invalid([](auto&t){Put<uint32_t>(t,0x4004,0);});
    invalid([](auto&t){Put<uint64_t>(t,0x4028,0);});
    invalid([](auto&t){t[0x4038]=2;});
    invalid([](auto&t){Put<int32_t>(t,0x403c,-1);});
    invalid([](auto&t){Put<uint64_t>(t,0x4030,17);});
    invalid([](auto&t){Put<uint64_t>(t,0x4030,0x20000);});
    invalid([](auto&t){Put<uint64_t>(t,0x4018,UINT64_MAX);});
    invalid([](auto&t){Put<uint64_t>(t,24,0x10003);Put<uint64_t>(t,0x4008,0x10003);Put<uint64_t>(t,0x8010,0x10003);});
    compression::Storage corrupt(table,3,physical.size());
    auto saved=physical;std::fill(physical.begin()+16,physical.end(),0);
    Reject([&]{corrupt.Read(out.data(),1,2,source);});
    physical=saved;corrupt.Read(out.data(),1,2,source);assert(out[0]==0x53);
    compression::Storage shortRead(table,3,physical.size());
    Reject([&]{shortRead.Read(out.data(),1,2,[](auto,auto,auto){throw std::runtime_error("short read");});});
    std::vector<std::thread> workers;
    for(int i=0;i<4;++i) workers.emplace_back([&]{std::array<uint8_t,100> b{};for(int j=0;j<100;++j){storage.Read(b.data(),b.size(),2+j,source);for(auto v:b)assert(v==0x53);}});
    for(auto &t:workers)t.join();
    std::cout<<"PASS mixed ranges, LZ4, cache, concurrent reads, 64-bit size, EOF and malformed tables/blocks\n";
}
