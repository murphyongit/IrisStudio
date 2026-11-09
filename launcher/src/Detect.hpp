#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "App.hpp"
#include "Components/DemoModLogic.hpp"
#include "Setup/SetupLogic.hpp"

namespace Launcher
{
    // The catalogs the launcher embeds (core/resources): the character zones to build (and the campaign maps' kits,
    // which are built the same way) and the animations of the pack
    struct Catalogs
    {
        std::vector<IWXMVM::Setup::CharacterZone> zones;
        // the Change Map menu's pictures of campaign levels and Zombies maps, copied with the zones
        std::vector<IWXMVM::Setup::MenuPicture> pictures;
        // the weapons' textures only single player's Rezurrection files have, written with the zones
        IWXMVM::Setup::WeaponTextures weaponTextures;
        IWXMVM::Setup::AnimationCatalog animations;
        std::string charactersHash;  // of the files, so a setup knows which catalogs it was built for
        std::string animationsHash;
    };
    // read once, on the first call; throws std::runtime_error if they don't read
    const Catalogs& LoadCatalogs();

    // What a setup built, in setup.json next to the zones (the mod's storage\t5\iris): the fingerprint of the game's
    // zones and the catalogs it was built from
    struct SetupRecord
    {
        std::string fingerprint;
        std::string characters;
        std::string animations;
        std::string gameFolder;
        std::vector<IWXMVM::Setup::ZoneFile> zoneFiles;  // the game's zone files it was built from
        std::size_t zones = 0;
        std::size_t animationsWritten = 0;

        static std::optional<SetupRecord> Load(const std::filesystem::path& file);
        bool Save(const std::filesystem::path& file) const;
    };

    // The pinned OpenAssetTools release whose Linker builds the zones (setup.ps1 has the same)
    inline constexpr std::string_view OAT_VERSION = "v0.34.0";
    inline constexpr std::string_view OAT_ZIP_URL =
        "https://github.com/Laupetin/OpenAssetTools/releases/download/v0.34.0/oat-windows.zip";
    inline constexpr std::string_view OAT_ZIP_SHA256 = "fd2aafd5a6a99d520f67c16ab6e12873da87e8de0b81312d239a1ee2db59d515";
    inline constexpr std::string_view LINKER_SHA256 = "3e9cc19d44d37cd0af61d7c597457fcb9da919c7e31d4d5a197055c13461c6b4";

    // The pinned FFmpeg build the setup downloads when the mod has none: gyan.dev's essentials build of FFmpeg 9.0.2,
    // from its GitHub mirror (GPL-3), checked against the SHA-256 GitHub publishes for it; only bin\ffmpeg.exe is kept
    inline constexpr std::string_view FFMPEG_ZIP_URL =
        "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip";
    inline constexpr std::string_view FFMPEG_ZIP_SHA256 =
        "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba";
    inline constexpr std::size_t FFMPEG_ZIP_BYTES = 114768076;

    enum class PlutoniumState
    {
        Missing,     // no bootstrapper
        NotUpdated,  // Plutonium's own launcher hasn't downloaded T5 yet
        Installed,
    };

    enum class GameFrom
    {
        None,
        Settings,   // the folder picked (in the summary or Settings) or found before
        Steam,      // a Steam library that has Black Ops
        Uninstall,  // Windows' list of installed programs
    };

    // What the launcher finds, worked out on a thread of its own
    struct Detection
    {
        PlutoniumState plutonium = PlutoniumState::Missing;
        std::filesystem::path bootstrapper;

        std::filesystem::path gameFolder;
        GameFrom gameFrom = GameFrom::None;
        bool savedFolderGone = false;  // the folder in the settings isn't a Black Ops folder anymore
        std::vector<IWXMVM::Setup::ZoneFile> zoneFiles;
        IWXMVM::Setup::GameZones gameZones;
        std::string fingerprint;

        std::size_t zonesTotal = 0;    // in the catalog
        std::size_t zonesWanted = 0;   // of those, the ones whose level this game has
        std::size_t zonesPresent = 0;  // of those, the ones built
        std::uint64_t zonesBytes = 0;
        bool zonesStale = false;     // built from other game files or another catalog
        std::size_t picturesWanted = 0;   // the menu's pictures whose zone this game has
        std::size_t picturesPresent = 0;  // of those, the ones copied
        // the weapons' textures file (Plutonium's storage\t5\main): where it goes (empty
        // when it has nowhere to go: a mod folder outside Plutonium's storage, a game without Rezurrection's files),
        // and whether it is written
        std::filesystem::path weaponTexturesFile;
        bool weaponTexturesPresent = false;
        bool newCharacters = false;  // stale for another catalog alone: the game's files are the same

        bool packPresent = false;
        std::size_t packTotal = 0;     // in the catalog
        std::size_t packWanted = 0;    // of those, the ones in a zone this game has
        std::size_t packMatching = 0;  // of those, the ones the pack has with the catalog's hash
        std::uint64_t packBytes = 0;
        std::string packError;

        // the game's mods: the folders of Plutonium's storage\t5\mods and the game folder's mods with a mod.ff or an
        // .iwd
        std::vector<IWXMVM::Components::DemoModLogic::GameMod> mods;

        std::filesystem::path linker;  // OpenAssetTools' Linker.exe, checked against its SHA-256
        std::filesystem::path ffmpeg;
        std::uint64_t freeBytes = 0;
        std::optional<SetupRecord> record;
        std::string catalogError;

        bool GameFound() const
        {
            return !gameFolder.empty();
        }
        bool ZonesReady() const
        {
            return zonesWanted > 0 && zonesPresent >= zonesWanted && !zonesStale && picturesPresent >= picturesWanted &&
                   (weaponTexturesFile.empty() || weaponTexturesPresent);
        }
        bool PackReady() const
        {
            return packPresent && packWanted > 0 && packMatching >= packWanted;
        }
    };

    // whether a folder is Black Ops': BlackOpsMP.exe and zone\Common in it
    bool IsGameFolder(const std::filesystem::path& folder);
    // the Black Ops folder a picked folder is in or is (its parent when zone or main was picked); none if neither
    std::optional<std::filesystem::path> GameFolderOf(const std::filesystem::path& picked);
    // the zone files of a game folder
    IWXMVM::Setup::GameZones ListGameZones(const std::filesystem::path& gameFolder, std::string* fingerprint = nullptr,
                                           std::vector<IWXMVM::Setup::ZoneFile>* files = nullptr);
    // where the mod finds FFmpeg (CaptureManager::GetFFmpegPath); empty if in neither place
    std::filesystem::path FindFFmpeg(const Options& options);
    // OpenAssetTools' Linker.exe of the pinned version, in the launcher's tools or setup.ps1's; empty if neither
    std::filesystem::path FindLinker(const Options& options);

    Detection Detect(const Options& options, const Settings& settings);
}  // namespace Launcher
