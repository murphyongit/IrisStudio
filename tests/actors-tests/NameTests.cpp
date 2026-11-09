#include "StdInclude.hpp"
#include "Test.hpp"

#include "Version.hpp"

using namespace IWXMVM;

// The mod's name in each game (Version.hpp)

TEST("the mod is the game's codename, then iris")
{
    CHECK(std::string_view(ModName(Types::Game::T5)) == "T5iris");
    CHECK(std::string_view(ModName(Types::Game::IW3)) == "IW3iris");
    CHECK(std::string_view(ModName(Types::Game::IW5)) == "IW5iris");
    CHECK(std::string_view(ModName(Types::Game::None)) == "Iris");
}
