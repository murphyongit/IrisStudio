#pragma once

// While the editor has the mouse (the free camera turns by how far the cursor moved from the middle of the game's
// picture, and puts it back there), nothing else may move the cursor: something putting it at the middle of the
// window every frame made the camera spin, as the two middles differ. The game's own mouse code is switched off by a
// patch; this stops everything else in the process (Plutonium's input) at SetCursorPos itself.
namespace IWXMVM::IW5::CursorGuard
{
    void Install();
    void SetEditorHasMouse(bool has);
}  // namespace IWXMVM::IW5::CursorGuard
