#include "StdInclude.hpp"
#include "ModelLightingLogic.hpp"

namespace IWXMVM::Components::ModelLightingLogic
{
    std::uint32_t GetDynamicModelSlots(std::uint32_t slots, std::uint32_t sceneDObjs, std::uint32_t sceneModels)
    {
        const auto taken = sceneDObjs + sceneModels + RESERVED_SLOTS;
        return slots > taken + LEAST_DYNAMIC_MODEL_SLOTS ? slots - taken : LEAST_DYNAMIC_MODEL_SLOTS;
    }

    std::int32_t FindNearestSlot(std::span<const Slot> slots, const float* origin)
    {
        std::int32_t nearest = -1;
        float best = std::numeric_limits<float>::max();
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            const auto& slot = slots[i];
            if (slot.primaryLight == SLOT_BEING_LIT)
                continue;
            const auto x = slot.origin[0] - origin[0];
            const auto y = slot.origin[1] - origin[1];
            const auto z = slot.origin[2] - origin[2];
            const auto distance = x * x + y * y + z * z;
            if (distance < best)
            {
                best = distance;
                nearest = static_cast<std::int32_t>(i);
            }
        }
        return nearest;
    }
}  // namespace IWXMVM::Components::ModelLightingLogic
