#!/usr/bin/env python3
"""Check the production HOS Result ABI, including the KOF XIII raw error."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#include <cstddef>
#include <optional>
#define PAGE_SIZE 4096
#include "app/src/main/cpp/skyline/common/result.h"
#include <cassert>
int main() {
    static_assert(sizeof(skyline::Result)==4);
    assert(skyline::Result{}.raw==0);
    skyline::Result kof{3535362U};
    assert(kof.module==2 && kof.id==6905);
    for(unsigned module:{0,1,2,128,511}) for(unsigned id=0;id<8192;++id) {
        skyline::Result built{static_cast<skyline::u16>(module),static_cast<skyline::u16>(id)};
        assert(built.raw==(module|(id<<9)));
        skyline::Result parsed{built.raw};
        assert(parsed.module==module && parsed.id==id);
    }
    skyline::Result reserved{0xFFFFFFFFU};
    assert(reserved.module==511 && reserved.id==8191);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'test.cpp';p.write_text(source);out=Path(tmp)/'test'
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-I',str(root),str(p),'-o',str(out)],check=True)
 subprocess.run([str(out)],check=True)
print('PASS: 13-bit result descriptions, KOF NotMounted, reserved bits, 4-byte ABI')
