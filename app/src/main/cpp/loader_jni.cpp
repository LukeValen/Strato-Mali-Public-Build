// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "skyline/crypto/key_store.h"
#include "skyline/vfs/nca.h"
#include "skyline/vfs/os_backing.h"
#include "skyline/vfs/os_filesystem.h"
#include "skyline/loader/nro.h"
#include "skyline/loader/nso.h"
#include "skyline/loader/nca.h"
#include "skyline/loader/xci.h"
#include "skyline/loader/nsp.h"
#include "skyline/jvm.h"
#include "skyline/loader/installed_content.h"

extern "C" JNIEXPORT jstring JNICALL Java_org_stratoemu_strato_utils_GameContentUtils_validatePackage(JNIEnv *env, jobject, jint contentFd, jint gameFd, jint gameFormat, jstring keysPath, jstring gameId) {
    skyline::signal::ScopedStackBlocker stackBlocker;
    try {
        auto keys = std::make_shared<skyline::crypto::KeyStore>(skyline::JniString(env, keysPath));
        std::string idString = skyline::JniString(env, gameId);
        if (idString.size() != 16 || idString.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw std::runtime_error("Invalid game title ID");
        auto id = std::stoull(idString, nullptr, 16);
        auto base = std::make_shared<skyline::vfs::OsBacking>(gameFd);
        std::unique_ptr<skyline::loader::Loader> loader;
        switch (static_cast<skyline::loader::RomFormat>(gameFormat)) {
            case skyline::loader::RomFormat::NSP: loader = std::make_unique<skyline::loader::NspLoader>(base, keys); break;
            case skyline::loader::RomFormat::XCI: loader = std::make_unique<skyline::loader::XciLoader>(base, keys); break;
            default: throw std::runtime_error("Update/DLC installation requires an NSP or XCI base game");
        }
        if (loader->programId != id) throw std::runtime_error("Base program ID does not match the selected game");
        std::vector<skyline::loader::content::Package> packages;
        packages.emplace_back(std::make_shared<skyline::vfs::OsBacking>(contentFd), keys, id, true);
        skyline::loader::content::Apply(*loader, packages, keys);
        return nullptr;
    } catch (const skyline::loader::loader_exception &e) {
        const char *message = "Invalid content or missing decryption keys";
        switch (e.error) {
            case skyline::loader::LoaderResult::MissingHeaderKey: message = "Missing header_key in prod.keys"; break;
            case skyline::loader::LoaderResult::MissingTitleKey: message = "Missing title key or ticket"; break;
            case skyline::loader::LoaderResult::MissingTitleKek: message = "Missing titlekek in prod.keys"; break;
            case skyline::loader::LoaderResult::MissingKeyArea: message = "Missing key_area_key in prod.keys"; break;
            default: break;
        }
        return env->NewStringUTF(message);
    } catch (const std::exception &e) {
        return env->NewStringUTF(e.what());
    }
}

extern "C" JNIEXPORT jint JNICALL Java_org_stratoemu_strato_loader_RomFile_populate(JNIEnv *env, jobject thiz, jint jformat, jint fd, jstring appFilesPathJstring, jint systemLanguage) {
    skyline::signal::ScopedStackBlocker stackBlocker;

    skyline::loader::RomFormat format{static_cast<skyline::loader::RomFormat>(jformat)};

    auto keyStore{std::make_shared<skyline::crypto::KeyStore>(skyline::JniString(env, appFilesPathJstring))};
    std::unique_ptr<skyline::loader::Loader> loader;
    try {
        auto backing{std::make_shared<skyline::vfs::OsBacking>(fd)};

        switch (format) {
            case skyline::loader::RomFormat::NRO:
                loader = std::make_unique<skyline::loader::NroLoader>(backing);
                break;
            case skyline::loader::RomFormat::NSO:
                loader = std::make_unique<skyline::loader::NsoLoader>(backing);
                break;
            case skyline::loader::RomFormat::NCA:
                loader = std::make_unique<skyline::loader::NcaLoader>(backing, keyStore);
                break;
            case skyline::loader::RomFormat::XCI:
                loader = std::make_unique<skyline::loader::XciLoader>(backing, keyStore);
                break;
            case skyline::loader::RomFormat::NSP:
                loader = std::make_unique<skyline::loader::NspLoader>(backing, keyStore);
                break;
            default:
                return static_cast<jint>(skyline::loader::LoaderResult::ParsingError);
        }
    } catch (const skyline::loader::loader_exception &e) {
        return static_cast<jint>(e.error);
    } catch (const std::exception &e) {
        return static_cast<jint>(skyline::loader::LoaderResult::ParsingError);
    }

    jclass clazz{env->GetObjectClass(thiz)};
    jfieldID applicationNameField{env->GetFieldID(clazz, "applicationName", "Ljava/lang/String;")};
    jfieldID applicationTitleIdField{env->GetFieldID(clazz, "applicationTitleId", "Ljava/lang/String;")};
    jfieldID applicationAuthorField{env->GetFieldID(clazz, "applicationAuthor", "Ljava/lang/String;")};
    jfieldID rawIconField{env->GetFieldID(clazz, "rawIcon", "[B")};
    jfieldID applicationVersionField{env->GetFieldID(clazz, "applicationVersion", "Ljava/lang/String;")};

    if (loader->nacp) {
        auto language{skyline::language::GetApplicationLanguage(static_cast<skyline::language::SystemLanguage>(systemLanguage))};
        if (((1 << static_cast<skyline::u32>(language)) & loader->nacp->supportedTitleLanguages) == 0)
            language = loader->nacp->GetFirstSupportedTitleLanguage();

        env->SetObjectField(thiz, applicationNameField, env->NewStringUTF(loader->nacp->GetApplicationName(language).c_str()));
        env->SetObjectField(thiz, applicationVersionField, env->NewStringUTF(loader->nacp->GetApplicationVersion().c_str()));
        env->SetObjectField(thiz, applicationTitleIdField, env->NewStringUTF(loader->nacp->GetSaveDataOwnerId().c_str()));
        env->SetObjectField(thiz, applicationAuthorField, env->NewStringUTF(loader->nacp->GetApplicationPublisher(language).c_str()));

        auto icon{loader->GetIcon(language)};
        jbyteArray iconByteArray{env->NewByteArray(static_cast<jsize>(icon.size()))};
        env->SetByteArrayRegion(iconByteArray, 0, static_cast<jsize>(icon.size()), reinterpret_cast<const jbyte *>(icon.data()));
        env->SetObjectField(thiz, rawIconField, iconByteArray);
    }

    return static_cast<jint>(skyline::loader::LoaderResult::Success);
}

extern "C" JNIEXPORT jstring Java_org_stratoemu_strato_preference_FirmwareImportPreference_fetchFirmwareVersion(JNIEnv *env, jobject thiz, jstring systemArchivesPathJstring, jstring keysPathJstring) {
    struct SystemVersion {
        skyline::u8 major;
        skyline::u8 minor;
        skyline::u8 micro;
        skyline::u8 _pad0_;
        skyline::u8 revisionMajor;
        skyline::u8 revisionMinor;
        skyline::u8 _pad1_[2];
        skyline::u8 platformString[0x20];
        skyline::u8 versionHash[0x40];
        skyline::u8 displayVersion[0x18];
        skyline::u8 displayTitle[0x80];
    };

    constexpr skyline::u64 systemVersionProgramId{0x0100000000000809};

    auto systemArchivesFileSystem{std::make_shared<skyline::vfs::OsFileSystem>(skyline::JniString(env, systemArchivesPathJstring))};
    auto systemArchives{systemArchivesFileSystem->OpenDirectory("")};
    auto keyStore{std::make_shared<skyline::crypto::KeyStore>(skyline::JniString(env, keysPathJstring))};

    for (const auto &entry : systemArchives->Read()) {
        std::shared_ptr<skyline::vfs::Backing> backing{systemArchivesFileSystem->OpenFile(entry.name)};
        auto nca{skyline::vfs::NCA(backing, keyStore)};

        if (nca.header.programId == systemVersionProgramId && nca.romFs != nullptr) {
            auto controlRomFs{std::make_shared<skyline::vfs::RomFileSystem>(nca.romFs)};
            auto file{controlRomFs->OpenFile("file")};
            if (!file)
                throw skyline::exception("Firmware version file is missing");
            SystemVersion systemVersion{};
            file->Read<SystemVersion>(systemVersion);
            const auto *version{reinterpret_cast<const char *>(systemVersion.displayVersion)};
            const std::string boundedVersion(version, strnlen(version, sizeof(systemVersion.displayVersion)));
            return env->NewStringUTF(boundedVersion.c_str());
        }
    }

    return env->NewStringUTF("");
}

std::vector<skyline::u8> decodeBfttfFont(const std::shared_ptr<skyline::vfs::Backing> bfttfFile){
    constexpr skyline::u32 fontKey{0x06186249};
    constexpr skyline::u32 BFTTFMagic{0x18029a7f};

    auto firstBytes{bfttfFile->Read<skyline::u32>()};
    auto firstBytesXor{firstBytes ^ fontKey};

    if (firstBytesXor == BFTTFMagic) {
        constexpr size_t initialOffset{8};
        if (bfttfFile->size < initialOffset || bfttfFile->size % sizeof(skyline::u32))
            throw skyline::exception("Firmware font has a truncated header or word");
        std::vector<skyline::u8> font(bfttfFile->size - initialOffset);

        for (size_t offset = initialOffset; offset < bfttfFile->size; offset += 4) {
            skyline::u32 decodedData{bfttfFile->Read<skyline::u32>(offset) ^ fontKey};

            font[offset - 8] = static_cast<skyline::u8>(decodedData >> 0);
            font[offset - 7] = static_cast<skyline::u8>(decodedData >> 8);
            font[offset - 6] = static_cast<skyline::u8>(decodedData >> 16);
            font[offset - 5] = static_cast<skyline::u8>(decodedData >> 24);
        }

        return font;
    }
    return {};
}

extern "C" JNIEXPORT void Java_org_stratoemu_strato_preference_FirmwareImportPreference_extractFonts(JNIEnv *env, jobject thiz, jstring systemArchivesPathJstring, jstring keysPathJstring, jstring fontsPath) {
    // Fonts are stored in the following NCAs
    // 0x0100000000000810 -> "FontNintendoExtended"
    // 0x0100000000000811 -> "FontStandard"
    // 0x0100000000000812 -> "FontKorean"
    // 0x0100000000000813 -> "FontChineseTraditional"
    // 0x0100000000000814 -> "FontChineseSimplified"

    constexpr skyline::u64 firstFontProgramId{0x0100000000000810};
    constexpr skyline::u64 lastFontProgramId{0x0100000000000814};

    const std::map<std::string, std::string> sharedFontFilenameDictionary = {
        {"nintendo_ext_003.bfttf", "FontNintendoExtended"},
        {"nintendo_ext2_003.bfttf", "FontNintendoExtended2"},
        {"nintendo_udsg-r_std_003.bfttf", "FontStandard"},
        {"nintendo_udsg-r_ko_003.bfttf", "FontKorean"},
        {"nintendo_udjxh-db_zh-tw_003.bfttf", "FontChineseTraditional"},
        {"nintendo_udsg-r_org_zh-cn_003.bfttf", "FontChineseSimplified"},
        {"nintendo_udsg-r_ext_zh-cn_003.bfttf", "FontExtendedChineseSimplified"}
    };

    auto fontsFileSystem{std::make_shared<skyline::vfs::OsFileSystem>(skyline::JniString(env, fontsPath))};
    auto systemArchivesFileSystem{std::make_shared<skyline::vfs::OsFileSystem>(skyline::JniString(env, systemArchivesPathJstring))};
    auto systemArchives{systemArchivesFileSystem->OpenDirectory("")};
    auto keyStore{std::make_shared<skyline::crypto::KeyStore>(skyline::JniString(env, keysPathJstring))};

    for (const auto &entry : systemArchives->Read()) {
        std::shared_ptr<skyline::vfs::Backing> backing{systemArchivesFileSystem->OpenFile(entry.name)};
        auto nca{skyline::vfs::NCA(backing, keyStore)};

        if (nca.header.programId >= firstFontProgramId && nca.header.programId <= lastFontProgramId && nca.romFs != nullptr) {
            auto controlRomFs{std::make_shared<skyline::vfs::RomFileSystem>(nca.romFs)};

            for (auto fileEntry = controlRomFs->fileMap.begin(); fileEntry != controlRomFs->fileMap.end(); fileEntry++) {
                auto fileName{fileEntry->first};
                auto bfttfFile{controlRomFs->OpenFile(fileName)};

                auto decodedFont{decodeBfttfFont(bfttfFile)};
                if (decodedFont.empty())
                    continue;

                auto ttfFileName{sharedFontFilenameDictionary.at(fileName) + ".ttf"};
                if (fontsFileSystem->FileExists(ttfFileName))
                    fontsFileSystem->DeleteFile(ttfFileName);

                fontsFileSystem->CreateFile(ttfFileName, decodedFont.size());
                std::shared_ptr<skyline::vfs::Backing> ttfFile{fontsFileSystem->OpenFile(ttfFileName, {true, true, false})};

                ttfFile->Write(decodedFont);
            }
        }
    }
}

// Setup reuses the existing firmware readers, converting native validation failures
// to Java IOExceptions so invalid archives/keys cannot terminate the setup activity.
extern "C" JNIEXPORT jstring Java_org_stratoemu_strato_setup_FirmwareInstaller_fetchFirmwareVersion(JNIEnv *env, jobject thiz, jstring archives, jstring keys) {
    try {
        return Java_org_stratoemu_strato_preference_FirmwareImportPreference_fetchFirmwareVersion(env, thiz, archives, keys);
    } catch (const std::exception &error) {
        env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
        return nullptr;
    }
}

extern "C" JNIEXPORT void Java_org_stratoemu_strato_setup_FirmwareInstaller_extractFonts(JNIEnv *env, jobject thiz, jstring archives, jstring keys, jstring fonts) {
    try {
        Java_org_stratoemu_strato_preference_FirmwareImportPreference_extractFonts(env, thiz, archives, keys, fonts);
    } catch (const std::exception &error) {
        env->ThrowNew(env->FindClass("java/io/IOException"), error.what());
    }
}
