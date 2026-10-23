// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "ctr_encrypted_backing.h"
#include "patch_table.h"

namespace skyline::vfs {
    struct BktrHeader {
        u64 offset, size;
        u32 magic, version, entries, reserved;
    };
    static_assert(sizeof(BktrHeader) == 0x20);

    class BktrBacking : public Backing {
        std::shared_ptr<Backing> base;
        patch::Table relocations, subsections;
        std::vector<std::shared_ptr<Backing>> decrypted;
      protected:
        size_t ReadImpl(span<u8> output, size_t offset) override {
            patch::Require(offset <= size && output.size() <= size - offset, "BKTR read out of range");
            size_t done{};
            while (done < output.size()) {
                auto index = patch::Find(relocations, offset);
                const auto &entry = relocations.entries[index];
                size_t amount = std::min<u64>(output.size() - done, patch::End(relocations, index) - offset);
                size_t physical = entry.physical + offset - entry.offset;
                std::shared_ptr<Backing> source = base;
                if (entry.source) {
                    auto sub = patch::Find(subsections, physical);
                    amount = std::min<u64>(amount, patch::End(subsections, sub) - physical);
                    source = decrypted[sub];
                }
                patch::Require(source && source->ReadUnchecked(output.subspan(done, amount), physical) == amount, "Short BKTR source read");
                done += amount;
                offset += amount;
            }
            return done;
        }
      public:
        BktrBacking(std::shared_ptr<Backing> base, std::shared_ptr<Backing> raw,
                    std::shared_ptr<Backing> metadata, const BktrHeader &relocation,
                    const BktrHeader &subsection, crypto::KeyStore::Key128 ctr,
                    crypto::KeyStore::Key128 key, size_t absoluteOffset, bool encrypted)
            : base(std::move(base)) {
            patch::Require(this->base != nullptr, "An update requires its base game");
            for (const auto &header : {relocation, subsection}) {
                patch::Require(header.magic == util::MakeMagic<u32>("BKTR") && header.version <= 1, "Unsupported BKTR format");
                patch::Require(header.offset <= raw->size && header.size <= raw->size - header.offset && header.size <= 32 * 1024 * 1024, "Invalid BKTR table size");
            }
            // BKTR tables are consecutive, but an NCA section may have bytes
            // after the subsection table. Their presence does not extend the
            // addressable patch data or the table itself.
            patch::Require(relocation.offset + relocation.size == subsection.offset &&
                           subsection.offset + subsection.size <= raw->size, "Invalid BKTR section layout");
            auto parse = [&](const BktrHeader &header, bool isRelocation) {
                std::vector<u8> bytes(header.size);
                patch::Require(metadata->ReadUnchecked(bytes, header.offset) == bytes.size(), "Short BKTR metadata read");
                return patch::Parse(bytes, isRelocation, header.entries);
            };
            relocations = parse(relocation, true);
            subsections = parse(subsection, false);
            patch::Require(subsections.size == subsection.offset, "Invalid BKTR subsection limit");
            // The relocation table begins at the end of the patch data. Some
            // subsection tables explicitly contain that terminal boundary;
            // it is a valid zero-length sentinel, not another data region.
            // Keep rejecting entries past the boundary (corruption), and
            // never make the sentinel addressable by patch reads.
            if (subsections.entries.back().offset > relocation.offset)
                throw std::runtime_error(
                    "Invalid BKTR data limit (last subsection=" +
                    std::to_string(subsections.entries.back().offset) +
                    ", patch data end=" + std::to_string(relocation.offset) + ")");
            subsections.size = relocation.offset;
            size = relocations.size;
            for (size_t i{}; i < relocations.entries.size(); ++i) {
                const auto &entry = relocations.entries[i];
                u64 limit = entry.source ? relocation.offset : this->base->size;
                u64 length = patch::End(relocations, i) - entry.offset;
                patch::Require(entry.physical <= limit && length <= limit - entry.physical, "BKTR source outside file");
            }
            for (const auto &entry : subsections.entries) {
                auto counter = ctr;
                auto generation = util::SwapEndianness(entry.source);
                std::memcpy(counter.data() + 4, &generation, 4);
                decrypted.push_back(encrypted ? std::make_shared<CtrEncryptedBacking>(counter, key, raw, absoluteOffset) : raw);
            }
        }
    };
}
