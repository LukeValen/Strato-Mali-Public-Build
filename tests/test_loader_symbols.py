#!/usr/bin/env python3
"""Exercise the production symbol resolver with ASan/UBSan, without Android."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "app/src/main/cpp/skyline/loader"
cpp = (SRC / "loader.cpp").read_text()
header = (SRC / "loader.h").read_text()
struct = header[header.index("        struct ExecutableSymbolicInfo {"):]
struct = struct[:struct.index("\n        };") + len("\n        };")]
resolver = cpp[cpp.index("    template<ElfSymbol ElfSym>\n    Loader::SymbolInfo Loader::ResolveSymbol"):]
resolver = resolver[:resolver.index("\n    inline std::string GetFunctionStackTrace")]
copy_block = cpp[cpp.index("            ExecutableSymbolicInfo symbolicInfo{"):]
copy_block = copy_block[:copy_block.index("\n        }\n") ]

harness = r"""
#include <algorithm>
#include <cassert>
#include <cstring>
#include <elf.h>
#include <iostream>
#include <span>
#include <string>
#include <vector>
#include <concepts>
#include <cstdint>
#include <stdexcept>
using u8 = uint8_t;
template<class T> struct span : std::span<T> {
    using std::span<T>::span;
    span(std::vector<T>& v) : std::span<T>(v) {}
    template<class U> span<U> cast() {
        if (this->size_bytes() % sizeof(U)) throw std::runtime_error("unaligned");
        return {reinterpret_cast<U*>(this->data()), this->size_bytes()/sizeof(U)};
    }
};
template<class T> span(std::vector<T>&) -> span<T>;
template<class T> concept ElfSymbol = std::same_as<T, Elf64_Sym> || std::same_as<T, Elf32_Sym>;
struct Loader {
STRUCT
    std::vector<ExecutableSymbolicInfo> executables;
    struct SymbolInfo { char* name{}; std::string_view executableName; };
    template<ElfSymbol T> SymbolInfo ResolveSymbol(void*);
    void add(u8* base, size_t size, std::vector<u8>& symbols, std::vector<char>& strings) {
        size_t hookSize=16;
        struct {size_t size;} patch{16};
        auto executableBase = base + patch.size + hookSize;
        std::string name="test.nso";
        span dynsym(symbols);
        span dynstr(strings);
COPY
    }
};
RESOLVER
int main() {
    std::vector<u8> memory(512);
    Loader loader;
    assert(!loader.ResolveSymbol<Elf64_Sym>(memory.data()).name);
    Elf64_Sym symbol{}; symbol.st_name=1; symbol.st_value=8; symbol.st_size=16;
    {
        std::vector<u8> symbols(sizeof(symbol)); std::memcpy(symbols.data(), &symbol, sizeof(symbol));
        std::vector<char> strings={'\0','t','e','s','t','\0'};
        loader.add(memory.data()+64, 128, symbols, strings);
        std::fill(symbols.begin(),symbols.end(),0xFE);
        std::fill(strings.begin(),strings.end(),'X');
    } // temporary NSO buffers are destroyed before symbolisation
    auto lookup=[&](size_t n) {return loader.ResolveSymbol<Elf64_Sym>(memory.data()+n);};
    assert(lookup(0).executableName.empty()); // below first module
    assert(lookup(200).executableName.empty()); // beyond final module
    assert(lookup(192).executableName.empty()); // exclusive module end
    assert(lookup(64).executableName=="test.nso.patch");
    assert(lookup(80).executableName=="test.nso.hook");
    assert(std::string(lookup(104).name)=="test");
    assert(!lookup(120).name); // exclusive symbol end
    loader.executables[0].symbolStrings.pop_back(); // unterminated symbol
    assert(!lookup(104).name);
    loader.executables[0].symbols.push_back(0); // malformed optional dynsym
    assert(lookup(104).executableName=="test.nso");
    assert(!lookup(104).name);
    std::vector<u8> noSymbols; std::vector<char> noStrings;
    loader.add(memory.data()+256, 64, noSymbols,noStrings);
    assert(lookup(220).executableName.empty()); // gap between modules
    assert(lookup(300).executableName=="test.nso");
    assert(!lookup(300).name);
    std::cout<<"PASS: empty modules, address bounds, owned symbol lifetime, symbol bounds, malformed metadata\n";
}
"""
harness = harness.replace("STRUCT", struct).replace("COPY", copy_block).replace("RESOLVER", resolver)
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "test.cpp"
    binary = Path(tmp) / "test"
    source.write_text(harness)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-g", "-O1",
                    "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
