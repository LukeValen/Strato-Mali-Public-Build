// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace skyline::loader::mods {
    struct TextPatch {
        uint64_t offset; // Offset in the uncompressed NSO image, including its 0x100-byte header.
        std::vector<uint8_t> bytes;
    };

    inline std::string TrimPatchText(std::string value) {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return {};
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    inline unsigned HexDigit(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        throw std::runtime_error("Invalid hexadecimal value in pchtxt");
    }

    inline uint64_t ParseHex(std::string_view value) {
        if (value.starts_with("0x") || value.starts_with("0X")) value.remove_prefix(2);
        if (value.empty() || value.size() > 16) throw std::runtime_error("Invalid pchtxt offset");
        uint64_t result = 0;
        for (const auto ch : value) result = (result << 4) | HexDigit(ch);
        return result;
    }

    // Parse only the binary patch subset of IPSwitch. Unsupported syntax fails the
    // whole file so a partially applied mod cannot leave the game in an unknown state.
    inline std::vector<TextPatch> ParsePatchText(std::istream &input, const std::array<uint8_t, 32> &buildId) {
        std::vector<TextPatch> patches;
        bool matches = false, enabled = false, seenBuildId = false, bigEndian = false;
        int64_t offsetShift = 0;
        std::string line;
        size_t count = 0;
        while (std::getline(input, line)) {
            if (++count > 16384 || line.size() > 4096) throw std::runtime_error("Pchtxt exceeds size limits");
            const auto comment = line.find("//");
            if (comment != std::string::npos) line.resize(comment);
            line = TrimPatchText(std::move(line));
            if (line.empty() || line.front() == '#') continue;
            if (line == "@stop") break;
            if (line.starts_with("@nsobid-")) {
                const auto id = std::string_view(line).substr(8);
                if (id.size() != 40 && id.size() != 64) throw std::runtime_error("Invalid pchtxt Build ID");
                matches = true;
                for (size_t i = 0; i < id.size() / 2; ++i)
                    matches &= static_cast<uint8_t>((HexDigit(id[2 * i]) << 4) | HexDigit(id[2 * i + 1])) == buildId[i];
                seenBuildId = true;
                enabled = false;
            } else if (line == "@enabled" || line == "@disabled") {
                if (!seenBuildId) throw std::runtime_error("Pchtxt missing Build ID");
                enabled = line == "@enabled";
            } else if (line.starts_with("@flag offset_shift ")) {
                auto value = TrimPatchText(line.substr(19));
                bool negative = !value.empty() && value.front() == '-';
                if (negative) value.erase(0, 1);
                const bool hex = value.starts_with("0x") || value.starts_with("0X");
                if (!hex && (value.empty() || !std::all_of(value.begin(), value.end(), [](char ch) { return ch >= '0' && ch <= '9'; })))
                    throw std::runtime_error("Invalid pchtxt shift");
                uint64_t parsed = 0;
                if (hex) parsed = ParseHex(value);
                else for (char ch : value) {
                    if (parsed > (std::numeric_limits<uint64_t>::max() - (ch - '0')) / 10)
                        throw std::runtime_error("Pchtxt shift overflow");
                    parsed = parsed * 10 + (ch - '0');
                }
                if (parsed > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                    throw std::runtime_error("Pchtxt shift out of range");
                offsetShift = negative ? -static_cast<int64_t>(parsed) : static_cast<int64_t>(parsed);
            } else if (line == "@flag print_values") {
                continue;
            } else if (line == "@big-endian" || line == "@little-endian") {
                bigEndian = line == "@big-endian";
            } else if (line.front() == '@') {
                throw std::runtime_error("Unsupported pchtxt directive: " + line);
            } else {
                if (!seenBuildId) throw std::runtime_error("Pchtxt missing Build ID");
                const auto separator = line.find_first_of(" \t");
                if (separator == std::string::npos) throw std::runtime_error("Invalid pchtxt patch line");
                const uint64_t rawOffset = ParseHex(std::string_view(line).substr(0, separator));
                if (rawOffset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
                    (offsetShift > 0 && rawOffset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - offsetShift)))
                    throw std::runtime_error("Pchtxt offset overflow");
                const int64_t shifted = static_cast<int64_t>(rawOffset) + offsetShift;
                if (shifted < 0x100) throw std::runtime_error("Pchtxt writes into NSO header");
                auto value = TrimPatchText(line.substr(separator + 1));
                if (value.empty() || value.front() == '"') throw std::runtime_error("Unsupported pchtxt value");
                std::vector<uint8_t> bytes;
                size_t pos = 0;
                do {
                    const auto end = value.find_first_of(" \t", pos);
                    const auto token = std::string_view(value).substr(pos, end == std::string::npos ? end : end - pos);
                    if (token.empty() || token.size() % 2 || token.size() > 8192)
                        throw std::runtime_error("Invalid pchtxt byte sequence");
                    std::vector<uint8_t> word;
                    for (size_t i = 0; i < token.size(); i += 2)
                        word.push_back(static_cast<uint8_t>((HexDigit(token[i]) << 4) | HexDigit(token[i + 1])));
                    if (bigEndian) std::reverse(word.begin(), word.end());
                    bytes.insert(bytes.end(), word.begin(), word.end());
                    if (bytes.size() > 4096) throw std::runtime_error("Pchtxt patch too large");
                    if (end == std::string::npos) break;
                    pos = value.find_first_not_of(" \t", end);
                } while (pos != std::string::npos);
                if (enabled && matches) patches.push_back({static_cast<uint64_t>(shifted), std::move(bytes)});
            }
        }
        if (!seenBuildId) throw std::runtime_error("Pchtxt missing Build ID");
        return patches;
    }

    inline std::vector<TextPatch> ReadPatchText(const std::string &path, const std::array<uint8_t, 32> &buildId) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input || input.tellg() > 1024 * 1024) throw std::runtime_error("Cannot read pchtxt or file exceeds 1 MB");
        input.seekg(0);
        return ParsePatchText(input, buildId);
    }
}
