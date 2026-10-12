// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <map>
#include <mbedtls/md.h>
#include <vfs/nca.h>
#include <vfs/region_backing.h>
#include <vfs/ticket.h>
#include "content_metadata.h"

namespace skyline::loader::content {
    inline std::string ContentName(const Record &record) {
        std::string name;
        for (auto b : record.id) name += fmt::format("{:02x}", b);
        return name + ".nca";
    }
    // Validate the container before passing any name or count to existing VFS parsers.
    inline std::map<std::string, std::shared_ptr<vfs::Backing>> ReadPackage(std::shared_ptr<vfs::Backing> backing) {
        using namespace vfs::patch;
        std::vector<u8> header(16);
        Require(backing->size >= header.size() && backing->ReadUnchecked(header) == header.size(), "Truncated NSP header");
        Require(Value<u32>(header, 0) == util::MakeMagic<u32>("PFS0"), "Select an uncompressed NSP content package");
        auto count = Value<u32>(header, 4), stringsSize = Value<u32>(header, 8);
        Require(count && count <= 65536 && stringsSize <= 16 * 1024 * 1024, "Invalid NSP file table size");
        u64 stringsOffset = 16 + static_cast<u64>(count) * 24;
        u64 dataOffset = stringsOffset + stringsSize;
        Require(dataOffset <= backing->size, "Truncated NSP file table");
        std::vector<u8> table(dataOffset);
        Require(backing->ReadUnchecked(table) == table.size(), "Short NSP file table read");
        std::map<std::string, std::shared_ptr<vfs::Backing>> files;
        for (u32 i{}; i < count; ++i) {
            size_t pos = 16 + i * 24;
            auto offset = Value<u64>(table, pos), size = Value<u64>(table, pos + 8);
            auto nameOffset = Value<u32>(table, pos + 16);
            Require(nameOffset < stringsSize && offset <= backing->size - dataOffset && size <= backing->size - dataOffset - offset, "Invalid NSP entry bounds");
            auto nameBegin = reinterpret_cast<const char *>(table.data() + stringsOffset + nameOffset);
            auto end = static_cast<const char *>(std::memchr(nameBegin, 0, stringsSize - nameOffset));
            Require(end && end - nameBegin <= 255, "Invalid NSP filename");
            std::string name(nameBegin, end);
            Require(!name.empty() && name.find('/') == std::string::npos && name.find('\\') == std::string::npos, "Invalid NSP filename");
            Require(files.emplace(name, std::make_shared<vfs::RegionBacking>(backing, dataOffset + offset, size)).second, "Duplicate NSP filename");
        }
        return files;
    }
    class Package {
        static void Verify(const std::shared_ptr<vfs::Backing> &file, const Record &record) {
            struct Digest {
                mbedtls_md_context_t context;
                Digest() { mbedtls_md_init(&context); }
                ~Digest() { mbedtls_md_free(&context); }
            } digest;
            using namespace vfs::patch;
            Require(mbedtls_md_setup(&digest.context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) == 0 && mbedtls_md_starts(&digest.context) == 0, "Could not initialize content integrity check");
            std::vector<u8> buffer(std::min<size_t>(file->size, 1024 * 1024));
            for (size_t offset{}; offset < file->size;) {
                size_t length = std::min(buffer.size(), file->size - offset);
                Require(file->ReadUnchecked(span(buffer).first(length), offset) == length && mbedtls_md_update(&digest.context, buffer.data(), length) == 0, "Failed to read content for integrity check");
                offset += length;
            }
            std::array<u8, 32> hash{};
            Require(mbedtls_md_finish(&digest.context, hash.data()) == 0 && hash == record.hash, "Content integrity check failed; the NSP may be damaged");
        }
      public:
        std::map<std::string, std::shared_ptr<vfs::Backing>> files;
        std::vector<Metadata> metadata;
        Package(std::shared_ptr<vfs::Backing> backing, std::shared_ptr<crypto::KeyStore> keys, u64 gameId, bool verifyIntegrity = false) : files(ReadPackage(std::move(backing))) {
            using namespace vfs::patch;
            for (const auto &[name, file] : files) {
                if (!name.ends_with(".tik")) continue;
                Require(file->size >= 0x2c0 && file->size <= 0x4000, "Invalid content ticket size");
                vfs::Ticket ticket(file);
                auto key = span(ticket.titleKeyBlock).subspan(0, 16).as<crypto::KeyStore::Key128>();
                keys->PopulateTitleKey(ticket.rightsId, key);
            }
            for (const auto &[name, file] : files) {
                if (!name.ends_with(".nca")) continue;
                vfs::NCA header(file, keys, false, {}, true);
                if (header.contentType != vfs::NcaContentType::Meta) continue;
                vfs::NCA nca(file, keys);
                Require(nca.cnmt != nullptr, "Missing CNMT filesystem");
                for (const auto &entry : nca.cnmt->OpenDirectory("", {false, true})->Read()) {
                    if (!entry.name.ends_with(".cnmt")) continue;
                    auto cnmt = nca.cnmt->OpenFile(entry.name);
                    Require(cnmt->size <= 4 * 1024 * 1024, "CNMT is too large");
                    std::vector<u8> bytes(cnmt->size);
                    Require(cnmt->ReadUnchecked(bytes) == bytes.size(), "Short CNMT read");
                    auto meta = ParseMetadata(bytes, gameId);
                    Require(meta.titleId == header.header.programId, "CNMT/NCA title ID mismatch");
                    for (const auto &record : meta.records) {
                        auto it = files.find(ContentName(record));
                        if (!ValidateRecordPresence(meta, record, it != files.end(), it != files.end() ? it->second->size : 0)) continue;
                        if (verifyIntegrity) Verify(it->second, record);
                        vfs::NCA content(it->second, keys, false, {}, true);
                        bool validType = record.type == 1 ? content.contentType == vfs::NcaContentType::Program :
                            record.type == 2 ? (content.contentType == vfs::NcaContentType::Data || content.contentType == vfs::NcaContentType::PublicData) :
                            record.type == 3 ? content.contentType == vfs::NcaContentType::Control : true;
                        Require(validType, "CNMT/NCA content type mismatch");
                        Require(content.header.programId == meta.titleId || (meta.type == 0x81 && content.header.programId == gameId), "Content NCA belongs to another title");
                    }
                    metadata.push_back(std::move(meta));
                }
            }
            Require(!metadata.empty(), "No Update or DLC metadata found in this NSP");
        }
    };
}
