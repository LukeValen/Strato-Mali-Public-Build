// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
namespace strato::video {
// The only connection between the portable video implementation and Strato.
struct Memory {
    std::function<void(std::uint32_t, void*, std::size_t)> read;
    std::function<void(std::uint32_t, const void*, std::size_t)> write;
    void ReadBlock(std::uint64_t address, void* output, std::size_t size) const;
    void WriteBlock(std::uint64_t address, const void* input, std::size_t size) const;
};
class Runtime {
public:
    explicit Runtime(Memory memory);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    unsigned ReserveChannel(); // Called while constructing Host1x channels
    void NvdecMethod(unsigned channel, std::uint32_t method, std::uint32_t argument);
    void VicMethod(unsigned channel, std::uint32_t method, std::uint32_t argument);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
