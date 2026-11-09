#pragma once

#include "Types/Game.hpp"

namespace IWXMVM
{
    constexpr const char* IWXMVM_VERSION = "1.0";

    // The mod's name in each game, as the menu bar, About and the message boxes have it: the game's codename, then
    // "iris" (2026-10-28). The launcher that sets them up is Iris Studio.
    constexpr const char* ModName(Types::Game game)
    {
        switch (game)
        {
            case Types::Game::IW3:
                return "IW3iris";
            case Types::Game::IW5:
                return "IW5iris";
            case Types::Game::T5:
                return "T5iris";
            default:
                return "Iris";
        }
    }
}  // namespace IWXMVM
