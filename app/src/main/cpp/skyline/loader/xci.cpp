// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <common.h>
#include <kernel/types/KProcess.h>
#include <vfs/region_backing.h>
#include "nca.h"
#include "mod_content.h"
#include "xci.h"

namespace skyline::loader {
    XciLoader::XciLoader(const std::shared_ptr<vfs::Backing> &backing, const std::shared_ptr<crypto::KeyStore> &keyStore) {
        header = backing->Read<GamecardHeader>();

        if (header.magic != util::MakeMagic<u32>("HEAD"))
            throw exception("Invalid XCI file");

        if (header.hfs0PartitionOffset < sizeof(GamecardHeader) || header.hfs0PartitionOffset > backing->size || header.hfs0HeaderSize > backing->size - header.hfs0PartitionOffset)
            throw exception("Invalid XCI root partition bounds");
        // The root HFS0 includes its child partition data, not just the header.
        xci = std::make_shared<vfs::PartitionFileSystem>(std::make_shared<vfs::RegionBacking>(backing, header.hfs0PartitionOffset, backing->size - header.hfs0PartitionOffset));

        auto root{xci->OpenDirectory("", {false, true})};
        for (const auto &entry : root->Read()) {
            auto entryDir{std::make_shared<vfs::PartitionFileSystem>(xci->OpenFile(entry.name))};
            if (entry.name == "secure")
                secure = entryDir;
            else if (entry.name == "normal")
                normal = entryDir;
            else if (entry.name == "update")
                update = entryDir;
            else if (entry.name == "logo")
                logo = entryDir;
        }

        if (secure) {
            root = secure->OpenDirectory("", {false, true});
            for (const auto &entry : root->Read()) {
                if (entry.name.substr(entry.name.find_last_of('.') + 1) != "nca")
                    continue;

                try {
                    auto nca{vfs::NCA(secure->OpenFile(entry.name), keyStore, true)};

                    if (nca.contentType == vfs::NcaContentType::Program && nca.romFs != nullptr && nca.exeFs != nullptr)
                        programNca = std::move(nca);
                    else if (nca.contentType == vfs::NcaContentType::Control && nca.romFs != nullptr)
                        controlNca = std::move(nca);
                } catch (const loader_exception &e) {
                    throw loader_exception(e.error);
                } catch (const std::exception &e) {
                    continue;
                }
            }
        } else {
            throw exception("Corrupted secure partition");
        }

        if (!programNca || !controlNca)
            throw exception("Incomplete XCI file");

        romFs = programNca->romFs;
        programId = programNca->header.programId;
        baseRomFsSection = programNca->romFsSection;
        controlRomFs = std::make_shared<vfs::RomFileSystem>(controlNca->romFs);
        nacp.emplace(controlRomFs->OpenFile("control.nacp"));
    }

    void *XciLoader::LoadProcessData(const std::shared_ptr<kernel::type::KProcess> &process, const DeviceState &state) {
        std::shared_ptr<vfs::FileSystem> exefs = installedExeFs ? installedExeFs : programNca->exeFs;
        if (!modExeFsOverrides.empty())
            exefs = std::make_shared<mods::ExeFsOverlay>(exefs, modExeFsOverrides);
        process->npdm = vfs::NPDM(exefs->OpenFile("main.npdm"));
        return NcaLoader::LoadExeFs(this, exefs, process, state);
    }

    std::vector<u8> XciLoader::GetIcon(language::ApplicationLanguage language) {
        if (controlRomFs == nullptr)
            return std::vector<u8>();

        std::shared_ptr<vfs::Backing> icon{};

        auto iconName{fmt::format("icon_{}.dat", language::ToString(language))};
        if (!(icon = controlRomFs->OpenFileUnchecked(iconName, {true, false, false}))) {
            iconName = fmt::format("icon_{}.dat", language::ToString(nacp->GetFirstSupportedTitleLanguage()));
            icon = controlRomFs->OpenFileUnchecked(iconName, {true, false, false});
        }

        if (icon == nullptr)
            return std::vector<u8>();

        std::vector<u8> buffer(icon->size);
        icon->Read(buffer);
        return buffer;
    }
}
