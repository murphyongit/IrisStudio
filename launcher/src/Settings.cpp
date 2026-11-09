#include "Settings.hpp"

#include <fstream>
#include <system_error>

#include "nlohmann/json.hpp"

#include "Platform.hpp"
#include "Setup/SetupLogic.hpp"

namespace Launcher
{
    Settings Settings::Load(const std::filesystem::path& file)
    {
        Settings settings;
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
            return settings;

        const auto json = nlohmann::json::parse(stream, nullptr, false);
        if (!json.is_object())
            return settings;

        auto text = [&](const char* key, auto& value) {
            if (const auto it = json.find(key); it != json.end() && it->is_string())
                value = it->get<std::string>();
        };
        std::string gameFolder;
        text("gameFolder", gameFolder);
        settings.gameFolder = std::filesystem::path(std::u8string(gameFolder.begin(), gameFolder.end()));
        text("palette", settings.palette);
        text("playerName", settings.playerName);
        text("gameMod", settings.gameMod);
        if (const auto it = json.find("checkUpdates"); it != json.end() && it->is_boolean())
            settings.checkUpdates = it->get<bool>();
        return settings;
    }

    std::string ModPalette::Read(const std::filesystem::path& config)
    {
        const auto text = Platform::ReadFile(config);
        return text ? IWXMVM::Setup::ModPalette(*text) : std::string();
    }

    bool ModPalette::Write(const std::filesystem::path& config, const std::string& palette)
    {
        const auto text = Platform::ReadFile(config);
        if (!text)
            return false;
        if (IWXMVM::Setup::ModPalette(*text) == palette)
            return true;
        const auto written = IWXMVM::Setup::WithModPalette(*text, palette);
        return written && Platform::WriteFile(config, *written);
    }

    std::optional<std::string> ModGameMod::Read(const std::filesystem::path& config)
    {
        const auto text = Platform::ReadFile(config);
        return text ? IWXMVM::Setup::ModGameMod(*text) : std::nullopt;
    }

    bool ModGameMod::Write(const std::filesystem::path& config, const std::string& gameMod)
    {
        const auto text = Platform::ReadFile(config);
        if (!text)
            return false;
        if (IWXMVM::Setup::ModGameMod(*text) == gameMod)
            return true;
        const auto written = IWXMVM::Setup::WithModGameMod(*text, gameMod);
        return written && Platform::WriteFile(config, *written);
    }

    bool Settings::Save(const std::filesystem::path& file) const
    {
        const auto gameFolderText = gameFolder.u8string();
        const nlohmann::json json = {
            {"gameFolder", std::string(gameFolderText.begin(), gameFolderText.end())},
            {"palette", palette},
            {"playerName", playerName},
            {"checkUpdates", checkUpdates},
            {"gameMod", gameMod},
        };

        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        // written next to it first, so a crash never leaves half a file
        auto temporary = file;
        temporary += ".part";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
                return false;
            stream << json.dump(2) << '\n';
            if (!stream)
                return false;
        }
        std::filesystem::rename(temporary, file, error);
        return !error;
    }
}  // namespace Launcher
