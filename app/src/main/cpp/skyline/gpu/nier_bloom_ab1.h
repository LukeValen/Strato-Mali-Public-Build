// SPDX-License-Identifier: MPL-2.0
// Experimental NieR/Mali A/B: suppress only bloom extraction (color attachment 1).
// No shader instruction insertion: rewrite four stores to an existing float zero.
// Refuse to patch if SPIR-V does not match the expected safe structure.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace skyline::gpu::nier_bloom {
inline bool SuppressBrightPass(std::vector<std::uint32_t>& words) {
    constexpr std::uint32_t Magic = 0x07230203;
    constexpr std::uint32_t Decorate = 71, Location = 30;
    constexpr std::uint32_t TypeFloat = 22, TypePointer = 32, Constant = 43;
    constexpr std::uint32_t Variable = 59, AccessChain = 65, InBoundsAccessChain = 66;
    constexpr std::uint32_t Store = 62, EntryPoint = 15;
    constexpr std::uint32_t Output = 3, Fragment = 4;
    if (words.size() < 12 || words[0] != Magic || words[3] == 0 || words[3] > 1048576)
        return false;

    struct Insn { std::uint32_t op; std::size_t i, count; };
    std::vector<Insn> instructions;
    instructions.reserve(words.size() / 4);
    std::unordered_set<std::uint32_t> locationOne;
    std::unordered_set<std::uint32_t> floatTypes;
    std::unordered_map<std::uint32_t, std::uint32_t> pointerPointees;
    std::uint32_t zeroId{};
    bool isFragment{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> constants;
    std::vector<std::uint32_t> outputVars;

    for (std::size_t i = 5; i < words.size();) {
        const auto count = words[i] >> 16;
        const auto op = words[i] & 0xffffU;
        if (!count || count > words.size() - i)
            return false;
        instructions.push_back({op, i, count});
        if (op == EntryPoint && count >= 3 && words[i + 1] == Fragment)
            isFragment = true;
        if (op == Decorate && count >= 4 && words[i + 2] == Location && words[i + 3] == 1)
            locationOne.insert(words[i + 1]);
        if (op == TypeFloat && count == 3 && words[i + 2] == 32)
            floatTypes.insert(words[i + 1]);
        if (op == TypePointer && count >= 4)
            pointerPointees[words[i + 1]] = words[i + 3];
        if (op == Constant && count == 4 && words[i + 3] == 0)
            constants.emplace_back(words[i + 1], words[i + 2]);
        if (op == Variable && count >= 4 && words[i + 3] == Output)
            outputVars.push_back(words[i + 2]);
        i += count;
    }
    if (!isFragment) return false;
    for (const auto& [type, id] : constants) {
        if (floatTypes.count(type)) {
            zeroId = id;
            break;
        }
    }
    if (!zeroId || zeroId >= words[3]) return false;

    std::unordered_set<std::uint32_t> pointers;
    for (const auto id : outputVars)
        if (locationOne.count(id))
            pointers.insert(id);
    if (pointers.size() != 1) return false;

    std::vector<std::size_t> stores;
    for (const auto& ins : instructions) {
        if ((ins.op == AccessChain || ins.op == InBoundsAccessChain) &&
            ins.count >= 4 && pointers.count(words[ins.i + 3])) {
            const auto it = pointerPointees.find(words[ins.i + 1]);
            if (it != pointerPointees.end() && floatTypes.count(it->second))
                pointers.insert(words[ins.i + 2]);
        }
        if (ins.op == Store && ins.count >= 3 && pointers.count(words[ins.i + 1]))
            stores.push_back(ins.i + 2);
    }
    // The captured NieR bright-pass writes four F32 components to RT1. An
    // unexpected shader layout must remain unmodified.
    if (stores.size() != 4) return false;
    for (const auto position : stores)
        words[position] = zeroId;
    return true;
}
}  // namespace skyline::gpu::nier_bloom
