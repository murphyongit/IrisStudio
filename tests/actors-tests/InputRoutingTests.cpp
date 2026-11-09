#include "StdInclude.hpp"
#include "Test.hpp"

#include "UI/InputRouting.hpp"

using namespace IWXMVM::UI::InputRouting;

namespace
{
    constexpr State SHOWN = {true, false};
    constexpr State HIDDEN = {false, false};
    // the game's console or a menu (the main menu) has the keyboard
    constexpr State SHOWN_GAME_UI = {true, true};
    // a demo playing
    constexpr State SHOWN_DEMO = {true, false, true};
    constexpr State HIDDEN_DEMO = {false, false, true};
    constexpr State HIDDEN_DEMO_CONSOLE = {false, true, true};
    constexpr State SHOWN_DEMO_CONSOLE = {true, true, true};
    constexpr State HIDDEN_DEMO_MENU = {false, true, true, true};

    constexpr Key OTHER = Key::Other;
    constexpr Key CONSOLE = Key::Console;
    constexpr Key ESCAPE = Key::Escape;

    constexpr std::uint32_t LEFT = 1;
    constexpr std::uint32_t RIGHT = 2;
}  // namespace

TEST("while the editor is shown, keys it doesn't use don't reach the game's binds")
{
    // T opened the chat, Escape left the demo
    CHECK(!KeyPressReachesGame(SHOWN, false, OTHER));
    CHECK(!KeyPressReachesGame(SHOWN, true, OTHER));
    // the console still opens
    CHECK(KeyPressReachesGame(SHOWN, false, CONSOLE));
}

TEST("with the editor hidden the game gets every key but the editor's")
{
    CHECK(KeyPressReachesGame(HIDDEN, false, OTHER));
    CHECK(KeyPressReachesGame(HIDDEN, false, CONSOLE));
    // F1 shows the editor again, Escape stops a recording
    CHECK(!KeyPressReachesGame(HIDDEN, true, ESCAPE));
}

TEST("the game's console and menus get keys while the editor is shown, but not the editor's")
{
    CHECK(KeyPressReachesGame(SHOWN_GAME_UI, false, OTHER));
    CHECK(!KeyPressReachesGame(SHOWN_GAME_UI, true, OTHER));
    // typing ~ into a text field of the editor doesn't close the console
    CHECK(!KeyPressReachesGame(SHOWN_GAME_UI, true, CONSOLE));
}

TEST("Escape never ends a demo, the editor shown or hidden, the console open or not; a menu of the game's gets it")
{
    // the game ends a demo at an Escape with none of its menus open, also with its console open (which Escape doesn't
    // close): STOP and the console's disconnect do that
    CHECK(!KeyPressReachesGame(HIDDEN_DEMO, false, ESCAPE));
    CHECK(!KeyPressReachesGame(SHOWN_DEMO, false, ESCAPE));
    CHECK(!KeyPressReachesGame(HIDDEN_DEMO_CONSOLE, false, ESCAPE));
    CHECK(!KeyPressReachesGame(SHOWN_DEMO_CONSOLE, false, ESCAPE));
    CHECK(KeyPressReachesGame(HIDDEN_DEMO_MENU, false, ESCAPE));
    // other keys as before, and Escape in the menus out of a demo
    CHECK(KeyPressReachesGame(HIDDEN_DEMO, false, OTHER));
    CHECK(KeyPressReachesGame(HIDDEN_DEMO, false, CONSOLE));
    CHECK(KeyPressReachesGame(HIDDEN, false, ESCAPE));
    CHECK(KeyPressReachesGame(SHOWN_GAME_UI, false, ESCAPE));
    // a recording still stops at it (the editor's command)
    CHECK(!KeyPressReachesGame(HIDDEN_DEMO, true, ESCAPE));
}

TEST("typed characters go where the keys go, never into the game from the editor's text fields")
{
    CHECK(!CharacterReachesGame(SHOWN, false));
    CHECK(CharacterReachesGame(SHOWN_GAME_UI, false));
    CHECK(!CharacterReachesGame(SHOWN_GAME_UI, true));
    CHECK(CharacterReachesGame(HIDDEN, false));
}

TEST("mouse buttons and the wheel reach the game only with the editor hidden")
{
    CHECK(!MouseReachesGame(SHOWN));
    CHECK(!MouseReachesGame(SHOWN_GAME_UI));
    CHECK(MouseReachesGame(HIDDEN));
}

TEST("a button pressed over the editor is never down for the game")
{
    // pressed and let go while the editor has the mouse
    auto told = GameButtons(0, 0, LEFT, false);
    CHECK(told == 0);
    told = GameButtons(told, LEFT, 0, false);
    CHECK(told == 0);
    // still held when F1 hands the mouse to the game: not a press there
    told = GameButtons(0, 0, RIGHT, false);
    told = GameButtons(told, RIGHT, RIGHT, true);
    CHECK(told == 0);
    told = GameButtons(told, RIGHT, 0, true);
    CHECK(told == 0);
}

TEST("a button pressed while the game has the mouse stays down for it until let go, whoever has the mouse then")
{
    auto told = GameButtons(0, 0, LEFT, true);
    CHECK(told == LEFT);
    // F1 shows the editor while it is held; another button pressed now isn't the game's
    told = GameButtons(told, LEFT, LEFT | RIGHT, false);
    CHECK(told == LEFT);
    // let go: the game is told
    told = GameButtons(told, LEFT | RIGHT, RIGHT, false);
    CHECK(told == 0);
}
