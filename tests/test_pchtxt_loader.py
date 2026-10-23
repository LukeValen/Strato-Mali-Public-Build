#!/usr/bin/env python3
"""Exercise the native IPSwitch parser with the reported Persona 5 patch."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = root / "app/src/main/cpp/skyline/loader/pchtxt.h"
source = r'''
#include <cassert>
#include <sstream>
#include "pchtxt.h"
int main() {
    std::array<uint8_t, 32> id{};
    const char *hex = "357EFC198E728CB5D55265AFF304F03CFC9CC0E3";
    for (size_t i = 0; i < 20; ++i) id[i] = (skyline::loader::mods::HexDigit(hex[2*i]) << 4) |
                                                 skyline::loader::mods::HexDigit(hex[2*i+1]);
    const char *persona = R"(@nsobid-357EFC198E728CB5D55265AFF304F03CFC9CC0E3
@flag print_values
@flag offset_shift 0x100
// 60fps
@enabled
00486DA0 28008052
@stop
@StevensND
)";
    std::istringstream input(persona);
    auto patches = skyline::loader::mods::ParsePatchText(input, id);
    assert(patches.size() == 1 && patches[0].offset == 0x486ea0);
    assert((patches[0].bytes == std::vector<uint8_t>{0x28,0x00,0x80,0x52}));
    id[0] ^= 1;
    std::istringstream mismatch(persona);
    assert(skyline::loader::mods::ParsePatchText(mismatch, id).empty());
    std::istringstream malformed("@nsobid-357EFC198E728CB5D55265AFF304F03CFC9CC0E3\n@enabled\n00486DA0 GG\n");
    bool rejected = false;
    try { skyline::loader::mods::ParsePatchText(malformed, id); }
    catch (const std::exception &) { rejected = true; }
    assert(rejected);
}
'''
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    (path / "test.cpp").write_text(source)
    subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(header.parent),
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: Persona pchtxt offset, Build ID check, malformed patch rejection")
