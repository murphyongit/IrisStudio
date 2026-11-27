#pragma once
#include "StdInclude.hpp"

#include "Addresses.hpp"
#include "Utilities/Patches.hpp"

namespace IWXMVM::IW4::Patches
{
    using namespace IWXMVM::Patches;

    struct IW4Patches
    {
        // The game's own reading of the mouse (and its putting the pointer back in the middle), left out while the
        // editor has the mouse
        ReturnPatch IN_Frame{Addresses::IN_Frame, PatchApplySetting::Deferred};

        // CL_FirstSnapshot's call of Con_TimeJumped, left out while a rewind calls CL_FirstSnapshot again
        NopPatch<5> Con_TimeJumped{Addresses::CL_FirstSnapshot_TimeJumped, PatchApplySetting::Deferred};
    };

    inline IW4Patches& GetGamePatches()
    {
        static IW4Patches patches;
        return patches;
    }
}  // namespace IWXMVM::IW4::Patches
