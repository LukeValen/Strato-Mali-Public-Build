// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <crypto/aes_cipher.h>
#include <loader/loader.h>

#include "ctr_encrypted_backing.h"
#include "region_backing.h"
#include "compressed_backing.h"
#include "partition_filesystem.h"
#include "nca.h"
#include "rom_filesystem.h"
#include "directory.h"

namespace skyline::vfs {
    using namespace loader;

    NCA::NCA(std::shared_ptr<vfs::Backing> pBacking, std::shared_ptr<crypto::KeyStore> pKeyStore, bool pUseKeyArea, std::shared_ptr<Backing> pBaseRomFsSection, bool headerOnly) : backing(std::move(pBacking)), keyStore(std::move(pKeyStore)), useKeyArea(pUseKeyArea), baseRomFsSection(std::move(pBaseRomFsSection)) {
        header = backing->Read<NcaHeader>();

        if (header.magic != util::MakeMagic<u32>("NCA3")) {
            if (!keyStore->headerKey)
                throw loader_exception(LoaderResult::MissingHeaderKey);

            crypto::AesCipher cipher(*keyStore->headerKey, MBEDTLS_CIPHER_AES_128_XTS);

            cipher.XtsDecrypt({reinterpret_cast<u8 *>(&header), sizeof(NcaHeader)}, 0, 0x200);

            // Check if decryption was successful
            if (header.magic != util::MakeMagic<u32>("NCA3"))
                throw loader_exception(LoaderResult::ParsingError);
            encrypted = true;
        }

        contentType = header.contentType;
        rightsIdEmpty = header.rightsId == crypto::KeyStore::Key128{};
        if (header.size != backing->size)
            throw exception("NCA size does not match its file");
        if (headerOnly) return;

        for (size_t i{}; i < header.sectionHeaders.size(); i++) {
            auto &sectionHeader{header.sectionHeaders.at(i)};
            auto &sectionEntry{header.fsEntries.at(i)};
            if (!sectionEntry.startOffset && !sectionEntry.endOffset) continue;
            if (sectionEntry.startOffset < sizeof(NcaHeader) / constant::MediaUnitSize || sectionEntry.endOffset < sectionEntry.startOffset || static_cast<u64>(sectionEntry.endOffset) * constant::MediaUnitSize > backing->size)
                throw exception("Invalid NCA section bounds");

            if (sectionHeader.fsType == NcaSectionFsType::PFS0 && sectionHeader.hashType == NcaSectionHashType::HierarchicalSha256)
                ReadPfs0(sectionHeader, sectionEntry);
            else if (sectionHeader.fsType == NcaSectionFsType::RomFs && sectionHeader.hashType == NcaSectionHashType::HierarchicalIntegrity)
                ReadRomFs(sectionHeader, sectionEntry);
        }
    }

    void NCA::ReadPfs0(const NcaSectionHeader &sectionHeader, const NcaFsEntry &entry) {
        size_t offset{static_cast<size_t>(entry.startOffset) * constant::MediaUnitSize + sectionHeader.sha256HashInfo.pfs0Offset};
        size_t size{sectionHeader.sha256HashInfo.pfs0Size};
        size_t end{static_cast<size_t>(entry.endOffset) * constant::MediaUnitSize};
        if (offset > end || size > end - offset) throw exception("Invalid NCA PFS0 bounds");

        auto decrypted{CreateBacking(sectionHeader, std::make_shared<RegionBacking>(backing, offset, size), offset)};
        if (!decrypted) throw exception("Unsupported NCA PFS0 encryption");
        auto pfs{std::make_shared<PartitionFileSystem>(decrypted)};

        if (contentType == NcaContentType::Program) {
            // An ExeFS must always contain an NPDM and a main NSO, whereas the logo section will always contain a logo and a startup movie
            if (pfs->FileExists("main") && pfs->FileExists("main.npdm"))
                exeFs = std::move(pfs);
            else if (pfs->FileExists("NintendoLogo.png") && pfs->FileExists("StartupMovie.gif"))
                logo = std::move(pfs);
        } else if (contentType == NcaContentType::Meta) {
            cnmt = std::move(pfs);
        }
    }

    void NCA::ReadRomFs(const NcaSectionHeader &sectionHeader, const NcaFsEntry &entry) {
        size_t sectionOffset{static_cast<size_t>(entry.startOffset) * constant::MediaUnitSize};
        size_t sectionSize{static_cast<size_t>(entry.endOffset - entry.startOffset) * constant::MediaUnitSize};
        auto raw{std::make_shared<RegionBacking>(backing, sectionOffset, sectionSize)};
        auto decrypted{CreateBacking(sectionHeader, raw, sectionOffset)};
        if (!decrypted) throw exception("Unsupported NCA RomFS encryption");
        if (sectionHeader.encryptionType == NcaSectionEncryptionType::BKTR) {
            // Metadata scanners cannot apply a patch without its base section.
            if (!baseRomFsSection) return;
            auto key{encrypted ? (!(rightsIdEmpty || useKeyArea) ? GetTitleKey() : GetKeyAreaKey(sectionHeader.encryptionType)) : crypto::KeyStore::Key128{}};
            crypto::KeyStore::Key128 ctr{};
            auto secure = util::SwapEndianness(sectionHeader.secureValue);
            auto generation = util::SwapEndianness(sectionHeader.generation);
            std::memcpy(ctr.data(), &secure, 4);
            std::memcpy(ctr.data() + 4, &generation, 4);
            romFsSection = std::make_shared<BktrBacking>(baseRomFsSection, raw, decrypted, sectionHeader.relocation, sectionHeader.subsection, ctr, key, sectionOffset, encrypted);
        } else romFsSection = decrypted;
        size_t offset{sectionHeader.integrityHashInfo.levels.back().offset};
        size_t size{sectionHeader.integrityHashInfo.levels.back().size};
        if (offset > romFsSection->size || size > romFsSection->size - offset) throw exception("Invalid RomFS level bounds");
        romFs = std::make_shared<RegionBacking>(romFsSection, offset, size);
        if (sectionHeader.compression.offset || sectionHeader.compression.size || sectionHeader.compression.entries)
            romFs = std::make_shared<CompressedBacking>(romFs, sectionHeader.compression);
    }

    std::shared_ptr<Backing> NCA::CreateBacking(const NcaSectionHeader &sectionHeader, std::shared_ptr<Backing> rawBacking, size_t offset) {
        if (!encrypted)
            return rawBacking;

        switch (sectionHeader.encryptionType) {
            case NcaSectionEncryptionType::None:
                return rawBacking;
            case NcaSectionEncryptionType::CTR:
            case NcaSectionEncryptionType::BKTR: {
                auto key{!(rightsIdEmpty || useKeyArea) ? GetTitleKey() : GetKeyAreaKey(sectionHeader.encryptionType)};

                std::array<u8, 0x10> ctr{};
                u32 secureValueLE{util::SwapEndianness(sectionHeader.secureValue)};
                u32 generationLE{util::SwapEndianness(sectionHeader.generation)};
                std::memcpy(ctr.data(), &secureValueLE, 4);
                std::memcpy(ctr.data() + 4, &generationLE, 4);

                return std::make_shared<CtrEncryptedBacking>(ctr, key, std::move(rawBacking), offset);
            }
            default:
                return nullptr;
        }
    }

    u8 NCA::GetKeyGeneration() {
        u8 legacyGen{static_cast<u8>(header.legacyKeyGenerationType)};
        u8 gen{static_cast<u8>(header.keyGenerationType)};
        gen = std::max<u8>(legacyGen, gen);
        return gen > 0 ? gen - 1 : gen;
    }

    crypto::KeyStore::Key128 NCA::GetTitleKey() {
        u8 keyGeneration{GetKeyGeneration()};

        auto titleKey{keyStore->GetTitleKey(header.rightsId)};
        auto &titleKek{keyStore->titleKek.at(keyGeneration)};

        if (!titleKey)
            throw loader_exception(LoaderResult::MissingTitleKey);
        if (!titleKek)
            throw loader_exception(LoaderResult::MissingTitleKek);

        crypto::AesCipher cipher(*titleKek, MBEDTLS_CIPHER_AES_128_ECB);
        cipher.Decrypt(*titleKey);
        return *titleKey;
    }

    crypto::KeyStore::Key128 NCA::GetKeyAreaKey(NCA::NcaSectionEncryptionType type) {
        auto keyArea{[this, &type](crypto::KeyStore::IndexedKeys128 &keys) {
            u8 keyGeneration{GetKeyGeneration()};

            auto &keyArea{keys.at(keyGeneration)};

            if (!keyArea)
                throw loader_exception(LoaderResult::MissingKeyArea);

            size_t keyAreaIndex;
            switch (type) {
                case NcaSectionEncryptionType::XTS:
                    keyAreaIndex = 0;
                    break;
                case NcaSectionEncryptionType::CTR:
                case NcaSectionEncryptionType::BKTR:
                    keyAreaIndex = 2;
                    break;
                default:
                    throw exception("Unsupported NcaSectionEncryptionType");
            }

            crypto::KeyStore::Key128 decryptedKeyArea;
            crypto::AesCipher cipher(*keyArea, MBEDTLS_CIPHER_AES_128_ECB);
            cipher.Decrypt(decryptedKeyArea.data(), header.encryptedKeyArea[keyAreaIndex].data(), decryptedKeyArea.size());
            return decryptedKeyArea;
        }};

        switch (header.keyAreaEncryptionKeyType) {
            case NcaKeyAreaEncryptionKeyType::Application:
                return keyArea(keyStore->areaKeyApplication);
            case NcaKeyAreaEncryptionKeyType::Ocean:
                return keyArea(keyStore->areaKeyOcean);
            case NcaKeyAreaEncryptionKeyType::System:
                return keyArea(keyStore->areaKeySystem);
        }
        throw loader_exception(LoaderResult::ParsingError);
    }
}
