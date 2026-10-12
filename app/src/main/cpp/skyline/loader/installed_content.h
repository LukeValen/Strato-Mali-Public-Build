// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <filesystem>
#include <fcntl.h>
#include <vfs/os_backing.h>
#include <vfs/rom_filesystem.h>
#include "loader.h"
#include "content_package.h"

namespace skyline::loader::content {
    inline void ValidateRomFs(const std::shared_ptr<vfs::Backing> &backing) {
        using namespace vfs::patch;
        Require(backing && backing->size >= 0x50, "Missing or truncated RomFS header");
        std::vector<u8> header(0x50);
        Require(backing->ReadUnchecked(header) == header.size() && Value<u64>(header, 0) == 0x50, "Invalid RomFS header; check the base game and decryption keys");
        for (size_t i : {1, 3, 5, 7}) {
            auto offset = Value<u64>(header, i * 8), size = Value<u64>(header, (i + 1) * 8);
            Require(offset <= backing->size && size <= backing->size - offset, "Invalid RomFS table bounds");
        }
        Require(Value<u64>(header, 32) >= 24 && Value<u64>(header, 72) <= backing->size, "Invalid RomFS directory or data bounds");
    }
    inline void Apply(Loader &loader, const std::vector<Package> &packages, std::shared_ptr<crypto::KeyStore> keys) {
        using namespace vfs::patch;
        const Package *updatePackage{};
        const Metadata *update{};
        std::map<u64, std::pair<const Package *, const Metadata *>> dlcs;
        for (const auto &package : packages) for (const auto &meta : package.metadata) {
            if (meta.type == 0x81) {
                if (!update || meta.version > update->version) { update = &meta; updatePackage = &package; }
            } else {
                auto it = dlcs.find(meta.titleId);
                if (it == dlcs.end() || meta.version > it->second.second->version)
                    dlcs[meta.titleId] = {&package, &meta};
            }
        }
        auto romFs = loader.romFs;
        auto exeFs = loader.installedExeFs;
        auto nacp = loader.nacp;
        std::map<u64, std::shared_ptr<vfs::Backing>> contents;
        if (update) {
            Require(loader.baseRomFsSection != nullptr, "Update cannot be applied to this base game");
            for (const auto &record : update->records) {
                if (record.index != 0) continue;
                if (record.type != 1 && record.type != 3) continue;
                vfs::NCA nca(updatePackage->files.at(ContentName(record)), keys, false, loader.baseRomFsSection);
                if (record.type == 1) {
                    Require(nca.exeFs && nca.romFs && nca.exeFs->FileExists("main") && nca.exeFs->FileExists("main.npdm") && nca.exeFs->FileExists("rtld"), "Incomplete update program");
                    ValidateRomFs(nca.romFs);
                    romFs = nca.romFs;
                    exeFs = nca.exeFs;
                } else {
                    Require(nca.romFs != nullptr, "Missing update control data");
                    ValidateRomFs(nca.romFs);
                    vfs::RomFileSystem filesystem(nca.romFs);
                    nacp.emplace(filesystem.OpenFile("control.nacp"));
                }
            }
        }
        for (const auto &[id, selected] : dlcs) {
            for (const auto &record : selected.second->records) {
                if (record.type != 2 || record.index != 0) continue;
                vfs::NCA nca(selected.first->files.at(ContentName(record)), keys);
                Require(nca.romFs != nullptr, "DLC has no readable data filesystem");
                ValidateRomFs(nca.romFs);
                contents[id] = nca.romFs;
            }
        }
        loader.romFs = std::move(romFs);
        loader.installedExeFs = std::move(exeFs);
        loader.nacp = std::move(nacp);
        loader.addOnContent = std::move(contents);
        if (update) LOGI("Applied installed update version {}", update->version);
        LOGI("Loaded {} installed DLC titles", loader.addOnContent.size());
    }
    inline void LoadInstalled(Loader &loader, const std::string &publicPath, std::shared_ptr<crypto::KeyStore> keys) {
        if (!loader.programId) return;
        auto directory = std::filesystem::path(publicPath) / "contents" / fmt::format("{:016X}", loader.programId);
        if (!std::filesystem::exists(directory)) return;
        std::vector<std::filesystem::path> paths;
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".nsp") paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        std::vector<Package> packages;
        for (const auto &path : paths) {
            int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                LOGW("Skipping unreadable installed Update/DLC: {}", path.filename().string());
                continue;
            }
            try {
                packages.emplace_back(std::make_shared<vfs::OsBacking>(fd, true), keys, loader.programId);
            } catch (const std::exception &error) {
                // An installed NSP might have been corrupted after its initial
                // validation. Never prevent a valid base game from booting.
                LOGW("Skipping invalid Update/DLC {}: {}", path.filename().string(), error.what());
            }
        }
        if (!packages.empty()) {
            try {
                Apply(loader, packages, keys);
            } catch (const std::exception &error) {
                // Apply commits loader state only after validation succeeds.
                LOGW("Cannot apply installed content; starting base game: {}", error.what());
            }
        }
    }
}
