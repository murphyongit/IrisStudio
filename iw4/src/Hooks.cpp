#include "StdInclude.hpp"
#include "Hooks.hpp"

#include "Hooks/Camera.hpp"
#include "Hooks/Playback.hpp"

namespace IWXMVM::IW4::Hooks
{
    void Install()
    {
        Hooks::Playback::Install();
        Hooks::Camera::Install();
    }
}  // namespace IWXMVM::IW4::Hooks
