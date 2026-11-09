#pragma once

namespace IWXMVM::Components::AddressSpaceLogic
{
    // The game's 32-bit address space. The game's own steps take tens of MB of it
    // at a time and never give them back (the process's heap keeps the size of its peaks): a level's load 25 to 80 MB,
    // a restart of the renderer up to 46. In one long game of maps, characters and captures it fell from 1,524 MB free
    // to 425.
    struct AddressSpace
    {
        std::uint64_t free = 0;     // in all
        std::uint64_t largest = 0;  // the largest free block
    };

    // Below this much free, or a largest block below half of it, the space is short: what the mod keeps that nothing
    // needs (character zones no actor uses) goes.
    inline constexpr std::uint64_t FREE_FLOOR = 512ull << 20;

    bool IsShort(const AddressSpace& space, std::uint64_t freeFloor = FREE_FLOOR);

    // "425 MB free, the largest block 325 MB"
    std::string Describe(const AddressSpace& space);
}  // namespace IWXMVM::Components::AddressSpaceLogic
