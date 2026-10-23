// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <map>
#include <set>
#include <string>
#include <vfs/os_backing.h>
#include <vfs/rom_filesystem.h>
#include "loader.h"

namespace skyline::loader::mods {
    namespace fs = std::filesystem;

    // Replaces individual ExeFS files without changing the underlying NCA.
    class ExeFsOverlay final : public vfs::FileSystem {
        std::shared_ptr<vfs::FileSystem> base;
        std::map<std::string, fs::path> overrides;

      protected:
        std::shared_ptr<vfs::Backing> OpenFileImpl(const std::string &path, vfs::Backing::Mode mode) override {
            auto file = overrides.find(path);
            if (file == overrides.end()) return base->OpenFileUnchecked(path, mode);
            if (mode.write) throw exception("ExeFS mod files are read-only");
            const int fd = open(file->second.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) throw exception("Cannot read ExeFS mod {}", path);
            return std::make_shared<vfs::OsBacking>(fd, true);
        }
        std::optional<vfs::Directory::EntryType> GetEntryTypeImpl(const std::string &path) override {
            if (overrides.contains(path)) return vfs::Directory::EntryType::File;
            return base->GetEntryType(path);
        }
        std::shared_ptr<vfs::Directory> OpenDirectoryImpl(const std::string &path, vfs::Directory::ListMode mode) override {
            return base->OpenDirectoryUnchecked(path, mode);
        }

      public:
        ExeFsOverlay(std::shared_ptr<vfs::FileSystem> base, std::map<std::string, fs::path> overrides)
            : base(std::move(base)), overrides(std::move(overrides)) {}
    };

    // Virtual RomFS image: patch the file table and append replacement data.
    // This avoids copying/decrypting an entire game image and supports differing file sizes.
    // Only replacements of existing RomFS paths are supported, not additions.
    class RomFsOverlayBacking final : public vfs::Backing {
        struct Segment {
            size_t offset;
            std::shared_ptr<vfs::Backing> file;
        };
        std::shared_ptr<vfs::Backing> original;
        const size_t originalSize;
        vfs::RomFileSystem::RomFsHeader header{};
        std::map<std::string, size_t> fileEntries;
        std::map<size_t, std::array<u8, 16>> tablePatches;
        std::vector<Segment> segments;
        std::set<u32> visitedDirs;
        std::set<u32> visitedFiles;

        std::string ReadName(size_t pos, u32 len) {
            if (len > 4096 || pos > originalSize || len > originalSize - pos)
                throw exception("Invalid RomFS entry name");
            std::string name(len, '\0');
            if (len && original->ReadUnchecked(span(reinterpret_cast<u8 *>(name.data()), len), pos) != len)
                throw exception("Cannot read RomFS entry name");
            return name;
        }

        void IndexFiles(u32 offset, const std::string &prefix) {
            while (offset != 0xFFFFFFFF) {
                if (!visitedFiles.insert(offset).second) throw exception("Cyclic RomFS file table");
                const auto pos = header.fileMetaTableOffset + offset;
                if (pos > originalSize || sizeof(vfs::RomFileSystem::RomFsFileEntry) > originalSize - pos)
                    throw exception("Invalid RomFS file table");
                const auto entry = original->Read<vfs::RomFileSystem::RomFsFileEntry>(pos);
                const auto name = ReadName(pos + sizeof(entry), entry.nameSize);
                if (!name.empty()) fileEntries.emplace(prefix.empty() ? name : prefix + "/" + name, pos);
                offset = entry.siblingOffset;
            }
        }

        void IndexDirs(u32 offset, const std::string &parent, size_t depth = 0) {
            if (depth > 64) throw exception("RomFS directory depth exceeded");
            while (offset != 0xFFFFFFFF) {
                if (!visitedDirs.insert(offset).second) throw exception("Cyclic RomFS directory table");
                const auto pos = header.dirMetaTableOffset + offset;
                if (pos > originalSize || sizeof(vfs::RomFileSystem::RomFsDirectoryEntry) > originalSize - pos)
                    throw exception("Invalid RomFS directory table");
                const auto entry = original->Read<vfs::RomFileSystem::RomFsDirectoryEntry>(pos);
                const auto name = ReadName(pos + sizeof(entry), entry.nameSize);
                const auto path = name.empty() ? parent : (parent.empty() ? name : parent + "/" + name);
                if (entry.fileOffset != 0xFFFFFFFF) IndexFiles(entry.fileOffset, path);
                if (entry.childOffset != 0xFFFFFFFF) IndexDirs(entry.childOffset, path, depth + 1);
                offset = entry.siblingOffset;
            }
        }

      protected:
        size_t ReadImpl(span<u8> output, size_t offset) override {
            if (offset >= size) return 0;
            const size_t length = std::min(output.size(), size - offset);
            size_t done = 0;
            if (offset < originalSize) {
                const size_t amount = std::min(length, originalSize - offset);
                if (original->ReadUnchecked(output.subspan(0, amount), offset) != amount) return 0;
                done = amount;
            }
            while (done < length) {
                const size_t absolute = offset + done;
                auto segment = std::find_if(segments.begin(), segments.end(), [absolute](const Segment &s) {
                    return absolute >= s.offset && absolute - s.offset < s.file->size;
                });
                if (segment == segments.end()) return done;
                const size_t relative = absolute - segment->offset;
                const size_t amount = std::min(length - done, segment->file->size - relative);
                if (segment->file->ReadUnchecked(output.subspan(done, amount), relative) != amount) return done;
                done += amount;
            }
            // Redirect file offsets/sizes in the guest-visible RomFS file metadata.
            for (const auto &[patchStart, bytes] : tablePatches) {
                if (patchStart >= offset + length || patchStart + bytes.size() <= offset) continue;
                const size_t begin = std::max(patchStart, offset);
                const size_t end = std::min(patchStart + bytes.size(), offset + length);
                std::memcpy(output.data() + begin - offset, bytes.data() + begin - patchStart, end - begin);
            }
            return length;
        }

      public:
        RomFsOverlayBacking(std::shared_ptr<vfs::Backing> original, const std::map<std::string, fs::path> &replacements)
            : vfs::Backing({true, false, false}, original->size), original(std::move(original)), originalSize(size) {
            header = this->original->Read<vfs::RomFileSystem::RomFsHeader>();
            if (header.headerSize != sizeof(header) ||
                header.dirMetaTableOffset > originalSize || header.fileMetaTableOffset > originalSize)
                throw exception("Invalid RomFS header for mod overlay");
            IndexDirs(0, "");
            for (const auto &[name, filePath] : replacements) {
                const auto found = fileEntries.find(name);
                if (found == fileEntries.end()) {
                    LOGW("RomFS mod path does not exist in base game: {}", name);
                    continue;
                }
                const int fd = open(filePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (fd < 0) { LOGW("Cannot open RomFS mod: {}", name); continue; }
                auto source = std::make_shared<vfs::OsBacking>(fd, true);
                if (!source->size || source->size > (1ULL << 32)) {
                    LOGW("Skipping invalid RomFS mod file: {}", name);
                    continue;
                }
                if (size < header.dataOffset || size > SIZE_MAX - source->size) throw exception("RomFS mod overlay overflow");
                const u64 relativeOffset = size - header.dataOffset;
                const u64 fileSize = source->size;
                std::array<u8, 16> patch{};
                std::memcpy(patch.data(), &relativeOffset, sizeof(relativeOffset));
                std::memcpy(patch.data() + 8, &fileSize, sizeof(fileSize));
                tablePatches.emplace(found->second + offsetof(vfs::RomFileSystem::RomFsFileEntry, offset), patch);
                segments.push_back({size, source});
                size += source->size;
                LOGI("Applied RomFS mod file: {}", name);
            }
        }
        size_t PatchCount() const { return tablePatches.size(); }
    };

    inline void LoadMods(Loader &loader, const std::string &publicFilesPath) {
        if (!loader.programId) return;
        const auto base = fs::path(publicFilesPath) / "mods" / fmt::format("{:016X}", loader.programId);
        if (!fs::is_directory(base)) return;
        std::map<std::string, fs::path> exeFsReplacements;
        std::map<std::string, fs::path> romFsReplacements;
        std::vector<fs::path> exeFsPatches;
        unsigned cheats = 0;
        try {
            // Ordered mod names have deterministic override precedence.
            std::vector<fs::path> mods;
            for (const auto &item : fs::directory_iterator(base)) {
                if (item.is_directory() && !item.is_symlink() && !item.path().filename().string().empty() && item.path().filename().string().front() != '.' &&
                    !fs::exists(item.path() / ".disabled")) mods.emplace_back(item.path());
            }
            std::sort(mods.begin(), mods.end());
            for (const auto &mod : mods) for (const std::string type : {"romfs", "exefs", "cheats"}) {
                const auto directory = mod / type;
                if (!fs::is_directory(directory) || fs::is_symlink(directory)) continue;
                for (const auto &entry : fs::recursive_directory_iterator(directory, fs::directory_options::skip_permission_denied)) {
                    if (!entry.is_regular_file() || entry.is_symlink()) continue;
                    const auto relative = entry.path().lexically_relative(directory).generic_string();
                    if (relative.empty() || relative.starts_with("..")) continue;
                    if (type == "romfs") romFsReplacements[relative] = entry.path();
                    else if (type == "exefs") {
                        if (entry.path().extension() == ".pchtxt") {
                            exeFsPatches.push_back(entry.path());
                            continue;
                        }
                        // Executable replacement only: limit to known ExeFS module names.
                        if (relative == "main" || relative == "rtld" || relative == "sdk" ||
                            relative == "main.npdm" || relative.starts_with("subsdk"))
                            exeFsReplacements[relative] = entry.path();
                    } else cheats++;
                }
            }
            loader.modExeFsOverrides = std::move(exeFsReplacements);
            loader.modExeFsPatches = std::move(exeFsPatches);
            if (!romFsReplacements.empty() && loader.romFs) {
                auto overlay = std::make_shared<RomFsOverlayBacking>(loader.romFs, romFsReplacements);
                if (overlay->PatchCount()) loader.romFs = std::move(overlay);
            }
            if (cheats) LOGW("{} cheat text files found; guest-memory cheat execution is not yet supported", cheats);
        } catch (const std::exception &error) {
            LOGW("Cannot load game mods; continuing without remaining overrides: {}", error.what());
        }
    }
}
