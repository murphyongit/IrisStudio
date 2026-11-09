#pragma once

#include "Types/GameState.hpp"

// The game's mod switched from the Demo tab, apart from ImGui so it can be
// tested. Plutonium's loadmod at the menu reloads the game's files and its renderer for some seconds, the editor going
// away and coming back with it, so a switch stops a demo playing first, sends loadmod once its notice has been drawn,
// and is done when the game runs the mod, the editor came back, and the game has been at the menu for a moment. A demo
// to play after it then plays as PLAY plays it.
namespace IWXMVM::UI::ModSwitchLogic
{
    // frames drawn from the click to loadmod, so the notice is on screen before the window freezes
    inline constexpr std::uint32_t SEND_AFTER = 2;
    // milliseconds at the menu, after a demo was stopped, before loadmod goes out
    inline constexpr std::uint64_t MENU_FOR = 500;
    // milliseconds the game runs the mod at the menu, the editor back, before the switch is done, and the frames its
    // main thread runs meanwhile: it goes on loading for some seconds once the editor is back (14 s measured), and
    // takes no demo command until it's done
    inline constexpr std::uint64_t SETTLE_FOR = 1000;
    inline constexpr std::uint32_t SETTLE_MAIN_FRAMES = 30;
    // milliseconds from the click until a switch the game doesn't run yet counts as failed
    inline constexpr std::uint64_t GIVE_UP_AFTER = 60000;

    struct Switch
    {
        std::string fsGame;                     // the mod's fs_game ("mods/mp_bots"), empty to unload it
        std::uint64_t since = 0;                // milliseconds, when it was asked for
        std::uint32_t frames = 0;               // drawn since
        bool stopping = false;                  // a demo or game was being played, and the menu was asked for
        std::optional<std::uint64_t> atMenu;    // since when the game is at the menu, before loadmod
        std::optional<std::uint64_t> sent;      // when loadmod went out
        std::uint32_t initializedAtSend = 0;    // the editor's starts then
        std::optional<std::uint64_t> settled;   // since when the game runs the mod at the menu, the editor back
        std::uint32_t mainFramesAtSettled = 0;   // the main thread's frames then
    };

    // what the game does in a frame
    struct Seen
    {
        std::uint64_t now = 0;
        Types::GameState state = Types::GameState::MainMenu;
        std::string loadedFsGame;
        std::uint32_t initialized = 0;  // how many times the editor's UI has started (once more after a reload)
        std::optional<std::uint32_t> mainFrames;  // the game's main thread's frames, when the game can tell
    };

    enum class Outcome
    {
        Waiting,
        Stop,  // the demo or game playing is left for the menu now
        Send,  // loadmod goes out now
        Done,
        GaveUp,
    };
    // a frame of a switch: what it comes to, the frame counted (Send records itself as sent)
    Outcome Advance(Switch& modSwitch, const Seen& seen);
}  // namespace IWXMVM::UI::ModSwitchLogic
