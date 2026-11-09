#include "StdInclude.hpp"
#include "DemoModLogic.hpp"

#include <cctype>

namespace IWXMVM::Components::DemoModLogic
{
    std::string_view GetInfoValue(std::string_view info, std::string_view key)
    {
        while (info.starts_with('\\'))
        {
            info.remove_prefix(1);
            const auto keyEnd = info.find('\\');
            if (keyEnd == std::string_view::npos)
                break;

            const auto currentKey = info.substr(0, keyEnd);
            info.remove_prefix(keyEnd + 1);

            const auto value = info.substr(0, info.find('\\'));
            if (currentKey == key)
                return value;

            info.remove_prefix(value.size());
        }

        return {};
    }

    std::string_view GetModName(std::string_view fsGame)
    {
        for (const std::string_view folder : {"mods/", "mods\\"})
        {
            if (fsGame.size() > folder.size() &&
                std::equal(folder.begin(), folder.end(), fsGame.begin(),
                           [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
                return fsGame.substr(folder.size());
        }
        return fsGame;
    }

    bool Plays(std::string_view demoFsGame, std::string_view loadedFsGame)
    {
        // the game's check (0x56F440) compares them with strcmp
        return demoFsGame == loadedFsGame;
    }

    std::string GetRefusal(std::string_view demoFsGame, std::string_view loadedFsGame)
    {
        if (Plays(demoFsGame, loadedFsGame))
            return {};

        const auto demoMod = GetModName(demoFsGame);
        const auto loadedMod = GetModName(loadedFsGame);
        if (demoFsGame.empty())
            return std::format(
                "It was recorded without a mod, and the mod {} is loaded. Start the game without a mod "
                "to play it.",
                loadedMod);
        if (loadedFsGame.empty())
            return std::format(
                "It was recorded with the mod {}, which isn't loaded. Start the game with that mod to "
                "play it.",
                demoMod);
        return std::format(
            "It was recorded with the mod {}, and the mod {} is loaded. Start the game with {} to play "
            "it.",
            demoMod, loadedMod, demoMod);
    }

    std::string GetFsGame(std::string_view modName)
    {
        return modName.empty() ? std::string() : std::format("mods/{}", modName);
    }

    namespace
    {
        std::string Lower(std::string_view text)
        {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return lower;
        }

        bool IsModFolder(const std::filesystem::path& folder)
        {
            std::error_code error;
            for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
            {
                if (!it->is_regular_file(error))
                    continue;
                const auto name = Lower(it->path().filename().string());
                if (name == "mod.ff" || name.ends_with(".iwd"))
                    return true;
            }
            return false;
        }
    }  // namespace

    std::vector<GameMod> FindMods(const std::vector<std::filesystem::path>& modsFolders)
    {
        std::vector<GameMod> mods;
        for (const auto& modsFolder : modsFolders)
        {
            std::error_code error;
            for (std::filesystem::directory_iterator it(modsFolder, error), end; !error && it != end;
                 it.increment(error))
            {
                if (!it->is_directory(error))
                    continue;
                auto name = it->path().filename().string();
                if (HasMod(mods, name) || !IsModFolder(it->path()))
                    continue;
                mods.push_back({std::move(name), modsFolder});
            }
        }
        std::sort(mods.begin(), mods.end(), [](const GameMod& a, const GameMod& b) {
            return Lower(a.name) < Lower(b.name);
        });
        return mods;
    }

    bool HasMod(const std::vector<GameMod>& mods, std::string_view name)
    {
        const auto lower = Lower(name);
        return std::any_of(mods.begin(), mods.end(), [&](const GameMod& mod) { return Lower(mod.name) == lower; });
    }

    std::string GetNeedsTitle(std::string_view demoName, std::string_view demoFsGame)
    {
        return demoFsGame.empty() ? std::format("{} needs no mod", demoName)
                                  : std::format("{} needs {}", demoName, GetModName(demoFsGame));
    }

    std::string GetNeedsLine(std::string_view demoFsGame, std::string_view loadedFsGame, bool modFound)
    {
        if (!demoFsGame.empty() && !modFound)
            return std::format("{} isn't in your mods folders.", GetModName(demoFsGame));
        return loadedFsGame.empty() ? std::string("No mod is loaded.")
                                    : std::format("{} is loaded.", GetModName(loadedFsGame));
    }
}  // namespace IWXMVM::Components::DemoModLogic
