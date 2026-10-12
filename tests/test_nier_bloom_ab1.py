#!/usr/bin/env python3
"""Compile/run a host-only smoke test for the isolated NieR bloom A/B rewrite."""
from pathlib import Path
import subprocess
import tempfile
import shutil
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "app/src/main/cpp/skyline/gpu/nier_bloom_ab1.h"

class BloomABTest(unittest.TestCase):
    def test_safe_guard_and_isolation(self):
        header = HEADER.read_text()
        manager = (ROOT/"app/src/main/cpp/skyline/gpu/shader_manager.cpp").read_text()
        gpu = (ROOT/"app/src/main/cpp/skyline/gpu.cpp").read_text()
        self.assertIn("stores.size() != 4", header)
        self.assertIn("pointers.size() != 1", header)
        self.assertIn("nier_bloom::SuppressBrightPass", manager)
        self.assertIn("0xF4215778B3A6FB13ULL", manager)
        self.assertIn("gpu.nierDiagnosticsEnabled", manager)
        self.assertIn("program.stage == Shader::Stage::Fragment", manager)
        self.assertIn("_nier_mali_geom_invocation_v1_bloom_ab1", gpu)
        self.assertIn("state.loader->programId == 0x0100B8E016F76000ULL", gpu)
        self.assertIn("traits.vendorId == 0x13B5U", gpu)
        self.assertNotIn("isberd-nier-ab1", (ROOT/".github/workflows/strato-mali-build.yml").read_text())

    def test_host_cpp_patch(self):
        cc = shutil.which("clang++") or shutil.which("g++")
        if not cc:
            self.skipTest("C++ compiler unavailable locally")
        code = r"""
#include <cassert>
#include <cstdint>
#include <vector>
#include <gpu/nier_bloom_ab1.h>
using std::uint32_t;
void emit(std::vector<uint32_t>& w, uint32_t op, std::initializer_list<uint32_t> args) {
    w.push_back((static_cast<uint32_t>(args.size()+1)<<16)|op);
    w.insert(w.end(), args.begin(), args.end());
}
int main(){
    std::vector<uint32_t> shader = {0x07230203, 0x00010400, 0, 100, 0};
    emit(shader,15,{4,12,0});  // fragment shader
    emit(shader,71,{11,30,1}); // output RT1 Location 1
    emit(shader,71,{10,30,0}); // primary color Location 0
    emit(shader,22,{1,32});    // float32 type 1
    emit(shader,32,{2,3,1});  // output float pointer type 2
    emit(shader,43,{1,3,0});  // zero float id=3
    emit(shader,59,{4,11,3}); // output location 1
    emit(shader,59,{4,10,3}); // output location 0
    for (uint32_t i=0;i<4;i++) {
        emit(shader,65,{2,20+i,11,i});
        emit(shader,62,{20+i,30+i});
    }
    emit(shader,62,{10,55}); // primary color must survive
    auto before=shader;
    assert(skyline::gpu::nier_bloom::SuppressBrightPass(shader));
    unsigned old_stores=0, new_stores=0, original_primary=0;
    for(size_t i=5;i<shader.size();) {
        auto size=shader[i]>>16;
        if((shader[i]&0xFFFF)==62) {
            if(shader[i+1]>=20 && shader[i+1]<=23) {
                ++new_stores;
                assert(shader[i+2]==3);
            }
            if(shader[i+1]==10) {
                ++original_primary;
                assert(shader[i+2]==55);
            }
        }
        i+=size;
    }
    assert(new_stores==4 && original_primary==1);
    auto malformed=before; malformed[0]=0; assert(!skyline::gpu::nier_bloom::SuppressBrightPass(malformed));
    auto truncated=before; truncated.pop_back(); assert(!skyline::gpu::nier_bloom::SuppressBrightPass(truncated));
    // No output location 1 -> no modification
    auto different=before;
    for (size_t i=5;i<different.size();) {
        auto size=different[i]>>16;
        if ((different[i]&0xFFFF)==71 && different[i+1]==11) {different[i+3]=2;break;}
        i+=size;
    }
    auto copy=different; assert(!skyline::gpu::nier_bloom::SuppressBrightPass(different));
    assert(different==copy);
}
"""
        with tempfile.TemporaryDirectory() as td:
            source=Path(td)/"probe.cpp"
            exe=Path(td)/"probe"
            source.write_text(code)
            subprocess.run([cc, "-std=c++17", "-O1", "-I", str(ROOT/"app/src/main/cpp/skyline"),
                            str(source), "-o", str(exe)], check=True, timeout=90)
            subprocess.run([str(exe)], check=True, timeout=30)

if __name__=="__main__":
    unittest.main()
