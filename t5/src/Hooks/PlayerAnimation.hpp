#pragma once

namespace IWXMVM::T5::Hooks::PlayerAnimation
{
    // the catalog animation a corpse plays as its death, empty for one of the players'
    std::string GetCatalogDeath(std::int32_t number);

    // a live game's map is over: what was kept of it goes (a demo's load does the same)
    void ResetLive();

    // a rewind takes the demo back to its start, or another film starts: the times the game's IK keeps of later moments
    // go
    void ForgetIkTimes();
    // off, ForgetIkTimes leaves them, as before its fix (a test with the fix off)
    void SetIkTimesForgotten(bool on);

    void Install();
}  // namespace IWXMVM::T5::Hooks::PlayerAnimation
