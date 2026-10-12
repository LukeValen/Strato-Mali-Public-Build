// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "backing.h"
#include "bktr_backing.h"
#include "compressed_storage.h"

namespace skyline::vfs {
    class CompressedBacking : public Backing {
        std::shared_ptr<Backing> source;
        compression::Storage storage;

        static std::vector<u8> ReadTable(const std::shared_ptr<Backing> &source, const BktrHeader &header) {
            patch::Require(header.magic == util::MakeMagic<u32>("BKTR") && header.version <= 1 && header.entries,
                           "Invalid NCA compression header");
            patch::Require(header.offset <= source->size && header.size <= source->size - header.offset &&
                           header.size <= 32 * 1024 * 1024, "Invalid NCA compression table bounds");
            std::vector<u8> table(header.size);
            patch::Require(source->ReadUnchecked(table, header.offset) == table.size(), "Short compression table read");
            return table;
        }

      protected:
        size_t ReadImpl(span<u8> output, size_t offset) override {
            storage.Read(output.data(), output.size(), offset, [&](uint8_t *data, size_t length, uint64_t physical) {
                patch::Require(source->ReadUnchecked(span<u8>(data, length), physical) == length, "Short compressed source read");
            });
            return output.size();
        }

      public:
        CompressedBacking(std::shared_ptr<Backing> source, const BktrHeader &header)
            : Backing({true, false, false}, 0), source(std::move(source)),
              storage(ReadTable(this->source, header), header.entries, header.offset) {
            size = storage.Size();
        }
    };
}
