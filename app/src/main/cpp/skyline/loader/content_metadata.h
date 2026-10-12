// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <string>
#include <vfs/patch_table.h>

namespace skyline::loader::content {
    struct Record { std::array<uint8_t, 32> hash; std::array<uint8_t, 16> id; uint64_t size; uint8_t type, index; };
    struct Metadata { uint64_t titleId, applicationId; uint32_t version; uint8_t type; std::vector<Record> records; };
    // Full update programs do not require incremental delta fragments.
    // If a fragment is supplied, it must still pass size and integrity checks.
    inline bool ValidateRecordPresence(const Metadata &meta, const Record &record, bool present, uint64_t size) {
        using namespace vfs::patch;
        if (!present && meta.type == 0x81 && record.type == 6) return false;
        Require(present, "Missing required content NCA");
        Require(size == record.size, "Content NCA size does not match metadata");
        return true;
    }
    inline Metadata ParseMetadata(const std::vector<uint8_t> &bytes, uint64_t gameId) {
        using namespace vfs::patch;
        Require(bytes.size() >= 0x20, "Truncated CNMT header");
        Metadata meta{Value<uint64_t>(bytes, 0), 0, Value<uint32_t>(bytes, 8), Value<uint8_t>(bytes, 12), {}};
        auto extendedSize = Value<uint16_t>(bytes, 14);
        auto count = Value<uint16_t>(bytes, 16);
        Require(meta.type == 0x81 || meta.type == 0x82, "Select a separate Update or DLC NSP, not a base game");
        Require(extendedSize >= 8 && extendedSize <= bytes.size() - 0x20, "Invalid CNMT extended header");
        meta.applicationId = Value<uint64_t>(bytes, 0x20);
        Require(gameId && meta.applicationId == gameId, "This content belongs to another game");
        if (meta.type == 0x81) Require(meta.titleId == (gameId | 0x800), "Invalid update title ID");
        else Require(meta.titleId > gameId + 0x1000 && meta.titleId - (gameId + 0x1000) <= 0xfff, "Invalid DLC title ID");
        size_t offset = 0x20 + extendedSize;
        Require(count && count <= (bytes.size() - offset) / 0x38, "Invalid CNMT content count");
        bool requiredContent = false;
        for (uint16_t i = 0; i < count; ++i, offset += 0x38) {
            Record record{};
            std::memcpy(record.hash.data(), bytes.data() + offset, record.hash.size());
            std::memcpy(record.id.data(), bytes.data() + offset + 0x20, record.id.size());
            for (size_t j = 0; j < 6; ++j) record.size |= static_cast<uint64_t>(bytes[offset + 0x30 + j]) << (j * 8);
            record.type = bytes[offset + 0x36];
            record.index = bytes[offset + 0x37];
            Require(record.type <= 6 && record.size >= 0xc00, "Invalid CNMT content record");
            if (record.index == 0 && record.type == (meta.type == 0x81 ? 1 : 2)) requiredContent = true;
            meta.records.push_back(record);
        }
        Require(requiredContent, "Delta-only and incomplete content packages are unsupported");
        return meta;
    }
}
