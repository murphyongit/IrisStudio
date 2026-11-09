#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ModelLightingLogic.hpp"

using namespace IWXMVM::Components;
using ModelLightingLogic::Slot;

// Models past the lighting cache's 1024 slots: cuba's list reaches 1456 dynamic models

TEST("model lighting: dynamic models leave the scene's DObjs and models their slots and some for newcomers")
{
    CHECK(ModelLightingLogic::GetDynamicModelSlots(1024, 0, 0) == 960);
    CHECK(ModelLightingLogic::GetDynamicModelSlots(1024, 5, 272) == 683);
    // a scene that takes them all still leaves the dynamic models some
    CHECK(ModelLightingLogic::GetDynamicModelSlots(1024, 500, 500) == ModelLightingLogic::LEAST_DYNAMIC_MODEL_SLOTS);
    CHECK(ModelLightingLogic::GetDynamicModelSlots(1024, 704, 0) == ModelLightingLogic::LEAST_DYNAMIC_MODEL_SLOTS);
    CHECK(ModelLightingLogic::GetDynamicModelSlots(1024, 703, 0) == 257);
}

TEST("model lighting: a model shares the nearest slot that is lit")
{
    const auto none = std::numeric_limits<float>::max();
    const std::array<Slot, 4> slots{{
        {{none, none, none}, 0},       // never used: what the game resets the origins to
        {{100.0f, 0.0f, 0.0f}, 3},
        {{10.0f, 0.0f, 0.0f}, ModelLightingLogic::SLOT_BEING_LIT},
        {{0.0f, 50.0f, 0.0f}, 3},
    }};
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    CHECK(ModelLightingLogic::FindNearestSlot(slots, origin) == 3);
    const float nearFirst[3] = {90.0f, 0.0f, 0.0f};
    CHECK(ModelLightingLogic::FindNearestSlot(slots, nearFirst) == 1);
}

TEST("model lighting: no slot lit, none to share")
{
    const std::array<Slot, 2> slots{{
        {{1.0f, 0.0f, 0.0f}, ModelLightingLogic::SLOT_BEING_LIT},
        {{2.0f, 0.0f, 0.0f}, ModelLightingLogic::SLOT_BEING_LIT},
    }};
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    CHECK(ModelLightingLogic::FindNearestSlot(slots, origin) == -1);
    CHECK(ModelLightingLogic::FindNearestSlot({}, origin) == -1);
}
