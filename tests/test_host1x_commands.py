#!/usr/bin/env python3
"""Validate bounded CDMA decoding used by the video diagnostics."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include <array>
#include <cassert>
#include <vector>
#include "soc/host1x/command_decoder.h"
using skyline::soc::host1x::DecodeCommands;
int main() {
 using U=uint32_t;using Write=std::array<U,3>;
 U cls=1;std::vector<Write> writes;
 auto decode=[&](std::vector<U> words){DecodeCommands(words,cls,[&](U c,U m,U a){writes.push_back({c,m,a});});};
 decode({(0x10u<<16)|(0xf0u<<6)|3,0x200,9});
 assert((writes==std::vector<Write>{{0xf0,0x10,0x200},{0xf0,0x11,9}}));
 writes.clear();decode({0x10100002,0x300,1});
 assert((writes==std::vector<Write>{{0xf0,0x10,0x300},{0xf0,0x11,1}}));
 writes.clear();decode({0x20110002,7,8,0x30000005,10,20,0x401200ff});
 assert((writes==std::vector<Write>{{0xf0,0x11,7},{0xf0,0x11,8},{0xf0,0,10},{0xf0,2,20},{0xf0,0x12,255}}));
 auto reject=[&](std::vector<U> v){bool threw=false;try{decode(v);}catch(const std::runtime_error&){threw=true;}assert(threw);};
 reject({0x10100002,1});reject({0x30100001});reject({(0xf0u<<6)|1});
 reject({0x1fff0002,1,2});reject({0x3fff0002,1});reject({0x60000000});
 writes.clear();decode({});assert(writes.empty());
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(source)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-g",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I" + str(root / "app/src/main/cpp/skyline"),
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: Host1x class persistence, method addressing, immediate/masked writes and truncated gathers")
