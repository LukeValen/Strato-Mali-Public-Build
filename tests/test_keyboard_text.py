#!/usr/bin/env python3
"""Exercise actual swkbd encoding with exact output and guarded buffer boundaries."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = r'''
#include "app/src/main/cpp/skyline/applet/swkbd/software_keyboard_text.h"
#include <array>
#include <cassert>
#include <cstring>
#include <string>
using skyline::applet::swkbd::EncodeKeyboardText;
int main() {
    std::array<std::uint8_t, 32> b{};
    EncodeKeyboardText(b.data(), b.size(), u"João 🫐", true);
    assert(std::strcmp(reinterpret_cast<char*>(b.data()), "João 🫐") == 0);
    const std::u16string text{u"A🫐B"};
    for (std::size_t capacity = 0; capacity <= 12; ++capacity) {
        b.fill(0xCD);
        EncodeKeyboardText(b.data() + 1, capacity, text, true);
        assert(b[0] == 0xCD && b[capacity + 1] == 0xCD);
        if (capacity > 0) {
            const auto expected = capacity < 2 ? "" : capacity < 6 ? "A" : capacity < 7 ? "A🫐" : "A🫐B";
            assert(std::strcmp(reinterpret_cast<char*>(b.data() + 1), expected) == 0);
        }
        b.fill(0xCD);
        EncodeKeyboardText(b.data() + 1, capacity, text, false);
        assert(b[0] == 0xCD && b[capacity + 1] == 0xCD);
        if (capacity >= 2) {
            const std::u16string expected{capacity < 4 ? u"" : capacity < 8 ? u"A" : capacity < 10 ? u"A🫐" : u"A🫐B"};
            assert(std::memcmp(b.data()+1, expected.c_str(), (expected.size()+1)*2) == 0);
        }
    }
    EncodeKeyboardText(b.data(), b.size(), std::u16string(1, 0xD800), true);
    assert(std::strcmp(reinterpret_cast<char*>(b.data()), "\xEF\xBF\xBD") == 0);
    EncodeKeyboardText(b.data(), b.size(), u"", true);
    assert(b[0] == 0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / 'test.cpp'
    cpp.write_text(source)
    exe = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(root), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: UTF-8, UTF-16, accents, emoji, truncation, empty and malformed text')
