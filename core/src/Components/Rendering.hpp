#pragma once
#include "Components/CaptureManager.hpp"

namespace IWXMVM::Components::Rendering
{
    Types::RenderingFlags GetRenderingFlags();

    void SetVisibleElements(VisibleElements elements);
    // only what the game reads when it builds a frame (the first person gun), see CaptureManager::OnFrameHandedOver
    void SetVisibleElementsOfScene(VisibleElements elements);
    void ResetVisibleElements();

    void Initialize();
}  // namespace IWXMVM::Components::Rendering