#pragma once

#include "Types/GameState.hpp"

// A demo starting after PLAY, apart from ImGui so it can be tested. The game's
// window, the editor's too, freezes for a few seconds as the game starts reading a demo, then shows its loading screen,
// and the game stays at the menu until the demo plays. So PLAY starts a load the Demos list waits on: its notice is
// drawn first, the demo command goes out SEND_AFTER frames after the click, and every PLAY waits until the demo plays,
// the game is back at the menu without it, or a minute has gone.
namespace IWXMVM::UI::DemoLoadLogic
{
    // frames drawn from the click to the demo command, so the notice is on screen before the window freezes
    inline constexpr std::uint32_t SEND_AFTER = 2;
    // milliseconds from the command to the game reading the demo, before it counts as not started
    inline constexpr std::uint64_t START_WITHIN = 10000;
    // milliseconds the game is at the menu without reading it, once it did, before it counts as back at the menu
    inline constexpr std::uint64_t BACK_AT_MENU = 1000;
    // milliseconds from the click until the list stops waiting on it
    inline constexpr std::uint64_t GIVE_UP_AFTER = 60000;

    struct Load
    {
        std::string name;                      // the demo's file name, as the game names the demo it plays
        std::uint64_t since = 0;               // milliseconds, when PLAY was pressed
        std::uint32_t frames = 0;              // drawn since
        std::optional<std::uint64_t> sent;     // when its command went out
        bool read = false;                     // the game was seen reading it
        std::optional<std::uint64_t> stopped;  // since when it isn't, once it was
    };

    // what the game does in a frame
    struct Seen
    {
        std::uint64_t now = 0;
        Types::GameState state = Types::GameState::MainMenu;
        std::string playing;  // the demo it plays (its file name), in a demo
        // whether it reads a demo it was asked to play, before it plays it; none when the game can't tell
        std::optional<bool> reading;
    };

    enum class Outcome
    {
        Waiting,
        Send,  // the demo command goes out now
        Played,
        BackAtMenu,  // the game read it, and is back at the menu without it
        NotStarted,  // the game never read it
        GaveUp,      // a minute went by
    };
    // a frame of a load: what it comes to, the frame counted
    Outcome Advance(Load& load, const Seen& seen);
    // why a load that ended without its demo did, in words; empty for the others
    std::string_view Describe(Outcome outcome);
}  // namespace IWXMVM::UI::DemoLoadLogic
