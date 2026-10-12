#!/usr/bin/env python3
"""Validate resolution ratio calculations against all requested scales and Vulkan limits."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#include <cassert>
#include <climits>
#include "gpu/resolution_scaling.h"
#include "gpu/resolution_budget.h"
#include "gpu/resolution_shader.h"
#include <shader_compiler/common/settings.h>
extern bool ReadShaderSettings();
using skyline::gpu::resolution::Scale;
int main(){
 Shader::Settings::values.resolution_info.active = true;
 Shader::Settings::values.resolution_info.up_scale = 3;
 Shader::Settings::values.resolution_info.down_shift = 2;
 assert(ReadShaderSettings());
 skyline::gpu::resolution::ShaderScale uniforms;
 assert(uniforms.downFactor == 1.0f);
 assert(uniforms.SetTexture(0) && uniforms.SetTexture(31) && uniforms.SetTexture(32) && uniforms.SetTexture(127));
 assert(uniforms.textures[0] == 0x80000001u && uniforms.textures[1] == 1u && uniforms.textures[3] == 0x80000000u);
 assert(!uniforms.SetTexture(128) && !uniforms.SetTexture(UINT_MAX));

 std::atomic<std::uint64_t> allocated{0};
 {
  skyline::gpu::resolution::Reservation full(allocated, skyline::gpu::resolution::Reservation::Limit);
  assert(full.reserved && allocated == skyline::gpu::resolution::Reservation::Limit);
  skyline::gpu::resolution::Reservation overflow(allocated, 1); assert(!overflow.reserved);
 }
 assert(allocated == 0);
 skyline::gpu::resolution::Reservation tooBig(allocated, UINT64_MAX); assert(!tooBig.reserved);

 for(int i=0;i<7;++i){
  auto scale=Scale::FromSetting(i);
  auto e=scale.TryExtent({1280,720},{4096,4096});
  assert(e&&e->width==320u*(i+2)&&e->height==180u*(i+2));
  assert(scale.Factor()==(i+2)/4.0f);
  assert(scale.IsNative()==(i==2));
  auto tiny=scale.TryExtent({1,1},{4096,4096});assert(tiny&&tiny->width>=1&&tiny->height>=1);
  assert(!scale.TryExtent({0,720},{4096,4096}));
  assert(!scale.TryExtent({1280,0},{4096,4096}));
 }
 for(int index:{-1,7,INT_MIN,INT_MAX})assert(Scale::FromSetting(index).IsNative());
 assert(!Scale::FromSetting(6).TryExtent({UINT_MAX,UINT_MAX},{UINT_MAX,UINT_MAX}));
 assert(!Scale::FromSetting(6).TryExtent({2560,1440},{4096,4096}));
 assert(!Scale::FromSetting(2).TryExtent({1280,720},{1279,720}));
 auto odd=Scale::FromSetting(0).TryExtent({1279,719},{4096,4096});assert(odd&&odd->width==639&&odd->height==359);
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(source)
 (p/'settings.cpp').write_text('''#include <shader_compiler/common/settings.h>
bool ReadShaderSettings() {
 const auto &scale = Shader::Settings::values.resolution_info;
 return scale.active && scale.up_scale == 3 && scale.down_shift == 2;
}
''')
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                 '-I'+str(root/'app/src/main/cpp/skyline'),'-I'+str(root/'app/src/main/cpp/shader_compiler_overrides'),
                 '-I'+str(root/'tests/shader_include'),str(p/'test.cpp'),str(p/'settings.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: all seven ratios, native default, tiny/odd extents, overflow, device limits, shader ABI and cross-TU settings')
