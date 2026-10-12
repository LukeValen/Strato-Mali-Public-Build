// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace skyline::applet::swkbd {
    // Always reserve the terminator and never split a UTF-8 sequence or UTF-16 pair.
    inline void EncodeKeyboardText(std::uint8_t *output, std::size_t capacity,
                                   std::u16string_view text, bool utf8) {
        std::fill_n(output, capacity, 0);
        std::size_t written{};
        for (std::size_t i{}; i < text.size(); ++i) {
            std::uint32_t point{text[i]};
            if (!point) break;
            if (point >= 0xD800 && point <= 0xDBFF) {
                if (i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
                    point = 0x10000 + ((point - 0xD800) << 10) + (text[++i] - 0xDC00);
                else point = 0xFFFD;
            } else if (point >= 0xDC00 && point <= 0xDFFF) point = 0xFFFD;
            if (utf8) {
                const std::size_t count{point < 0x80 ? 1U : point < 0x800 ? 2U : point < 0x10000 ? 3U : 4U};
                if (written + count >= capacity) break;
                if (count == 1) output[written++] = point;
                else {
                    output[written++] = (count == 2 ? 0xC0 : count == 3 ? 0xE0 : 0xF0) | (point >> (6 * (count - 1)));
                    for (std::size_t n = count - 1; n > 0; --n)
                        output[written++] = 0x80 | ((point >> (6 * (n - 1))) & 0x3F);
                }
            } else {
                const std::size_t count{point < 0x10000 ? 2U : 4U};
                if (written + count + 2 > capacity) break;
                auto writeUnit = [&](std::uint16_t unit) {
                    output[written++] = unit & 0xFF;
                    output[written++] = unit >> 8;
                };
                if (point < 0x10000) writeUnit(point);
                else {
                    point -= 0x10000;
                    writeUnit(0xD800 | (point >> 10));
                    writeUnit(0xDC00 | (point & 0x3FF));
                }
            }
        }
    }
}
