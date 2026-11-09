#pragma once

namespace IWXMVM::T5::Hooks::HUD
{
    extern bool showPlayerHUD;
    extern bool showScore;
    extern bool showIconsAndText;
    extern bool showBloodOverlay;
    // the Look tab's Show Shellshock/Flashbang, which cg_drawShellshock has only in first person (Camera's
    // ApplyShellshock)
    extern bool showShellshock;

    void Install();
}  // namespace IWXMVM::T5::Hooks::HUD
