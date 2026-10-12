#!/usr/bin/env python3
"""Exercise the production compute job against the existing CPU BCn decoder."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
texture = root / 'app/src/main/cpp/skyline/gpu/texture'
vkhpp = root / 'app/libraries/vkhpp'
includes = [root / 'tests/bcn_host', vkhpp, vkhpp / 'Vulkan-Headers/include', texture]
with tempfile.TemporaryDirectory() as temporary:
    binary = Path(temporary) / 'bcn_compute_test'
    command = [os.environ.get('CXX', 'g++'), '-std=c++20', '-O1', '-DNDEBUG', '-Wno-attributes']
    command += ['-I' + str(directory) for directory in includes]
    command += [str(root / 'tests/bcn_host/test.cpp'), str(texture / 'bcn_compute.cpp'), str(texture / 'bc_decoder.cpp'), '-ldl', '-pthread', '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
