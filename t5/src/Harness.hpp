#pragma once
#include "Structures.hpp"

namespace IWXMVM::T5::Harness
{
    // remembers whether a menu was shown, for the "menus" command
    void OnMenuChecked(const Structures::menuDef_t* menu, bool visible);

    // runs what the harness queued for the main thread; called by the main thread every frame
    void RunMainThreadActions();

    // does nothing unless IWXMVM_HARNESS_DIR is set
    void Install();

    // "eject raw" unloads the mod without the game letting go of what it holds of it first
    bool SkipsPrepareUnload();
}  // namespace IWXMVM::T5::Harness
