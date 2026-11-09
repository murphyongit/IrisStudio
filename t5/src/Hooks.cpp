#include "StdInclude.hpp"
#include "Hooks.hpp"

#include "Hooks/Playback.hpp"
#include "Hooks/Camera.hpp"
#include "Hooks/HUD.hpp"
#include "Hooks/PlayerAnimation.hpp"
#include "Hooks/Loadouts.hpp"
#include "Hooks/Rendering.hpp"
#include "Hooks/Effects.hpp"
#include "Hooks/ScriptMemory.hpp"
#include "Hooks/EntityClients.hpp"
#include "DemoScanner.hpp"
#include "ContentPacks.hpp"
#include "Actors.hpp"
#include "Lights.hpp"
#include "ModelLighting.hpp"

#ifdef IWXMVM_T5_HARNESS
#include "Harness.hpp"
#endif

namespace IWXMVM::T5::Hooks
{
    void Install()
    {
#ifdef IWXMVM_T5_HARNESS
        // IWXMVM_SKIP_HOOKS="camera,hud,..." leaves those hooks out, for narrowing down failures (tools/t5-harness);
        // playback's are always in
        char skipList[256]{};
        GetEnvironmentVariableA("IWXMVM_SKIP_HOOKS", skipList, sizeof(skipList));
        const auto skip = std::string(",") + skipList + ",";
        const auto wanted = [&](const char* name) {
            return skip.find(std::string(",") + name + ",") == std::string::npos;
        };
#else
        const auto wanted = [](const char*) { return true; };
#endif
        Hooks::ScriptMemory::Install();
        Hooks::EntityClients::Install();
        Hooks::Playback::Install();
        if (wanted("camera"))
            Hooks::Camera::Install();
        if (wanted("hud"))
            Hooks::HUD::Install();
        if (wanted("playeranimation"))
            Hooks::PlayerAnimation::Install();
        if (wanted("loadouts"))
            Hooks::Loadouts::Install();
        if (wanted("rendering"))
            Hooks::Rendering::Install();
        if (wanted("effects"))
            Hooks::Effects::Install();
        if (wanted("demoscanner"))
            DemoScanner::Install();
        if (wanted("contentpacks"))
            ContentPacks::Install();
        if (wanted("actors"))
            Actors::Install();
            Lights::Install();
        ModelLighting::Install();

#ifdef IWXMVM_T5_HARNESS
        Harness::Install();
#endif
    }
}  // namespace IWXMVM::T5::Hooks
