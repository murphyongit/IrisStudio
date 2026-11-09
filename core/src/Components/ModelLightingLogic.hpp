#pragma once

namespace IWXMVM::Components::ModelLightingLogic
{
    // The game lights each model it draws from a slot of a cache of 1024, which is plenty on
    // a multiplayer map. A campaign level has up to 1948 dynamic models, nearly all in the list of a frame in some
    // places. Apart from the game so the tests in tests/actors-tests build it on their own.

    // what the scene's newcomers (an actor added, a player moving) keep free
    inline constexpr std::uint32_t RESERVED_SLOTS = 64;
    inline constexpr std::uint32_t LEAST_DYNAMIC_MODEL_SLOTS = 256;

    // How many dynamic models of a frame take a slot of their own: the game lights them last, so taken up to the end
    // they leave no slot for a DObj that moves, which then isn't drawn
    std::uint32_t GetDynamicModelSlots(std::uint32_t slots, std::uint32_t sceneDObjs, std::uint32_t sceneModels);

    // A slot of the cache as this reads it
    struct Slot
    {
        float origin[3];
        std::uint8_t primaryLight;
    };

    // the primary light of a slot the lighting job is still working on
    inline constexpr std::uint8_t SLOT_BEING_LIT = 0xFF;

    // The slot lit nearest to an origin, for a model that gets none of its own; -1 when none is lit
    std::int32_t FindNearestSlot(std::span<const Slot> slots, const float* origin);
}  // namespace IWXMVM::Components::ModelLightingLogic
