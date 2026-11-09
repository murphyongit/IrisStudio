#include "StdInclude.hpp"
#include "Test.hpp"

#include "UI/DemoLoadLogic.hpp"

using namespace IWXMVM::UI;
using DemoLoadLogic::Outcome;
using IWXMVM::Types::GameState;

// A demo starting after PLAY: the notice first, the command two frames on, and
// the list waiting until the demo plays or the game is back at the menu without it

namespace
{
    constexpr auto DEMO = "player-3k-ak47-nuketown_249221000.dm_8";

    // a load of the demo whose command went out at a time (two frames after its click at 1000)
    DemoLoadLogic::Load Sent(std::uint64_t at = 1050)
    {
        DemoLoadLogic::Load load{.name = DEMO, .since = 1000};
        DemoLoadLogic::Advance(load, {.now = 1020});
        DemoLoadLogic::Advance(load, {.now = at});
        load.sent = at;
        return load;
    }

    DemoLoadLogic::Seen AtMenu(std::uint64_t now, std::optional<bool> reading)
    {
        return {.now = now, .state = GameState::MainMenu, .reading = reading};
    }
}  // namespace

TEST("a demo's command goes out two frames after PLAY, so its notice is drawn first")
{
    DemoLoadLogic::Load load{.name = DEMO, .since = 1000};
    CHECK(DemoLoadLogic::Advance(load, {.now = 1016}) == Outcome::Waiting);
    CHECK(DemoLoadLogic::Advance(load, {.now = 1033}) == Outcome::Send);
    CHECK(load.frames == DemoLoadLogic::SEND_AFTER);
}

TEST("a load ends as its demo plays, not as another one does")
{
    auto load = Sent();
    // the game reads it at the menu for a few seconds (frozen for part of them, drawing no frames)
    CHECK(DemoLoadLogic::Advance(load, AtMenu(1200, true)) == Outcome::Waiting);
    CHECK(DemoLoadLogic::Advance(load, AtMenu(9000, true)) == Outcome::Waiting);
    CHECK(load.read);
    // another demo playing isn't it
    CHECK(DemoLoadLogic::Advance(
              load, {.now = 9100, .state = GameState::InDemo, .playing = "other.dm_8", .reading = false}) ==
          Outcome::Waiting);
    CHECK(DemoLoadLogic::Advance(load, {.now = 14000, .state = GameState::InDemo, .playing = DEMO, .reading = false}) ==
          Outcome::Played);
}

TEST("a load ends without its demo once the game is back at the menu, never started it, or a minute went by")
{
    // read, then back at the menu for a second
    auto back = Sent();
    DemoLoadLogic::Advance(back, AtMenu(1500, true));
    CHECK(DemoLoadLogic::Advance(back, AtMenu(2500, false)) == Outcome::Waiting);
    CHECK(DemoLoadLogic::Advance(back, AtMenu(3000, true)) == Outcome::Waiting);
    CHECK(DemoLoadLogic::Advance(back, AtMenu(3200, false)) == Outcome::Waiting);
    CHECK(DemoLoadLogic::Advance(back, AtMenu(4200, false)) == Outcome::BackAtMenu);
    CHECK(DemoLoadLogic::Describe(Outcome::BackAtMenu) == "the game went back to the menu");

    // never read: given up 10 s after its command
    auto never = Sent();
    CHECK(DemoLoadLogic::Advance(never, AtMenu(5000, false)) == Outcome::Waiting);
    CHECK(DemoLoadLogic::Advance(never, AtMenu(1050 + DemoLoadLogic::START_WITHIN, false)) == Outcome::NotStarted);
    // (a demo that played, leaving it for this one, hasn't started it)
    auto leaving = Sent();
    CHECK(DemoLoadLogic::Advance(
              leaving, {.now = 1100, .state = GameState::InDemo, .playing = "other.dm_8", .reading = false}) ==
          Outcome::Waiting);

    // a game that can't tell: until it plays, or a minute
    auto unknown = Sent();
    CHECK(DemoLoadLogic::Advance(unknown, AtMenu(40000, std::nullopt)) == Outcome::Waiting);
    CHECK(DemoLoadLogic::Advance(unknown, AtMenu(1000 + DemoLoadLogic::GIVE_UP_AFTER, std::nullopt)) ==
          Outcome::GaveUp);
    CHECK(DemoLoadLogic::Describe(Outcome::Played).empty() && !DemoLoadLogic::Describe(Outcome::GaveUp).empty());
}
