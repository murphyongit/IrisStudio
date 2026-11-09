#include "StdInclude.hpp"
#include "Test.hpp"

#include "UI/ModSwitchLogic.hpp"

using namespace IWXMVM::UI;
using ModSwitchLogic::Outcome;
using IWXMVM::Types::GameState;

// The game's mod switched from the Demo tab with Plutonium's loadmod: a demo
// playing stopped first, loadmod once the notice is drawn, done when the game runs the mod with the editor back

namespace
{
    constexpr auto BOTS = "mods/mp_bots";

    ModSwitchLogic::Seen AtMenu(std::uint64_t now, std::string loaded = "", std::uint32_t initialized = 1)
    {
        return {.now = now, .state = GameState::MainMenu, .loadedFsGame = std::move(loaded), .initialized = initialized};
    }

    // a switch to mp_bots asked for at 1000 at the menu, whose loadmod went out at 1033
    ModSwitchLogic::Switch Sent()
    {
        ModSwitchLogic::Switch modSwitch{.fsGame = BOTS, .since = 1000};
        ModSwitchLogic::Advance(modSwitch, AtMenu(1016));
        ModSwitchLogic::Advance(modSwitch, AtMenu(1033));
        return modSwitch;
    }
}  // namespace

TEST("mod switch: at the menu, loadmod goes out two frames after the click, so its notice is drawn first")
{
    ModSwitchLogic::Switch modSwitch{.fsGame = BOTS, .since = 1000};
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(1016)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(1033)) == Outcome::Send);
    CHECK(modSwitch.sent == 1033u);
    CHECK(modSwitch.initializedAtSend == 1u);
    // once
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(1050)) == Outcome::Waiting);
}

TEST("mod switch: a demo playing is stopped first, and loadmod waits half a second at the menu")
{
    ModSwitchLogic::Switch modSwitch{.fsGame = "", .since = 1000};
    CHECK(ModSwitchLogic::Advance(modSwitch, {.now = 1016, .state = GameState::InDemo}) == Outcome::Stop);
    CHECK(ModSwitchLogic::Advance(modSwitch, {.now = 1033, .state = GameState::InDemo}) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(2000, BOTS)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(2400, BOTS)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(2500, BOTS)) == Outcome::Send);
}

TEST("mod switch: done once the game runs the mod, the editor came back, and a second went by at the menu")
{
    auto modSwitch = Sent();
    // the game reloads (the editor is away, so no frames), then runs it, the editor started again
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(1050, "", 1)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(8000, BOTS, 2)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(8500, BOTS, 2)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(9000, BOTS, 2)) == Outcome::Done);
}

TEST("mod switch: the mod's name alone, or the editor back alone, isn't done")
{
    auto modSwitch = Sent();
    // fs_game set before the files are reloaded
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(5000, BOTS, 1)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(7000, BOTS, 1)) == Outcome::Waiting);
    // the editor back, the old mod still
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(8000, "", 2)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(9500, "", 2)) == Outcome::Waiting);
    // the second at the menu counts from when both hold
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(10000, BOTS, 2)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(10900, BOTS, 2)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(11000, BOTS, 2)) == Outcome::Done);
}

TEST("mod switch: a switch that isn't done within a minute gives up")
{
    auto modSwitch = Sent();
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(60999, "", 2)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(61000, "", 2)) == Outcome::GaveUp);

    // and one that never reached the menu
    ModSwitchLogic::Switch stuck{.fsGame = BOTS, .since = 1000};
    CHECK(ModSwitchLogic::Advance(stuck, {.now = 1016, .state = GameState::InDemo}) == Outcome::Stop);
    CHECK(ModSwitchLogic::Advance(stuck, {.now = 61000, .state = GameState::InDemo}) == Outcome::GaveUp);
}

TEST("mod switch: a game that runs the mod only after a minute (ReShade compiling for the new device) is still done")
{
    auto modSwitch = Sent();
    // (no frames from the reload until ReShade let go)
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(150000, BOTS, 2)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, AtMenu(151000, BOTS, 2)) == Outcome::Done);
}

TEST("mod switch: done only once the game's main thread runs frames again, as it goes on loading after the editor")
{
    auto modSwitch = Sent();
    const auto seen = [](std::uint64_t now, std::uint32_t mainFrames) {
        auto each = AtMenu(now, BOTS, 2);
        each.mainFrames = mainFrames;
        return each;
    };
    CHECK(ModSwitchLogic::Advance(modSwitch, seen(8000, 500)) == Outcome::Waiting);
    // (14 s of loading: the editor draws, the main thread doesn't)
    CHECK(ModSwitchLogic::Advance(modSwitch, seen(9000, 500)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, seen(22000, 529)) == Outcome::Waiting);
    CHECK(ModSwitchLogic::Advance(modSwitch, seen(22100, 530)) == Outcome::Done);
}
