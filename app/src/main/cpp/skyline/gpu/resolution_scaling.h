// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
namespace skyline::gpu::resolution {
// This describes HOST render dimensions. Guest allocations, pitches and addresses
// must stay native; callers must synchronize original and scaled Vulkan images.
class Scale {
    unsigned quarters;
    explicit constexpr Scale(unsigned quarters) : quarters(quarters) {}
public:
    static constexpr Scale FromSetting(int index) {
        return Scale(index >= 0 && index < 7 ? unsigned(index + 2) : 4);
    }
    constexpr unsigned Numerator() const { return quarters; }
    constexpr unsigned DenominatorShift() const { return 2; }
    constexpr bool IsNative() const { return quarters == 4; }
    constexpr float Factor() const { return float(quarters) / 4.0f; }
    struct Extent { std::uint32_t width, height; };
    constexpr std::optional<Extent> TryExtent(Extent native, Extent deviceLimit) const {
        if (!native.width || !native.height) return std::nullopt;
        const auto w = std::max<std::uint64_t>(1, std::uint64_t(native.width) * quarters / 4);
        const auto h = std::max<std::uint64_t>(1, std::uint64_t(native.height) * quarters / 4);
        if (w > deviceLimit.width || h > deviceLimit.height) return std::nullopt;
        return Extent{std::uint32_t(w), std::uint32_t(h)};
    }
};
}
