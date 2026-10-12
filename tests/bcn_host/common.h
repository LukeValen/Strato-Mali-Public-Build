#pragma once
#include <array>
#include <utility>
#include <cstdint>
#include <climits>
#include <cstring>
#include <mutex>
#include <span>
#include <stdexcept>
#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan_raii.hpp>
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
namespace skyline {
    template<class T> using span = std::span<T>;
    struct exception : std::runtime_error {
        exception(const std::string &msg) : std::runtime_error(msg) {}
        template<class... T> exception(const char *msg, T&&...) : std::runtime_error(msg) {}
    };
}
