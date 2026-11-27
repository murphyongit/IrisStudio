#pragma once
#include "StdInclude.hpp"

namespace IWXMVM::IW4::Hooks::Camera
{
    void Install();
    void OnCameraChanged();

    // A demo has been asked for: what waits for its map (the sky's images) is done once the map is there.
    void OnDemoLoad();
}  // namespace IWXMVM::IW4::Hooks::Camera
