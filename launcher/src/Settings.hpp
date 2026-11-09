#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Launcher
{
    // The launcher's own settings, in launcher.json of its folder (%LOCALAPPDATA%\IrisStudio): never Plutonium's
    // config.json, which holds the user's token
    struct Settings
    {
        std::filesystem::path gameFolder;  // the Black Ops folder picked or found; empty until one is
        std::string palette = "mono";  // an id of core's UI::Palettes
        std::string playerName = "Player";  // the name Plutonium is started with in LAN
        bool checkUpdates = true;  // whether to ask GitHub for the latest release at the start
        std::string gameMod;  // the game mod the game is started with (its folder's name in mods); empty for none

        // the settings of a file; the defaults for what it lacks or doesn't have right
        static Settings Load(const std::filesystem::path& file);
        bool Save(const std::filesystem::path& file) const;
    };

    // The palette the mod keeps in its config.json (preferences.palette), so both show the same: the mod's, when it
    // has one, and the launcher's pick written into it (the rest of the file as the mod wrote it). Nothing is written
    // when there is no config.json yet: the mod's default is the launcher's.
    struct ModPalette
    {
        static std::string Read(const std::filesystem::path& config);
        static bool Write(const std::filesystem::path& config, const std::string& palette);
    };

    // The game mod the same way (preferences.gameMod): the mod's last switch from its
    // Demo tab, when it made one, and the launcher's pick written into it
    struct ModGameMod
    {
        static std::optional<std::string> Read(const std::filesystem::path& config);
        static bool Write(const std::filesystem::path& config, const std::string& gameMod);
    };
}  // namespace Launcher
