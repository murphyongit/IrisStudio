#pragma once

namespace IWXMVM::Components::DemoModLogic
{
    // Demos recorded with a game mod loaded (mp_bots, mp_editingmod) and the mod the game runs,
    // apart from the game so the tests in tests/actors-tests build it on their own. A demo names its mod in the
    // gamestate's system info ("\fs_game\mods/mp_bots"), and the game only plays it while that same fs_game is loaded:
    // a demo of another mod or of none is refused with an error that, at least without a mod loaded, ends the game
    // once its dialog is closed. So the mod checks first and refuses such a demo itself.

    // The value of a key in an info string ("\key1\value1\key2\value2..."), empty when it has none
    std::string_view GetInfoValue(std::string_view info, std::string_view key);

    // A mod as people name it, its fs_game without the mods folder: "mods/mp_bots" is "mp_bots"; empty for none
    std::string_view GetModName(std::string_view fsGame);

    // Whether the game plays a demo with that fs_game (empty for none) while the loaded one is loadedFsGame: only
    // when both are the same, compared exactly as the game compares them
    bool Plays(std::string_view demoFsGame, std::string_view loadedFsGame);

    // Why it doesn't, in words for the Demo tab's notice and the console; empty when it plays. Demos recorded on Steam
    // have no fs_game, and are refused with a mod loaded too.
    std::string GetRefusal(std::string_view demoFsGame, std::string_view loadedFsGame);

    // A mod's fs_game from its name, as the game and Plutonium's loadmod take it: "mp_bots" is "mods/mp_bots"; empty
    // for none
    std::string GetFsGame(std::string_view modName);

    // The mods of the game's mods folders: Plutonium's storage\t5\mods and the game
    // folder's mods, which the game's file system both reads. A folder is a mod when it has a mod.ff or an .iwd: the
    // game writes the logs and demos of every mod it ran into a folder of storage\t5\mods too.
    struct GameMod
    {
        std::string name;             // "mp_bots"
        std::filesystem::path folder;  // the mods folder it is in
    };
    // one per name whatever its case (the first folder's), sorted by name; a folder that isn't there has none
    std::vector<GameMod> FindMods(const std::vector<std::filesystem::path>& modsFolders);
    // whether a mod of that name is in the list, in any case
    bool HasMod(const std::vector<GameMod>& mods, std::string_view name);

    // The Demo tab's notice for a demo the game won't play with the mod it runs, in short words: its title
    // ("demo0012 needs mp_bots", "needs no mod") and its line ("No mod is loaded.", "mp_bots is loaded.", or when the
    // demo's mod isn't one of the user's, "mp_x isn't in your mods folders.")
    std::string GetNeedsTitle(std::string_view demoName, std::string_view demoFsGame);
    std::string GetNeedsLine(std::string_view demoFsGame, std::string_view loadedFsGame, bool modFound);
}  // namespace IWXMVM::Components::DemoModLogic
