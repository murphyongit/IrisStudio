#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What the launcher (launcher/) works out apart from Windows and the game, so it can be
// tested: Steam's library files, the fingerprint of the game's zones that the character zones and the animation pack
// were built from, and how the launcher words sizes and counts. The mod doesn't build this file; the launcher and the
// unit tests do.
namespace IWXMVM::Setup
{
    // Valve's KeyValues text, as libraryfolders.vdf and appmanifest_*.acf have it: "key" "value" pairs and "key" {
    // ... } blocks, with \\ \" \n \t escapes in quoted strings and // comments
    struct KeyValues
    {
        std::string value;
        std::vector<std::pair<std::string, KeyValues>> children;
        bool block = false;

        // the first child of a key, ignoring case; none if there is none
        const KeyValues* Find(std::string_view key) const;
    };
    // the text's root block (its keys at the top level); none for text that isn't KeyValues
    std::optional<KeyValues> ParseKeyValues(std::string_view text);

    inline constexpr std::string_view BLACK_OPS_APP_ID = "42700";
    inline constexpr std::string_view BLACK_OPS_INSTALL_DIR = "Call of Duty Black Ops";

    struct SteamLibrary
    {
        std::filesystem::path path;  // the library's folder, its steamapps folder in it
        bool hasApp = false;         // whether its "apps" lists the app (the newer format only)
    };
    // The libraries of steamapps\libraryfolders.vdf, those that list the app first. Takes the format of 2021 on
    // ("0" { "path" "..." "apps" { ... } }) and the older one ("1" "D:\\SteamLibrary").
    std::vector<SteamLibrary> SteamLibraries(std::string_view libraryFolders, std::string_view appId);
    // the folder under steamapps\common an appmanifest_<id>.acf names
    std::optional<std::string> InstallDir(std::string_view appManifest);

    // A zone file of the game: its path under zone\ (Common\cuba.ff, English\en_cuba.ff) and its size
    struct ZoneFile
    {
        std::string path;
        std::uint64_t size = 0;
    };
    // The game's zones in 16 hex digits, the same whatever their order and the case of their names; a game update
    // changes it, so the zones and pack built from the old files are built again. Sizes only: copying a game folder
    // keeps them, and an update that changes a zone changes its size.
    std::string Fingerprint(std::vector<ZoneFile> files);

    // 16 hex digits of a file's bytes (the embedded catalogs, so a setup knows which catalog it was built for)
    std::string Hash(std::span<const std::uint8_t> bytes);

    // A character zone of characters.json (version 2): its name, the level it is built from and its models, as
    // tools/t5-characters/zones.py takes them (character_models: the parts, the arrays they are picked from, the
    // wounded bodies and the limbs wounds throw off, each once, in that order)
    struct CharacterZone
    {
        std::string name;
        std::string level;
        std::vector<std::string> models;
        std::vector<std::pair<std::string, std::string>> assets;  // of other types (a kit's), as type and name
        // the levels it can be linked from instead, after `level` (a weapon's: see ChooseLevel)
        std::vector<std::string> otherLevels;
    };
    // the zones of the characters that have one, in the catalog's order; throws std::runtime_error for a catalog that
    // isn't one
    std::vector<CharacterZone> ReadCharacterZones(std::string_view charactersJson);
    // The campaign maps' kits of campaign-kits.json (version 1, tools/t5-maps/worlds.py),
    // built like character zones: each out of the first multiplayer map of its teamset (its level), with its models
    // and its scripts and effects; throws std::runtime_error for a file that isn't one
    std::vector<CharacterZone> ReadKitZones(std::string_view kitsJson);
    // The zones of the campaign's and Zombies' weapons and of the sound banks their shots' sounds are in
    // (campaign-weapons.json's "zones", version 1, tools/t5-weapons/weapons.py), built like
    // the kits: a weapon zone's assets are its weapons (with their upgraded, left-hand and alternate variants), a sound
    // zone's a level's bank; each can be linked from any of its sources, the first the game has (ChooseLevel). Throws
    // std::runtime_error for a file that isn't one.
    std::vector<CharacterZone> ReadWeaponZones(std::string_view weaponsJson);
    // The weapons' textures that only single player's Rezurrection files have (main/iw_36.iwd to iw_41.iwd, which
    // multiplayer skips): campaign-weapons.json's "textures", which the setup copies into one file of Plutonium's
    // storage\t5\main, searched before the game's main (none listed for a catalog without them)
    struct WeaponTextures
    {
        std::string file;  // iw_iris_weapons.iwd
        std::vector<std::string> images;
    };
    WeaponTextures ReadWeaponTextures(std::string_view weaponsJson);
    // Whether the end of that file (a zip lists what it holds there) names every image: a newer catalog can ask for more
    bool NamesAllImages(std::string_view archiveEnd, const std::vector<std::string>& images);
    inline constexpr std::array<std::string_view, 6> REZURRECTION_IWDS{"iw_36.iwd", "iw_37.iwd", "iw_38.iwd",
                                                                       "iw_39.iwd", "iw_40.iwd", "iw_41.iwd"};
    // The pictures of the game's Change Map menu for campaign levels and Zombies maps (campaign-kits.json's
    // "pictures"): images of the game's single player zones (zone\Common\<zone>.ff), which
    // the setup copies with OpenAssetTools' Unlinker as <image>.dds into the mod's pictures folder
    struct MenuPicture
    {
        std::string zone;
        std::string image;
    };
    std::vector<MenuPicture> ReadMenuPictures(std::string_view kitsJson);
    inline constexpr std::string_view MENU_PICTURES_FOLDER = "pictures";

    // An animation of animations.json (version 1) as the pack has it: its key, its hash and the zones it is in
    struct CatalogAnimation
    {
        std::string key;
        std::array<std::uint8_t, 8> hash{};
        std::vector<std::uint16_t> zones;  // into AnimationCatalog::zones
    };
    struct AnimationCatalog
    {
        std::vector<std::string> zones;  // the zones the catalog's animations come from, in their order
        std::vector<CatalogAnimation> animations;
    };
    // throws std::runtime_error for a catalog that isn't one
    AnimationCatalog ReadAnimationCatalog(std::string_view animationsJson);

    // The zone files of a game folder: the names in zone\Common and in each language's folder (zone\English), as they
    // are on disk
    struct GameZones
    {
        std::vector<std::string> common;
        std::vector<std::pair<std::string, std::vector<std::string>>> languages;  // the folder and its files

        // whether zone\Common has a zone (by its name without .ff, any case)
        bool HasCommon(std::string_view name) const;
    };
    // The zones the Linker loads for a level's character zones, in the order the game loads them (zones.py's
    // level_loads): patch and common (Zombies: patch, common_zombie, common_zombie_patch and the map's patch zone; a
    // multiplayer map, a kit's: code_post_gfx_mp, patch_mp and common_mp), then the level, each after its language twin
    // (zone\English\en_cuba.ff). Paths are under the game folder. None when the level's own zone isn't there.
    std::optional<std::vector<std::filesystem::path>> LevelLoads(const GameZones& zones, std::string_view level);
    // the level a zone is linked from in a game: its level, else the first of its other levels the game has; none when
    // it has none of them
    std::optional<std::string> ChooseLevel(const GameZones& zones, const CharacterZone& zone);

    // the Linker's zone definition of a character zone (zones.py's write_definition), and a kit's other assets
    std::string ZoneDefinition(std::string_view name, const std::vector<std::string>& models,
                               const std::vector<std::pair<std::string, std::string>>& assets = {});
    // a command line's argument, quoted as CommandLineToArgvW reads it back
    std::wstring QuoteArgument(std::wstring_view argument);

    // The latest release on GitHub (api.github.com/repos/<owner>/<repo>/releases/latest): its tag and page. None for
    // an answer that isn't one, or a draft or a prerelease.
    struct Release
    {
        std::string tag;
        std::string url;
    };
    std::optional<Release> ReadLatestRelease(std::string_view json);
    // whether a version ("v0.2.0", "0.1.10") is newer than another, by its numbers; false when either isn't one
    bool IsNewerVersion(std::string_view version, std::string_view than);

    // What a setup does, for its summary: what it downloads and how much it builds
    struct SetupNeeds
    {
        bool openAssetTools = false;
        bool ffmpeg = false;
        std::size_t zones = 0;       // character zones to build
        std::size_t animations = 0;  // the pack's animations, when it is written (0 when it isn't)
    };
    // The summary's numbers, from the sizes measured on the user's game files: the
    // downloads (their zips), what is built, the space all of it needs at most (the downloads unpacked, what is built,
    // and the pack twice while it is put together) and about how long it takes
    struct SetupEstimate
    {
        std::uint64_t downloadBytes = 0;
        std::uint64_t buildBytes = 0;
        std::uint64_t spaceBytes = 0;
        double seconds = 0.0;
    };
    SetupEstimate Estimate(const SetupNeeds& needs);
    // "Takes about 3 minutes." (to the nearest minute, a minute at least)
    std::string Duration(double seconds);

    // The zone files of a game that the character zones of some levels are built from: what the Linker loads for them
    // (LevelLoads), as the game's list has them; the others (multiplayer's, say) don't make the zones out of date
    std::vector<ZoneFile> FilesBuiltFrom(const GameZones& zones, const std::vector<std::string>& levels,
                                         const std::vector<ZoneFile>& files);

    // Whether zones built from some of the game's zone files are out of date in a game folder: a file they were built
    // from has another size there (a game update). Files of either side that the other hasn't don't count (a copy
    // with more map packs, or fewer), and neither does the folder they are in, so a copy of the same game doesn't
    // build them again.
    bool ZonesChanged(const std::vector<ZoneFile>& builtFrom, const std::vector<ZoneFile>& now);
    // Why character zones built before are out of date: the game's zone files they were built from changed
    // (ZonesChanged; a setup.json of an older launcher has the fingerprint of all of them only), or only the character
    // catalog did (a mod with new characters)
    enum class ZonesStale
    {
        No,
        GameUpdated,
        NewCharacters,
    };
    ZonesStale GetZonesStale(const std::vector<ZoneFile>& builtFrom, std::string_view builtFingerprint,
                             const std::vector<ZoneFile>& now, std::string_view fingerprint,
                             std::string_view builtCatalog, std::string_view catalog);

    // The palette in the mod's config.json (preferences.palette); empty when it has none
    std::string ModPalette(std::string_view configJson);
    // config.json with preferences.palette set, the rest as it was, written as the mod writes it (4 spaces); none for
    // text that isn't a config
    std::optional<std::string> WithModPalette(std::string_view configJson, std::string_view palette);
    // The game mod the mod's Demo tab last switched to (preferences.gameMod: its name, empty for none);
    // none until it switched once
    std::optional<std::string> ModGameMod(std::string_view configJson);
    // config.json with preferences.gameMod set, the rest as it was; none for text that isn't a config
    std::optional<std::string> WithModGameMod(std::string_view configJson, std::string_view gameMod);

    // 7938 as "7,938"
    std::string FormatCount(std::uint64_t count);
    // a size as the launcher shows it: "820 KB", "3.8 MB", "267 MB", "1.2 GB" (powers of 1024, as Explorer)
    std::string FormatSize(std::uint64_t bytes);
    // A name Plutonium can be started with (-name): printable ASCII without spaces or quotes, at most 15 characters;
    // "Player" for one with nothing left
    std::string PlayerName(std::string_view name);
}  // namespace IWXMVM::Setup
