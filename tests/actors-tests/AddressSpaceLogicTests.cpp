#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/AddressSpaceLogic.hpp"

using namespace IWXMVM::Components;

// The game's address space and when it counts as short

namespace
{
    constexpr std::uint64_t MB = 1ull << 20;
}

TEST("address space: short below 512 MB free or a largest block below 256 MB, not at a fresh game's")
{
    // a fresh game, and the end of the 52-scenario batch
    CHECK(!AddressSpaceLogic::IsShort({1524 * MB, 1452 * MB}));
    CHECK(AddressSpaceLogic::IsShort({425 * MB, 325 * MB}));
    // the floors themselves are not short
    CHECK(!AddressSpaceLogic::IsShort({512 * MB, 256 * MB}));
    CHECK(AddressSpaceLogic::IsShort({511 * MB, 400 * MB}));
    // plenty free in pieces: what a load or a heap's growth needs in one piece is gone
    CHECK(AddressSpaceLogic::IsShort({900 * MB, 255 * MB}));
    // another floor (the harness's)
    CHECK(AddressSpaceLogic::IsShort({1524 * MB, 1452 * MB}, 4000 * MB));
    CHECK(!AddressSpaceLogic::IsShort({425 * MB, 325 * MB}, 128 * MB));
}

TEST("address space: described in whole MB")
{
    CHECK(AddressSpaceLogic::Describe({425 * MB + 7, 325 * MB}) == "425 MB free, the largest block 325 MB");
}
