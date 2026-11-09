#pragma once
#include "Structures.hpp"

namespace IWXMVM::T5::WeaponZones
{
    // The campaign's and Zombies' weapons for the player of a live game (a devmap now, a live game with actors later).
    // The server's give (Cmd_Give_f) lets the weapon look-up register a weapon the
    // map didn't precache (the flag at 0x344383C) once its asset is loaded, so `/give <weapon>` of a weapon of the
    // weapon catalog loads its zone, and its shot's sound bank, first (on the main thread's next frame), then gives it
    // with the game's own command. Nothing depends on a demo. A zone whose weapon the game registered stays loaded
    // while it is (ContentPacks).

    // Loads the zones of a weapon variant of the weapon catalog (its own, then its sound's), on the main thread; false
    // for one the catalog hasn't or whose zone doesn't load, with why
    bool LoadForPlayer(const std::string& variant, std::string* error = nullptr, bool clientStarting = false);

    // Clears what no held weapon uses of the player's 15 slots for clips and ammo, before a give; the game ends when
    // the clips are full, and never frees them itself
    void FreeAmmoSlots(Structures::playerState_s& player);

    // Loads the zones of the weapons give asked for that weren't loaded (loading them from inside the server's command
    // deadlocked with the loader), then gives them again. Main thread, each frame.
    void RunFrame();

    void Install();
}  // namespace IWXMVM::T5::WeaponZones
