#include "StdInclude.hpp"
#include "InputRouting.hpp"

namespace IWXMVM::UI::InputRouting
{
    bool KeyPressReachesGame(const State& state, bool usedByEditor, Key key)
    {
        if (usedByEditor || (key == Key::Escape && state.inDemo && !state.menuTakesKeys))
            return false;
        return !state.editorShown || state.gameTakesKeys || key == Key::Console;
    }

    bool CharacterReachesGame(const State& state, bool typingInEditor)
    {
        return !typingInEditor && (!state.editorShown || state.gameTakesKeys);
    }

    bool MouseReachesGame(const State& state)
    {
        return !state.editorShown;
    }

    std::uint32_t GameButtons(std::uint32_t told, std::uint32_t before, std::uint32_t now, bool gameHasMouse)
    {
        const auto pressed = now & ~before;
        return (told & now) | (gameHasMouse ? pressed : 0);
    }
}  // namespace IWXMVM::UI::InputRouting
