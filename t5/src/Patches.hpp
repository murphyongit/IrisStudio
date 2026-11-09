#pragma once
#include "StdInclude.hpp"
#include "Addresses.hpp"
#include "../src/Utilities/Patches.hpp"

namespace IWXMVM::T5::Patches
{
    using namespace IWXMVM::Patches;

    struct T5Patches
    {
        // For rewinding
        NopPatch<5> Con_TimeJumped{GetGameAddresses().Con_TimeJumped_Call(), PatchApplySetting::Deferred};

        ReturnPatch IN_Frame{GetGameAddresses().IN_Frame(), PatchApplySetting::Deferred};

        // Like Quake 3, T5 ends demo playback on any key or mouse click, which includes input meant for the overlay.
        // Only escape keeps ending the demo.
        NopPatch<2> CL_KeyEvent_DemoAnyKey{GetGameAddresses().CL_KeyEvent_DemoAnyKey(), PatchApplySetting::Immediately};

        // Lets corpses keep a weapon in their hands, applied while "Attach weapon to player" is on
        JumpPatch CG_CorpseWeapon{GetGameAddresses().CG_CorpseWeapon(), PatchApplySetting::Deferred};
        // Returns false, so corpses play their whole death animation instead of turning into ragdolls. Applied together
        // with CG_CorpseWeapon: the weapon's bone isn't part of the ragdoll and would stay behind.
        Patch<3> BG_IsRagdollTrajectory{GetGameAddresses().BG_IsRagdollTrajectory(), "33 C0 C3"_bytes,
                                        PatchApplySetting::Deferred};

        // Hide the red flash after the player is hurt and the damage direction indicators drawn outside the menus, for
        // the blood overlay setting (see Hooks/HUD.cpp)
        ReturnPatch CG_DrawFlashDamage{GetGameAddresses().CG_DrawFlashDamage(), PatchApplySetting::Deferred};
        ReturnPatch CG_DrawDamageDirectionIndicators{GetGameAddresses().CG_DrawDamageDirectionIndicators(),
                                                     PatchApplySetting::Deferred};
    };

    inline T5Patches& GetGamePatches()
    {
        static T5Patches patches;
        return patches;
    }
}  // namespace IWXMVM::T5::Patches
