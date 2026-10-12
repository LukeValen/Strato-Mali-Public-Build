// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <limits>
#include <mutex>
#include <lz4.h>
#include "patch_table.h"

namespace skyline::vfs::compression {
    struct Entry {
        uint64_t offset, physical;
        uint8_t type;
        int32_t physicalSize;
    };

    // NCA compressed storage uses 16 KiB BucketTree nodes and 24-byte entries.
    // Offsets refer to the final IVFC data level, after applying any BKTR patch.
    class Storage {
        uint64_t virtualSize;
        std::vector<Entry> entries;
        std::mutex mutex;
        std::array<char, 0x10000> decoded{};
        std::array<char, LZ4_COMPRESSBOUND(0x10000)> encoded{};
        size_t cached = std::numeric_limits<size_t>::max();

        uint64_t End(size_t index) const {
            return index + 1 < entries.size() ? entries[index + 1].offset : virtualSize;
        }

      public:
        Storage(const std::vector<uint8_t> &table, uint32_t expectedEntries, uint64_t physicalSize) {
            using patch::Require;
            using patch::Value;
            constexpr size_t nodeSize = 0x4000;
            Require(table.size() >= nodeSize, "Truncated compression table");
            auto buckets = Value<uint32_t>(table, 4);
            // Larger trees require an additional index level; never misinterpret it as entries.
            Require(buckets && buckets <= (nodeSize - 16) / 8 && buckets <= table.size() / nodeSize - 1,
                    "Unsupported compression bucket tree");
            virtualSize = Value<uint64_t>(table, 8);
            Require(virtualSize && virtualSize <= INT64_MAX, "Invalid compressed storage size");
            Require(expectedEntries && expectedEntries <= buckets * ((nodeSize - 16) / 24), "Invalid compression entry count");
            entries.reserve(expectedEntries);
            for (uint32_t i = 0; i < buckets; ++i) {
                size_t node = (i + 1) * nodeSize;
                auto count = Value<uint32_t>(table, node + 4);
                auto begin = Value<uint64_t>(table, 16 + i * 8);
                auto end = i + 1 < buckets ? Value<uint64_t>(table, 16 + (i + 1) * 8) : virtualSize;
                Require(count && count <= (nodeSize - 16) / 24 && count <= expectedEntries - entries.size(), "Invalid compression bucket count");
                Require(begin < end && end <= virtualSize && Value<uint64_t>(table, node + 8) == end, "Invalid compression bucket bounds");
                for (uint32_t j = 0; j < count; ++j) {
                    size_t pos = node + 16 + j * 24;
                    Entry entry{Value<uint64_t>(table, pos), Value<uint64_t>(table, pos + 8),
                                Value<uint8_t>(table, pos + 16), Value<int32_t>(table, pos + 20)};
                    Require(entry.offset >= begin && entry.offset < end && (j || entry.offset == begin), "Invalid compression entry offset");
                    Require(entries.empty() ? entry.offset == 0 : entry.offset > entries.back().offset, "Unsorted compression entries");
                    Require(entry.type == 0 || entry.type == 1 || entry.type == 3, "Unsupported NCA compression type");
                    Require(entry.physical <= INT64_MAX, "Negative compression physical offset");
                    entries.push_back(entry);
                }
            }
            Require(entries.size() == expectedEntries, "Compression entry count mismatch");
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto &entry = entries[i];
                uint64_t length = End(i) - entry.offset;
                if (entry.type == 1) continue; // Zero ranges have no physical data.
                if (entry.type == 3) {
                    Require(length <= decoded.size() && entry.physicalSize > 0 &&
                            static_cast<size_t>(entry.physicalSize) <= encoded.size() && !(entry.physical & 15), "Invalid LZ4 block size or alignment");
                    length = static_cast<uint32_t>(entry.physicalSize);
                }
                Require(entry.physical <= physicalSize && length <= physicalSize - entry.physical, "Compressed source outside data region");
            }
        }

        uint64_t Size() const { return virtualSize; }

        // The source callback must read exactly the requested bytes or throw.
        template<typename ReadSource>
        void Read(uint8_t *output, size_t length, uint64_t offset, ReadSource readSource) {
            patch::Require(offset <= virtualSize && length <= virtualSize - offset, "Compressed storage read out of range");
            if (!length) return;
            std::lock_guard<std::mutex> lock(mutex);
            auto it = std::upper_bound(entries.begin(), entries.end(), offset,
                [](uint64_t value, const Entry &entry) { return value < entry.offset; });
            size_t index = static_cast<size_t>(it - entries.begin() - 1);
            while (length) {
                const auto &entry = entries[index];
                size_t amount = static_cast<size_t>(std::min<uint64_t>(length, End(index) - offset));
                uint64_t within = offset - entry.offset;
                if (entry.type == 0) {
                    readSource(output, amount, entry.physical + within);
                } else if (entry.type == 1) {
                    std::memset(output, 0, amount);
                } else {
                    if (cached != index) {
                        // A failed decode must not leave a previous cache entry marked valid.
                        cached = std::numeric_limits<size_t>::max();
                        readSource(reinterpret_cast<uint8_t *>(encoded.data()), entry.physicalSize, entry.physical);
                        int blockSize = static_cast<int>(End(index) - entry.offset);
                        patch::Require(LZ4_decompress_safe(encoded.data(), decoded.data(), entry.physicalSize, blockSize) == blockSize,
                                       "Invalid or truncated NCA LZ4 block");
                        cached = index;
                    }
                    std::memcpy(output, decoded.data() + within, amount);
                }
                output += amount;
                length -= amount;
                offset += amount;
                ++index;
            }
        }
    };
}
