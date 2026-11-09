#pragma once

// What of the keyboard and mouse reaches the game's own input besides the editor, apart from Windows so it can be
// tested (UIManager's window procedure asks about each message). While the editor is shown it has the keyboard and the
// mouse: a key it doesn't use would still run the game's bind for it (T opened the chat, the wheel switched weapons,
// Escape left the demo). Escape never leaves a demo, the editor shown or not: STOP in the Demo tab and the console's
// disconnect do. Releases always reach the game, so nothing pressed while it had them stays down there.
namespace IWXMVM::UI::InputRouting
{
    struct State
    {
        bool editorShown = false;    // the overlay is up (F1 hides it)
        bool gameTakesKeys = false;  // the game's console or one of its menus has the keyboard, which runs no binds
        bool inDemo = false;         // the game plays a demo
        bool menuTakesKeys = false;  // of those, one of its menus (or the chat field), not the console
    };

    // The keys routed apart from the others
    enum class Key
    {
        Other,
        Console,  // the one that opens the game's console
        Escape,   // ends a demo the game plays with none of its menus open, also with the console open (CL_KeyEvent)
    };

    // Whether a key press reaches the game: never one the editor uses (a command's key, or any while one of its text
    // fields, popups or controls has the keyboard), nor Escape in a demo unless one of the game's menus has the keys
    // (it closes that); else always with the editor hidden, and while it is shown, when the game takes keys itself or
    // for the key that opens its console
    bool KeyPressReachesGame(const State& state, bool usedByEditor, Key key);

    // Whether a typed character reaches the game: none typed into the editor's text fields, else as key presses do
    bool CharacterReachesGame(const State& state, bool typingInEditor);

    // Whether a press of a mouse button or the wheel reaches the game: only with the editor hidden
    bool MouseReachesGame(const State& state);

    // The mouse buttons down as the game is told of them (a bit each), from those it was told of, those down before and
    // those down now: one pressed while the game has the mouse is down for it until let go, one pressed while the
    // editor had it never is
    std::uint32_t GameButtons(std::uint32_t told, std::uint32_t before, std::uint32_t now, bool gameHasMouse);
}  // namespace IWXMVM::UI::InputRouting
