// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace skyline::soc::host1x {
    // CDMA opcodes used by nvhost gathers. Class selection persists between gathers.
    // Keep decoding separate from dispatch so captures can be validated before enabling engines.
    template<class Words, class Dispatch>
    void DecodeCommands(const Words &words, uint32_t &selectedClass, Dispatch dispatch) {
        size_t position{};
        auto argument = [&]() {
            if (position == words.size())
                throw std::runtime_error("Truncated Host1x method arguments");
            return words[position++];
        };
        while (position < words.size()) {
            const uint32_t header{argument()};
            const uint32_t opcode{header >> 28};
            const uint32_t method{(header >> 16) & 0xfff};
            switch (opcode) {
                case 0: {
                    selectedClass = (header >> 6) & 0x3ff;
                    const auto mask{header & 0x3f};
                    for (uint32_t bit{}; bit < 6; ++bit)
                        if (mask & (1u << bit))
                            dispatch(selectedClass, method + bit, argument());
                    break;
                }
                case 1:
                case 2: {
                    const auto count{header & 0xffff};
                    if (count > words.size() - position || (opcode == 1 && count && count - 1 > 0xfff - method))
                        throw std::runtime_error("Invalid Host1x method range");
                    for (uint32_t i{}; i < count; ++i)
                        dispatch(selectedClass, method + (opcode == 1 ? i : 0), argument());
                    break;
                }
                case 3: {
                    const auto mask{header & 0xffff};
                    for (uint32_t bit{}; bit < 16; ++bit)
                        if (mask & (1u << bit)) {
                            if (bit > 0xfff - method)
                                throw std::runtime_error("Invalid Host1x masked method range");
                            dispatch(selectedClass, method + bit, argument());
                        }
                    break;
                }
                case 4:
                    dispatch(selectedClass, method, header & 0xfff);
                    break;
                default:
                    throw std::runtime_error("Unsupported Host1x gather opcode");
            }
        }
    }
}
