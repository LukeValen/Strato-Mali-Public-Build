#!/usr/bin/env python3
"""Render at seven scales, resolve color/depth/stencil, and check native readback with Vulkan validation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
glslang = os.environ.get('GLSLANG', shutil.which('glslangValidator') or shutil.which('glslang'))
if not glslang:
    raise SystemExit('Install glslang-tools before running the Vulkan integration test')
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'triangle.vert').write_text('''#version 450
vec2 positions[3] = vec2[](vec2(-1,-1), vec2(1,-1), vec2(-1,1));
void main() { gl_Position = vec4(positions[gl_VertexIndex], 0.25, 1); }
''')
    (path / 'triangle.frag').write_text('''#version 450
layout(location=0) out vec4 color;
layout(set=0, binding=0) uniform sampler2D sourceImage;
layout(push_constant) uniform ResolutionInfo {
 layout(offset=0) uint textures[4];
 layout(offset=16) uint images[2];
 layout(offset=24) float downFactor;
} scaling;
void main() {
 vec2 nativeCoord = gl_FragCoord.xy * scaling.downFactor;
 bool scaledImage = (scaling.textures[0] & 1u) != 0u;
 ivec2 coord = ivec2(nativeCoord * (scaledImage ? 1.0 / scaling.downFactor : 1.0));
 vec4 source = texelFetch(sourceImage, coord, 0);
 bool abiValid = scaling.textures[3] == 0x80000000u && scaling.images[0] == 0u && scaling.images[1] == 0u;
 color = vec4(abiValid ? source.r : 0.0, nativeCoord.x / 255.0, 0, 1);
}
''')
    for stage in ('vert', 'frag'):
        subprocess.run([glslang, '-V', str(path / ('triangle.' + stage)), '-o', str(path / (stage + '.spv'))], check=True)
    command = [os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g',
               '-DVULKAN_HPP_NO_STRUCT_CONSTRUCTORS', '-I' + str(root / 'app/src/main/cpp/skyline')]
    if os.environ.get('VULKAN_INCLUDE_DIR'):
        command += ['-I' + os.environ['VULKAN_INCLUDE_DIR']]
    command += [str(root / 'tests/resolution_vulkan.cpp'), '-l:libvulkan.so.1', '-ldl', '-pthread', '-o', str(path / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test'), str(path / 'vert.spv'), str(path / 'frag.spv')], check=True)
