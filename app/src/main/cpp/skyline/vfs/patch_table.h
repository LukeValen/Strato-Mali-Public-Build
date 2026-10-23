// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace skyline::vfs::patch {
    inline void Require(bool condition, const char *message) {
        if (!condition) throw std::runtime_error(message);
    }
    template<typename T> T Value(const std::vector<uint8_t> &bytes, size_t offset) {
        Require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "Truncated content metadata");
        T value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }
    struct Entry { uint64_t offset, physical; uint32_t source; };
    struct Table { uint64_t size; std::vector<Entry> entries; };
    // Nintendo's two-level BKTR bucket layout. No packed-pointer casts or unchecked counts.
    inline Table Parse(const std::vector<uint8_t> &bytes, bool relocation, uint32_t expectedEntries) {
        constexpr size_t block = 0x4000;
        Require(bytes.size() >= block, "Truncated BKTR root");
        auto buckets = Value<uint32_t>(bytes, 4);
        Require(buckets && buckets <= 0x3ff0 / 8 && buckets <= bytes.size() / block - 1, "Unsupported BKTR bucket tree");
        Table table{Value<uint64_t>(bytes, 8), {}};
        Require(table.size != 0, "Empty BKTR address space");
        const size_t stride = relocation ? 20 : 16;
        for (uint32_t i = 0; i < buckets; ++i) {
            size_t start = (i + 1) * block;
            auto count = Value<uint32_t>(bytes, start + 4);
            Require(count && count <= 0x3ff0 / stride, "Invalid BKTR entry count");
            auto bucketStart = Value<uint64_t>(bytes, 16 + i * 8);
            auto bucketEnd = i + 1 < buckets ? Value<uint64_t>(bytes, 16 + (i + 1) * 8) : table.size;
            Require(bucketStart < bucketEnd && bucketEnd <= table.size && Value<uint64_t>(bytes, start + 8) == bucketEnd, "Invalid BKTR bucket limits");
            for (uint32_t j = 0; j < count; ++j) {
                size_t pos = start + 16 + j * stride;
                Entry e{Value<uint64_t>(bytes, pos), 0, 0};
                if (relocation) {
                    e.physical = Value<uint64_t>(bytes, pos + 8);
                    e.source = Value<uint32_t>(bytes, pos + 16);
                    Require(e.source <= 1, "Invalid BKTR source");
                } else e.source = Value<uint32_t>(bytes, pos + 12);
                // A subsection table may end with a zero-length boundary at
                // the physical patch-data limit. It cannot be addressed by Find.
                bool terminal = !relocation && i + 1 == buckets && j + 1 == count && e.offset == bucketEnd;
                Require(e.offset >= bucketStart && (e.offset < bucketEnd || terminal) &&
                        (!j ? e.offset == bucketStart : true), "Invalid BKTR offset");
                Require(table.entries.empty() ? e.offset == 0 : e.offset > table.entries.back().offset, "Unsorted BKTR entries");
                table.entries.push_back(e);
            }
        }
        Require(table.entries.size() == expectedEntries, "BKTR entry count mismatch");
        return table;
    }
    inline size_t Find(const Table &table, uint64_t offset) {
        Require(offset < table.size && !table.entries.empty(), "BKTR read outside address space");
        auto it = std::upper_bound(table.entries.begin(), table.entries.end(), offset,
            [](uint64_t value, const Entry &entry) { return value < entry.offset; });
        Require(it != table.entries.begin(), "BKTR missing first entry");
        return static_cast<size_t>(it - table.entries.begin() - 1);
    }
    inline uint64_t End(const Table &table, size_t index) {
        return index + 1 < table.entries.size() ? table.entries[index + 1].offset : table.size;
    }
}
