#include "StdInclude.hpp"
#include "WeaponZones.hpp"

#include "Addresses.hpp"
#include "ContentPacks.hpp"
#include "DemoScanner.hpp"
#include "Functions.hpp"
#include "Mod.hpp"
#include "Structures.hpp"
#include "Components/Actors.hpp"
#include "Components/WeaponCatalog.hpp"
#include "Utilities/HookManager.hpp"
#include "Utilities/Patches.hpp"

namespace IWXMVM::T5::WeaponZones
{
    // The weapons of a zone (a variant, its left-hand gun, its alternate mode) whose parent in the weapon option tables
    // is single player's ("ray_gun" of ray_gun_upgraded_zm), which multiplayer's tables haven't: the parent is cleared,
    // as of a weapon of none, so the game registers them (it ended the film with "Parent weapon 'ray_gun' not found")
    void ClearForeignParents(const std::string& zone)
    {
        typedef std::int32_t(__cdecl * BG_FindWeaponOptionsParent_t)(const char* name);
        const auto find =
            reinterpret_cast<BG_FindWeaponOptionsParent_t>(GetGameAddresses().BG_FindWeaponOptionsParent());
        static constexpr const char* NONE = "";
        for (const auto weapon : ContentPacks::GetZoneWeapons(zone))
        {
            const auto def = static_cast<Structures::WeaponVariantDef*>(weapon)->weapDef;
            if (def && def->parentWeaponName && *def->parentWeaponName && find(def->parentWeaponName) == -1)
                def->parentWeaponName = NONE;
        }
    }

    // The damage of a wonder weapon, most of which is Zombies' scripts' (the Thundergun's def has none, the Wunderwaffe's
    // 1), written into its def: multiplayer's own code then does it, with its kill feed and film.
    // What the scripts do beyond damage stays Zombies': the blast's fling, freezing, shrinking.
    void ApplyDamage(Structures::WeaponVariantDef& variant)
    {
        const auto& damage = Components::WeaponCatalog::Get().damage;
        const auto found = variant.szInternalName ? damage.find(variant.szInternalName) : damage.end();
        if (found == damage.end() || !variant.weapDef)
            return;
        const auto def = variant.weapDef;
        def->iDamage = found->second.direct;
        def->iExplosionInnerDamage = found->second.inner;
        def->iExplosionOuterDamage = found->second.outer;
        def->iExplosionRadius = found->second.radius;
        def->bProjImpactExplode = true;
    }

    // of a zone's weapons, which are all but those another zone registered
    void ApplyDamage(const std::string& zone)
    {
        for (const auto weapon : ContentPacks::GetZoneWeapons(zone))
            ApplyDamage(*static_cast<Structures::WeaponVariantDef*>(weapon));
    }

    // of the wonder weapons loaded, whichever weapon zone has them (a map registers none of its own: campaign levels and
    // Zombies maps keep only their world's types, so a map's Ray Gun is the mod's zone's)
    void ApplyDamage()
    {
        using namespace Structures;
        for (const auto& name : Components::WeaponCatalog::Get().damage | std::views::keys)
            if (const auto variant = ContentPacks::FindAsset(ASSET_TYPE_WEAPON, name.c_str()))
                ApplyDamage(*static_cast<WeaponVariantDef*>(variant));
    }

    bool LoadForPlayer(const std::string& variant, std::string* error, bool clientStarting)
    {
        const auto zones = Components::WeaponCatalog::GetZones(Components::WeaponCatalog::Get(), variant);
        if (zones.empty())
        {
            if (error)
                *error = variant + " isn't a campaign or Zombies weapon";
            return false;
        }
        for (const auto& zone : zones)
        {
            if (ContentPacks::IsCharacterZoneLoaded(zone))
                continue;
            if (!ContentPacks::HasGameZone(zone))
            {
                // (its sound's bank missing only leaves its shots silent)
                if (zone != zones.front())
                    continue;
                if (error)
                    *error = std::format("{} isn't built: run Iris Studio", zone);
                return false;
            }
            const auto result = ContentPacks::LoadCharacterZone(zone, clientStarting, true);
            if (!result.error.empty() && zone == zones.front())
            {
                if (error)
                    *error = std::format("{} didn't load: {}", zone, result.error);
                return false;
            }
            LOG_INFO("{} for {}: loaded in {} ms{}", zone, variant, result.milliseconds,
                     result.error.empty() ? "" : " (" + result.error + ")");
        }
        ClearForeignParents(zones.front());
        ApplyDamage();
        return true;
    }

    // what give asked for whose zones aren't loaded yet: loaded on the main thread's next frame (from inside the
    // server's command the loader deadlocked), then given again (see RunFrame)
    std::mutex queueMutex;
    std::vector<std::string> queued;

    // A player has 15 slots for clips and for ammo, and when the clips are full the game ends ("BG_AddAmmoClip: Too many
    // weapons"). The game's give takes the place of the gun in hand, but never frees that gun's slots, even with take all:
    // going through the guns filled them while the player held 7 weapons. So what no held weapon uses is cleared
    // before each give.
    void FreeAmmoSlots(Structures::playerState_s& player)
    {
        using namespace Structures;
        const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        const auto count = Functions::GetWeaponVariantCount();
        std::vector<std::int32_t> ammo, clips;
        const auto use = [&](std::uint32_t index) {
            if (index > 0 && index < count && variants[index])
            {
                ammo.push_back(variants[index]->iAmmoIndex);
                clips.push_back(variants[index]->iClipIndex);
            }
        };
        for (const auto& weapon : player.heldWeapons)
        {
            if (weapon.weapon == 0 || weapon.weapon >= count || !variants[weapon.weapon])
                continue;
            const auto variant = variants[weapon.weapon];
            use(weapon.weapon);
            use(variant->altWeaponIndex);
            if (variant->weapDef && variant->weapDef->bDualWield)
                use(variant->weapDef->dualWieldWeaponIndex);
        }
        const auto clear = [](auto& slots, const std::vector<std::int32_t>& used) {
            for (auto& slot : slots)
                if (slot.index != 0 && std::ranges::find(used, slot.index) == used.end())
                    slot = {};
        };
        clear(player.ammoNotInClip, ammo);
        clear(player.ammoInClip, clips);
    }

    // Cmd_Give_f(ent): its arguments are the client command's, read with ConcatArgs(1) as it reads them
    typedef void(__cdecl* Cmd_Give_f_t)(void* ent);
    Cmd_Give_f_t Cmd_Give_f_Trampoline = nullptr;

    void __cdecl Cmd_Give_f_Hook(void* ent)
    {
        typedef const char*(__cdecl * ConcatArgs_t)(std::int32_t start);
        const auto args = reinterpret_cast<ConcatArgs_t>(GetGameAddresses().ConcatArgs())(1);
        std::string name(args ? args : "");
        name = name.substr(0, name.find(' '));
        const auto zones = Components::WeaponCatalog::GetZones(Components::WeaponCatalog::Get(), name);
        auto& player = **reinterpret_cast<Structures::playerState_s**>(static_cast<std::uint8_t*>(ent) +
                                                                       Structures::GENTITY_CLIENT);
        FreeAmmoSlots(player);
        if (!zones.empty() && !ContentPacks::IsCharacterZoneLoaded(zones.front()))
        {
            std::lock_guard lock(queueMutex);
            if (std::ranges::find(queued, std::string(args)) == queued.end())
                queued.emplace_back(args);
            return;
        }
        if (!zones.empty())
        {
            ClearForeignParents(zones.front());
            ApplyDamage(zones.front());
        }
        Cmd_Give_f_Trampoline(ent);
    }

    void RunFrame()
    {
        std::vector<std::string> gives;
        {
            std::lock_guard lock(queueMutex);
            gives.swap(queued);
        }
        for (const auto& give : gives)
        {
            const auto name = give.substr(0, give.find(' '));
            std::string error;
            if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InGame && LoadForPlayer(name, &error))
                Functions::Cbuf_AddText("give " + give);
            else
            {
                LOG_ERROR("give {}: {}", name, error.empty() ? "not in a game" : error);
                // (the answer to a command typed in the console)
                Functions::Cbuf_AddText(std::format("echo give {}: {}", name, error.empty() ? "not in a game" : error));
            }
        }
    }

    // A film's weapons (or a game's the client joins) are registered by the client from the configstrings of the
    // weapons as it starts, before any frame of the mod's could load their zones: a weapon of the weapon catalog's
    // zones are loaded as the client gets to its configstring (it found no such weapon else: "Weapon index mismatch for
    // 'ppsh_sp'", and the film ended).
    //
    // Only the client's start reads those strings. In a live game the client shares the server's weapon table, so a gun
    // given later needs nothing, but in a film the string of a gun given while it was recorded reaches a client that
    // ignores it (CG_ConfigStringModified, 0x7F0100, returns for them), and the gun is never registered: the film's
    // switch to it read an empty slot (0x529FFB) and its item mask ended the film with
    // CGAME_SERVER_WEAPON_INDEX_MISMATCH. So the client reads a film's strings as if all of them were there from its
    // start: its strings are in the order the server registered them, so each weapon gets the index the film has for
    // it, and nothing depends on where the film plays or jumps.
    typedef void(__cdecl* CG_SetupWeaponConfigString_t)(std::int32_t index);
    CG_SetupWeaponConfigString_t CG_SetupWeaponConfigString_Trampoline = nullptr;
    typedef const char*(__cdecl* CL_GetConfigString_t)(std::int32_t index);
    std::optional<Patches::Patch<5>> configStringCall;

    // the strings the film sets later than its start, from the first of them on
    std::map<std::int32_t, std::string> filmStrings;

    const char* GetWeaponString(std::int32_t index)
    {
        const auto text = reinterpret_cast<CL_GetConfigString_t>(GetGameAddresses().CL_GetConfigString())(
            index + Components::WeaponCatalog::WEAPON_CONFIG_STRINGS);
        if (text && *text)
            return text;
        const auto film = filmStrings.find(index);
        return film != filmStrings.end() ? film->second.c_str() : text;
    }

    // in place of the game's call of CL_GetConfigString in CG_SetupWeaponConfigString
    const char* __cdecl CL_GetConfigString_Weapon(std::int32_t configString)
    {
        return GetWeaponString(configString - Components::WeaponCatalog::WEAPON_CONFIG_STRINGS);
    }

    void __cdecl CG_SetupWeaponConfigString_Hook(std::int32_t index)
    {
        // (the game reads them in order, once as the client starts)
        if (index == 0)
        {
            filmStrings.clear();
            if (Mod::GetGameInterface()->IsPlayingDemo())
            {
                const auto started = std::chrono::steady_clock::now();
                filmStrings = DemoScanner::ReadWeaponStrings(Mod::GetGameInterface()->GetDemoInfo().path);
                LOG_INFO("The film registers {} more weapons as it plays (read in {} ms)", filmStrings.size(),
                         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                               started)
                             .count());
            }
        }
        const auto text = GetWeaponString(index);
        std::istringstream fields(text ? text : "");
        std::string number, name;
        fields >> number >> name;
        const auto zones = Components::WeaponCatalog::GetZones(Components::WeaponCatalog::Get(), name);
        if (!zones.empty())
        {
            std::string error;
            if (!LoadForPlayer(name, &error, true))
                LOG_ERROR("The weapon {} of the configstrings didn't load: {}", name, error);
        }
        CG_SetupWeaponConfigString_Trampoline(index);
    }

    void Install()
    {
        HookManager::CreateHook(GetGameAddresses().Cmd_Give_f(), (std::uintptr_t)Cmd_Give_f_Hook,
                                (std::uintptr_t*)&Cmd_Give_f_Trampoline);
        HookManager::CreateHook(GetGameAddresses().CG_SetupWeaponConfigString(),
                                (std::uintptr_t)CG_SetupWeaponConfigString_Hook,
                                (std::uintptr_t*)&CG_SetupWeaponConfigString_Trampoline);

        const auto callAddress = GetGameAddresses().CG_SetupWeaponConfigString() + 10;
        if (*reinterpret_cast<const std::uint8_t*>(callAddress) != 0xE8)
        {
            LOG_ERROR("CG_SetupWeaponConfigString doesn't read its string with a call: guns given in a film won't load");
            return;
        }
        std::array<std::uint8_t, 5> call{0xE8};
        const auto hookOffset =
            static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(CL_GetConfigString_Weapon) - (callAddress + 5));
        std::memcpy(&call[1], &hookOffset, sizeof(hookOffset));
        // reverted when the mod is unloaded
        configStringCall.emplace(callAddress, call, Patches::PatchApplySetting::Immediately);
    }
}  // namespace IWXMVM::T5::WeaponZones
