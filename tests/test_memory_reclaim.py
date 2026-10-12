#!/usr/bin/env python3
"""Exercise real shared mappings: zeroed aliases, protected VMAs, intact neighbors."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#include <sys/mman.h>
#include <cerrno>
// New Linux kernels accept VM_MAYWRITE here. Inject the EACCES observed on
// Android for the original VMA only; alias creation/reclamation remain real.
static void *denyAddress{};
static int OldKernelMadvise(void *address, size_t size, int advice) {
    if (address == denyAddress && advice == MADV_REMOVE) { errno=EACCES; return -1; }
    return madvise(address,size,advice);
}
#define madvise OldKernelMadvise
#include "app/src/main/cpp/skyline/kernel/memory_reclaim.h"
#undef madvise
#include <cassert>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
// A nested kernel::memory would shadow skyline::memory in production headers.
namespace skyline::memory { struct NamespaceLookupProbe {}; }
namespace skyline::kernel { using MemoryLookupProbe = memory::NamespaceLookupProbe; }
using skyline::kernel::memoryReclaim::ReclaimSharedPages;
std::string permissions(void* address) {
    std::ifstream maps("/proc/self/maps"); std::string line;
    while(std::getline(maps,line)) {
        unsigned long start,end; char perms[5]{};
        if(std::sscanf(line.c_str(),"%lx-%lx %4s",&start,&end,perms)==3 &&
           reinterpret_cast<unsigned long>(address)>=start && reinterpret_cast<unsigned long>(address)<end) return perms;
    }
    assert(false); return {};
}
int main() {
    const size_t page=sysconf(_SC_PAGESIZE);
    for(int protect:{PROT_READ,PROT_NONE,PROT_READ|PROT_WRITE}) {
        auto* original=static_cast<unsigned char*>(mmap(nullptr,3*page,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0));
        assert(original!=MAP_FAILED); std::memset(original,0xA5,3*page);
        auto* alias=static_cast<unsigned char*>(mremap(original,0,3*page,MREMAP_MAYMOVE));
        assert(alias!=MAP_FAILED);
        assert(mprotect(original+page,page,protect)==0);
        const auto before=permissions(original+page);
        denyAddress = protect != (PROT_READ|PROT_WRITE) ? original+page : nullptr;
        assert(ReclaimSharedPages(original+page,page)==0);
        assert(permissions(original+page)==before);
        for(size_t i=0;i<3*page;++i) assert(alias[i]==(i>=page&&i<2*page?0:0xA5));
        assert(mprotect(original+page,page,PROT_READ|PROT_WRITE)==0);
        original[page]=0x37; assert(alias[page]==0x37); // still aliases the same backing
        assert(munmap(alias,3*page)==0); assert(munmap(original,3*page)==0);
    }
    denyAddress=nullptr;
    assert(ReclaimSharedPages(nullptr,0)==0);
    // Non-shared mappings must fail, not be silently replaced with private pages.
    auto* privateMap=static_cast<unsigned char*>(mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(privateMap!=MAP_FAILED); privateMap[0]=0x6B;
    assert(ReclaimSharedPages(privateMap,page)!=0 && privateMap[0]==0x6B);
    munmap(privateMap,page);
    // A permission split in one shared backing must not discard neighboring pages.
    auto* split=static_cast<unsigned char*>(mmap(nullptr,4*page,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0));
    assert(split!=MAP_FAILED); std::memset(split,0x55,4*page);
    assert(mprotect(split+page,page,PROT_READ)==0);
    denyAddress=split;
    assert(ReclaimSharedPages(split,3*page)==0);
    for(size_t i=0;i<3*page;++i) assert(split[i]==0);
    assert(split[3*page]==0x55);
    munmap(split,4*page);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'test.cpp';p.write_text(source);out=Path(tmp)/'test'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(root),str(p),'-o',str(out)],check=True)
 subprocess.run([str(out)],check=True)
print('PASS: shared page reclamation; read-only/no-access protections; aliases; neighbors; split VMAs; private mapping rejection')
