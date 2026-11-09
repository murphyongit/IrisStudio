#include "Detect.hpp"

#include <Windows.h>

#include <fstream>
#include <mutex>

#include "nlohmann/json.hpp"

#include "Components/AnimationPackLogic.hpp"
#include "Platform.hpp"
#include "Setup/Folders.hpp"
#include "../res/resource.h"

namespace Launcher
{
    namespace Setup = IWXMVM::Setup;

    const Catalogs& LoadCatalogs()
    {
        static std::once_flag once;
        static Catalogs catalogs;
        static std::string error;
        std::call_once(once, [] {
            try
            {
                const auto characters = Platform::ResourceText(IDR_CHARACTERS);
                const auto animations = Platform::ResourceText(IDR_ANIMATIONS);
                catalogs.zones = Setup::ReadCharacterZones(characters);
                // the campaign maps' kits, built as character zones are
                for (auto& kit : Setup::ReadKitZones(Platform::ResourceText(IDR_CAMPAIGN_KITS)))
                    catalogs.zones.push_back(std::move(kit));
                // the campaign's and Zombies' weapons and their sounds
                for (auto& weapon : Setup::ReadWeaponZones(Platform::ResourceText(IDR_CAMPAIGN_WEAPONS)))
                    catalogs.zones.push_back(std::move(weapon));
                // and the Change Map menu's pictures, copied with them
                catalogs.pictures = Setup::ReadMenuPictures(Platform::ResourceText(IDR_CAMPAIGN_KITS));
                catalogs.weaponTextures = Setup::ReadWeaponTextures(Platform::ResourceText(IDR_CAMPAIGN_WEAPONS));
                catalogs.animations = Setup::ReadAnimationCatalog(animations);
                catalogs.charactersHash = Setup::Hash(Platform::Resource(IDR_CHARACTERS));
                catalogs.animationsHash = Setup::Hash(Platform::Resource(IDR_ANIMATIONS));
            }
            catch (const std::exception& e)
            {
                error = e.what();
            }
        });
        if (!error.empty())
            throw std::runtime_error(error);
        return catalogs;
    }

    std::optional<SetupRecord> SetupRecord::Load(const std::filesystem::path& file)
    {
        const auto text = Platform::ReadFile(file);
        if (!text)
            return std::nullopt;
        const auto json = nlohmann::json::parse(*text, nullptr, false);
        if (!json.is_object() || json.value("version", 0) != 1)
            return std::nullopt;
        SetupRecord record;
        record.fingerprint = json.value("fingerprint", std::string());
        record.characters = json.value("characters", std::string());
        record.animations = json.value("animations", std::string());
        record.gameFolder = json.value("gameFolder", std::string());
        if (json.contains("zoneFiles") && json["zoneFiles"].is_object())
            for (const auto& [path, size] : json["zoneFiles"].items())
                if (size.is_number_unsigned())
                    record.zoneFiles.push_back({path, size.get<std::uint64_t>()});
        record.zones = json.value("zones", std::size_t{0});
        record.animationsWritten = json.value("animationsWritten", std::size_t{0});
        return record;
    }

    bool SetupRecord::Save(const std::filesystem::path& file) const
    {
        const nlohmann::json json = {
            {"version", 1},
            {"generator", "Iris Studio"},
            {"fingerprint", fingerprint},
            {"characters", characters},
            {"animations", animations},
            {"gameFolder", gameFolder},
            {"zones", zones},
            {"animationsWritten", animationsWritten},
            {"zoneFiles", nlohmann::json::object()},
        };
        auto written = json;
        for (const auto& zoneFile : zoneFiles)
            written["zoneFiles"][zoneFile.path] = zoneFile.size;
        return Platform::WriteFile(file, written.dump(2) + "\n");
    }

    bool IsGameFolder(const std::filesystem::path& folder)
    {
        std::error_code error;
        return !folder.empty() && std::filesystem::is_regular_file(folder / "BlackOpsMP.exe", error) &&
               std::filesystem::is_directory(folder / "zone" / "Common", error);
    }

    std::optional<std::filesystem::path> GameFolderOf(const std::filesystem::path& picked)
    {
        // the folder, or the one it is in two levels up at most (zone\Common)
        auto folder = picked;
        for (int up = 0; up < 3 && !folder.empty(); ++up)
        {
            if (IsGameFolder(folder))
                return folder;
            if (folder.parent_path() == folder)
                break;
            folder = folder.parent_path();
        }
        return std::nullopt;
    }

    Setup::GameZones ListGameZones(const std::filesystem::path& gameFolder, std::string* fingerprint,
                                   std::vector<Setup::ZoneFile>* listed)
    {
        Setup::GameZones zones;
        std::vector<Setup::ZoneFile> files;
        std::error_code error;
        for (const auto& folder : std::filesystem::directory_iterator(gameFolder / "zone", error))
        {
            if (!folder.is_directory(error))
                continue;
            const auto name = Platform::Utf8(folder.path().filename());
            std::vector<std::string> names;
            for (const auto& file : std::filesystem::directory_iterator(folder.path(), error))
            {
                if (!file.is_regular_file(error) || _wcsicmp(file.path().extension().c_str(), L".ff") != 0)
                    continue;
                const auto fileName = Platform::Utf8(file.path().filename());
                names.push_back(fileName);
                files.push_back({name + "\\" + fileName, file.file_size(error)});
            }
            if (_stricmp(name.c_str(), "Common") == 0)
                zones.common = std::move(names);
            else
                zones.languages.emplace_back(name, std::move(names));
        }
        if (listed)
            *listed = files;
        if (fingerprint)
            *fingerprint = Setup::Fingerprint(std::move(files));
        return zones;
    }

    std::filesystem::path FindFFmpeg(const Options& options)
    {
        std::error_code error;
        // the mod's places (CaptureManager::GetFFmpegPath), then the launcher's own folder when it is another
        auto candidates = Setup::FFmpegPlaces(Platform::LocalAppData(), Platform::RoamingAppData());
        candidates.push_back(options.home / "ffmpeg.exe");
        for (const auto& candidate : candidates)
            if ((!options.ownTools || candidate.native().starts_with(options.home.native())) &&
                std::filesystem::is_regular_file(candidate, error))
                return candidate;
        return {};
    }

    std::filesystem::path FindLinker(const Options& options)
    {
        const std::string folder = "oat-" + std::string(OAT_VERSION);
        for (const auto& candidate : {options.home / "tools" / folder / "Linker.exe",
                                      Platform::LocalAppData() / "IWXMVM" / "tools" / folder / "Linker.exe"})
        {
            if (options.ownTools && !candidate.native().starts_with(options.home.native()))
                continue;
            const auto hash = Platform::Sha256File(candidate);
            if (hash && Platform::Hex(*hash) == LINKER_SHA256)
                return candidate;
        }
        return {};
    }

    namespace
    {
        // the Black Ops folder of a Steam library that has it
        std::filesystem::path FromSteam()
        {
            const auto steam = Platform::RegistryString(true, L"Software\\Valve\\Steam", L"SteamPath");
            if (!steam)
                return {};
            const std::filesystem::path steamFolder = *steam;
            std::optional<std::string> folders;
            for (const auto& file : {steamFolder / "steamapps" / "libraryfolders.vdf",
                                     steamFolder / "config" / "libraryfolders.vdf"})
                if ((folders = Platform::ReadFile(file)))
                    break;
            auto libraries = folders ? Setup::SteamLibraries(*folders, Setup::BLACK_OPS_APP_ID)
                                     : std::vector<Setup::SteamLibrary>{};
            // Steam's own folder is a library too
            libraries.push_back({steamFolder, false});
            for (const auto& library : libraries)
            {
                const auto steamapps = library.path / "steamapps";
                std::string installDir(Setup::BLACK_OPS_INSTALL_DIR);
                if (const auto manifest =
                        Platform::ReadFile(steamapps / ("appmanifest_" + std::string(Setup::BLACK_OPS_APP_ID) + ".acf")))
                    installDir = Setup::InstallDir(*manifest).value_or(installDir);
                const auto folder = steamapps / "common" / Platform::FromUtf8(installDir);
                if (IsGameFolder(folder))
                    return std::filesystem::path(folder).make_preferred();
            }
            return {};
        }

        // the folder Windows' list of programs has for Black Ops (Steam writes it when it installs the game)
        std::filesystem::path FromUninstall()
        {
            const auto key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 42700";
            for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
                if (const auto location = Platform::RegistryString(false, key, L"InstallLocation", view))
                    if (IsGameFolder(*location))
                        return *location;
            return {};
        }
    }  // namespace

    Detection Detect(const Options& options, const Settings& settings)
    {
        Detection detection;

        // Plutonium (never its config.json, which holds the user's token)
        std::error_code error;
        detection.bootstrapper = options.plutonium / "bin" / "plutonium-bootstrapper-win32.exe";
        if (!std::filesystem::is_regular_file(detection.bootstrapper, error))
            detection.plutonium = PlutoniumState::Missing;
        else if (!std::filesystem::is_regular_file(options.plutonium / "games" / "t5mp.exe", error))
            detection.plutonium = PlutoniumState::NotUpdated;
        else
            detection.plutonium = PlutoniumState::Installed;

        // the game: the folder of the settings, then Steam's
        if (!settings.gameFolder.empty())
        {
            if (IsGameFolder(settings.gameFolder))
            {
                detection.gameFolder = settings.gameFolder;
                detection.gameFrom = GameFrom::Settings;
            }
            else
            {
                detection.savedFolderGone = true;
            }
        }
        if (detection.gameFolder.empty() && !options.noSteam)
        {
            if (auto folder = FromSteam(); !folder.empty())
                detection.gameFolder = folder, detection.gameFrom = GameFrom::Steam;
            else if (folder = FromUninstall(); !folder.empty())
                detection.gameFolder = folder, detection.gameFrom = GameFrom::Uninstall;
        }

        // (the mod's folder is storage\t5\iris, beside storage\t5\mods)
        std::vector<std::filesystem::path> modsFolders = {options.storage.parent_path() / "mods"};
        if (!detection.gameFolder.empty())
            modsFolders.push_back(detection.gameFolder / "mods");
        detection.mods = IWXMVM::Components::DemoModLogic::FindMods(modsFolders);

        detection.linker = FindLinker(options);
        detection.ffmpeg = FindFFmpeg(options);
        detection.freeBytes = Platform::FreeSpace(options.storage);
        detection.record = SetupRecord::Load(options.storage / "setup.json");

        const Catalogs* catalogs = nullptr;
        try
        {
            catalogs = &LoadCatalogs();
        }
        catch (const std::exception& e)
        {
            detection.catalogError = e.what();
            return detection;
        }
        detection.zonesTotal = catalogs->zones.size();
        detection.packTotal = catalogs->animations.animations.size();
        if (detection.gameFolder.empty())
            return detection;

        detection.gameZones = ListGameZones(detection.gameFolder, &detection.fingerprint, &detection.zoneFiles);

        // the zones: those whose level the game has (a weapon's: one of them), built
        const auto zonesFolder = options.storage / "zones";
        for (const auto& zone : catalogs->zones)
        {
            if (!Setup::ChooseLevel(detection.gameZones, zone))
                continue;
            ++detection.zonesWanted;
            const auto file = zonesFolder / Platform::FromUtf8(zone.name + ".ff");
            const auto size = std::filesystem::file_size(file, error);
            if (!error && size > 0)
            {
                ++detection.zonesPresent;
                detection.zonesBytes += size;
            }
        }
        // the menu's pictures, of the zones the game has, copied
        const auto picturesFolder = options.storage / std::string(Setup::MENU_PICTURES_FOLDER);
        for (const auto& picture : catalogs->pictures)
        {
            if (!detection.gameZones.HasCommon(picture.zone))
                continue;
            ++detection.picturesWanted;
            if (std::filesystem::file_size(picturesFolder / Platform::FromUtf8(picture.image + ".dds"), error) > 0 &&
                !error)
                ++detection.picturesPresent;
        }
        // the weapons' textures of Rezurrection's files, into Plutonium's storage\t5\main (the mod's folder is its
        // storage\t5\iris) when the game has those files
        if (!catalogs->weaponTextures.images.empty() && !catalogs->weaponTextures.file.empty() &&
            options.storage.parent_path().filename() == "t5" &&
            std::filesystem::is_regular_file(
                detection.gameFolder / "main" / std::string(Setup::REZURRECTION_IWDS.front()), error))
        {
            detection.weaponTexturesFile =
                options.storage.parent_path() / "main" / Platform::FromUtf8(catalogs->weaponTextures.file);
            // (and with the catalog's images: an older file has fewer. A zip lists them at its end.)
            const auto size = std::filesystem::file_size(detection.weaponTexturesFile, error);
            if (!error && size > 0)
            {
                std::ifstream file(detection.weaponTexturesFile, std::ios::binary);
                std::string end(static_cast<std::size_t>(std::min<std::uintmax_t>(size, 1 << 20)), '\0');
                file.seekg(static_cast<std::streamoff>(size - end.size()));
                file.read(end.data(), static_cast<std::streamsize>(end.size()));
                detection.weaponTexturesPresent = file && Setup::NamesAllImages(end, catalogs->weaponTextures.images);
            }
        }
        // out of date when a zone they were built from changed, wherever the game is now, or the catalog did
        if (detection.record)
        {
            const auto stale = Setup::GetZonesStale(detection.record->zoneFiles, detection.record->fingerprint,
                                                    detection.zoneFiles, detection.fingerprint,
                                                    detection.record->characters, catalogs->charactersHash);
            detection.zonesStale = stale != Setup::ZonesStale::No;
            detection.newCharacters = stale == Setup::ZonesStale::NewCharacters;
        }

        // the pack: every animation of a zone the game has, with the catalog's hash
        const auto& animationZones = catalogs->animations.zones;
        std::vector<bool> zoneThere(animationZones.size());
        for (std::size_t i = 0; i < animationZones.size(); ++i)
            zoneThere[i] = detection.gameZones.HasCommon(animationZones[i]);
        const auto packFile = options.storage / "animations.pack";
        std::unordered_map<std::string, IWXMVM::AnimationPackLogic::IndexEntry> index;
        detection.packPresent = std::filesystem::is_regular_file(packFile, error);
        if (detection.packPresent)
        {
            detection.packBytes = std::filesystem::file_size(packFile, error);
            try
            {
                std::ifstream stream(packFile, std::ios::binary);
                index = IWXMVM::AnimationPackLogic::ReadIndex(stream);
            }
            catch (const std::exception& e)
            {
                detection.packError = e.what();
            }
        }
        for (const auto& animation : catalogs->animations.animations)
        {
            if (std::none_of(animation.zones.begin(), animation.zones.end(), [&](auto z) { return zoneThere[z]; }))
                continue;
            ++detection.packWanted;
            const auto it = index.find(animation.key);
            if (it != index.end() && it->second.hash == animation.hash)
                ++detection.packMatching;
        }
        return detection;
    }
}  // namespace Launcher
