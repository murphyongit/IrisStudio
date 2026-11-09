#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/DemoModLogic.hpp"

#include <process.h>

using namespace IWXMVM::Components;

// Demos recorded with a game mod loaded: the game plays one only while its fs_game is
// loaded, so the mod refuses the others itself, in words

namespace
{
    // the system info of demo0012.dm_8, recorded on Plutonium with mp_bots
    constexpr std::string_view SYSTEM_INFO =
        "\\bg_fixFramerateDependentPhysics\\0\\bg_leanAmount\\20\\fs_game\\mods/mp_bots\\g_patchRocketJumps\\1"
        "\\sv_cheats\\1\\weapon_load_order\\default";
}  // namespace

TEST("demo mods: a demo's fs_game is read from its system info, empty when it has none")
{
    CHECK(DemoModLogic::GetInfoValue(SYSTEM_INFO, "fs_game") == "mods/mp_bots");
    CHECK(DemoModLogic::GetInfoValue(SYSTEM_INFO, "weapon_load_order") == "default");
    CHECK(DemoModLogic::GetInfoValue(SYSTEM_INFO, "bg_leanAmount") == "20");
    CHECK(DemoModLogic::GetInfoValue(SYSTEM_INFO, "fs_basegame").empty());
    // a key that is another's value isn't a key
    CHECK(DemoModLogic::GetInfoValue("\\a\\fs_game\\b\\c", "fs_game").empty());
    CHECK(DemoModLogic::GetInfoValue("", "fs_game").empty());
    CHECK(DemoModLogic::GetInfoValue("\\fs_game\\", "fs_game").empty());
}

TEST("demo mods: a mod is named without its folder")
{
    CHECK(DemoModLogic::GetModName("mods/mp_editingmod") == "mp_editingmod");
    CHECK(DemoModLogic::GetModName("Mods\\mp_bots") == "mp_bots");
    CHECK(DemoModLogic::GetModName("mp_bots") == "mp_bots");
    CHECK(DemoModLogic::GetModName("").empty());
}

TEST("demo mods: a demo plays only with its own fs_game loaded, as the game compares them")
{
    CHECK(DemoModLogic::Plays("", ""));
    CHECK(DemoModLogic::Plays("mods/mp_bots", "mods/mp_bots"));
    CHECK(!DemoModLogic::Plays("mods/mp_editingmod", ""));
    CHECK(!DemoModLogic::Plays("", "mods/mp_bots"));
    CHECK(!DemoModLogic::Plays("mods/mp_editingmod", "mods/mp_bots"));
    // the game's strcmp
    CHECK(!DemoModLogic::Plays("mods/MP_bots", "mods/mp_bots"));
}

TEST("demo mods: the refusal says which mod the demo needs and which is loaded")
{
    CHECK(DemoModLogic::GetRefusal("mods/mp_bots", "mods/mp_bots").empty());
    CHECK(DemoModLogic::GetRefusal("", "").empty());
    CHECK(DemoModLogic::GetRefusal("mods/mp_editingmod", "") ==
          "It was recorded with the mod mp_editingmod, which isn't loaded. Start the game with that mod to play it.");
    CHECK(DemoModLogic::GetRefusal("", "mods/mp_bots") ==
          "It was recorded without a mod, and the mod mp_bots is loaded. Start the game without a mod to play it.");
    CHECK(DemoModLogic::GetRefusal("mods/mp_editingmod", "mods/mp_bots") ==
          "It was recorded with the mod mp_editingmod, and the mod mp_bots is loaded. Start the game with "
          "mp_editingmod to play it.");
}

TEST("demo mods: a mod's fs_game is its name in the mods folder")
{
    CHECK(DemoModLogic::GetFsGame("mp_bots") == "mods/mp_bots");
    CHECK(DemoModLogic::GetFsGame("").empty());
    CHECK(DemoModLogic::GetModName(DemoModLogic::GetFsGame("mp_editingmod")) == "mp_editingmod");
}

namespace
{
    // a folder of its own under %TEMP%, gone again at the end of the test
    struct ModsFolder
    {
        std::filesystem::path path;

        explicit ModsFolder(std::string_view name)
            : path(std::filesystem::temp_directory_path() /
                   std::format("iris-mods-test-{}-{}", _getpid(), name))
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            std::filesystem::create_directories(path, error);
        }

        ~ModsFolder()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        void Add(const std::filesystem::path& file) const
        {
            std::filesystem::create_directories((path / file).parent_path());
            std::ofstream(path / file, std::ios::binary) << "x";
        }
    };
}  // namespace

TEST("demo mods: the mods are the folders of both mods folders with a mod.ff or an .iwd, once each, by name")
{
    // a real one: storage\t5\mods has mp_editingmod's files and a folder the game wrote mp_bots' logs into, the game
    // folder's mods has mp_bots
    const ModsFolder storage("storage"), game("game");
    storage.Add("mp_editingmod/mod.ff");
    storage.Add("mp_editingmod/console_mp.log");
    storage.Add("mp_bots/console_mp.log");
    storage.Add("mp_bots/demos/demos.dm_8");
    storage.Add("Zeta/zeta.IWD");
    game.Add("mp_bots/mod.ff");
    game.Add("mp_bots/mp_bots.iwd");
    game.Add("mp_bots/description.txt");
    game.Add("MP_EDITINGMOD/mod.ff");
    game.Add("empty/readme.txt");
    std::filesystem::create_directories(game.path / "nothing");

    const auto mods = DemoModLogic::FindMods({storage.path, game.path, storage.path / "missing"});
    CHECK(mods.size() == 3);
    if (mods.size() == 3)
    {
        CHECK(mods[0].name == "mp_bots");
        CHECK(mods[0].folder == game.path);
        CHECK(mods[1].name == "mp_editingmod");
        CHECK(mods[1].folder == storage.path);
        CHECK(mods[2].name == "Zeta");
    }
    CHECK(DemoModLogic::HasMod(mods, "MP_Bots"));
    CHECK(!DemoModLogic::HasMod(mods, "empty"));
    CHECK(!DemoModLogic::HasMod(mods, ""));
    CHECK(DemoModLogic::FindMods({}).empty());
}

TEST("demo mods: the notice says what a demo needs and what is loaded, in short words")
{
    CHECK(DemoModLogic::GetNeedsTitle("demo0012", "mods/mp_bots") == "demo0012 needs mp_bots");
    CHECK(DemoModLogic::GetNeedsTitle("nuketown", "") == "nuketown needs no mod");
    CHECK(DemoModLogic::GetNeedsLine("mods/mp_bots", "", true) == "No mod is loaded.");
    CHECK(DemoModLogic::GetNeedsLine("", "mods/mp_bots", true) == "mp_bots is loaded.");
    CHECK(DemoModLogic::GetNeedsLine("mods/mp_editingmod", "mods/mp_bots", true) == "mp_bots is loaded.");
    CHECK(DemoModLogic::GetNeedsLine("mods/mp_x", "", false) == "mp_x isn't in your mods folders.");
    // (no mod needed is always there)
    CHECK(DemoModLogic::GetNeedsLine("", "mods/mp_bots", false) == "mp_bots is loaded.");
}
