#include "StdInclude.hpp"
#include "LiveGame.hpp"

#include "Addresses.hpp"
#include "Functions.hpp"
#include "LiveActors.hpp"
#include "Structures.hpp"
#include "WeaponRig.hpp"
#include "WeaponZones.hpp"
#include "Components/LiveGame.hpp"
#include "Components/ShotLogic.hpp"
#include "Components/WeaponCatalog.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::T5::LiveGame
{
    using namespace Structures;
    namespace Logic = Components::LiveGameLogic;
    using Clock = std::chrono::steady_clock;

    // The player of a game this process hosts. Everything that touches the server runs on its thread (see LiveActors):
    // the frame hook there, and a shot, which the server fires itself. The main thread only says who the player is and
    // loads a gun's zones. A game someone else hosts runs none of it, and the main thread drops what is asked.
    std::atomic<std::int32_t> host = -1;

    // the guns whose zones are in, for the server's thread
    std::mutex gunsMutex;
    std::vector<Components::LiveGame::GunRequest> readyGuns;

    // _hardpoints.gsc's giveKillstreak for the map loaded, 0 for none, looked up as the game looks up its own callbacks
    std::atomic<std::int32_t> giveKillstreak = 0;
    inline constexpr const char* HARDPOINTS_SCRIPT = "maps/mp/gametypes/_hardpoints";

    // The game keeps its bodies in entities of their own, four of them, taking the place of the farthest when a fifth
    // comes (G_GetFreePlayerCorpseIndex, 0x5CDED0): numbers below the first free entity, so one freed is never reused
    // for something else (G_FreeEntity, 0x4EE050, frees a corpse's table entry too, as the game does on that fifth)
    inline constexpr std::int32_t BODY_COUNT = 4;
    struct Body
    {
        std::int32_t born = 0;  // the game's time it was made: another body in the same entity has another
        Clock::time_point seen;
    };
    std::array<std::optional<Body>, BODY_COUNT> bodies;
    std::atomic<std::int32_t> bodyCount = 0;

    // the first raise, a plain one (the game's usual one is its quick raise) and an empty magazine's, by the animation
    // the game starts (PM_Weapon_Raise, 0x7BFA10)
    inline constexpr std::int32_t WEAP_ANIM_RAISE = 0xD;
    inline constexpr std::int32_t WEAP_ANIM_FIRST_RAISE = 0xE;
    inline constexpr std::int32_t WEAP_ANIM_QUICK_RAISE = 0x18;
    inline constexpr std::int32_t WEAP_ANIM_EMPTY_RAISE = 0x1A;
    inline constexpr std::int32_t WEAP_ANIM_BITS = 0x3FF;
    struct RaiseCount
    {
        std::atomic<std::int32_t> first = 0;
        std::atomic<std::int32_t> plain = 0;
        std::atomic<std::int32_t> empty = 0;
        std::atomic<std::int32_t> last = 0;
    };
    RaiseCount serverRaises, clientRaises;

    std::mutex hitMutex;
    BulletHit lastHit;
    std::atomic<std::int32_t> bulletShots = 0;
    std::atomic<std::int32_t> bulletDirect = 0;

    std::mutex animationsMutex;
    std::vector<std::int32_t> recentAnimations;

    void CountRaise(RaiseCount& count, std::int32_t animation)
    {
        if (count.last.exchange(animation) == animation)
            return;
        const auto kind = animation & WEAP_ANIM_BITS;
        if (&count == &serverRaises)
        {
            std::lock_guard lock(animationsMutex);
            recentAnimations.push_back(kind);
        }
        if (kind == WEAP_ANIM_FIRST_RAISE)
            ++count.first;
        else if (kind == WEAP_ANIM_RAISE || kind == WEAP_ANIM_QUICK_RAISE)
            ++count.plain;
        else if (kind == WEAP_ANIM_EMPTY_RAISE)
            ++count.empty;
    }

    // The gun's first raise plays when its slot says the player hasn't had it yet (held.hadWeapon, set as the raise
    // starts), so that goes for every gun but the one in hand. No gun leaves the player, so its ammo is as it was, and the
    // game itself decides the rest: an empty magazine still comes out with its empty raise.
    void ClearFirstRaises(playerState_s& player)
    {
        for (auto& held : player.heldWeapons)
            if (held.weapon != 0 && held.weapon != player.weapon)
                held.hadWeapon = false;
    }

    const char* GetWeaponName(std::uint32_t index)
    {
        const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        return index > 0 && index < MAX_WEAPON_VARIANTS && variants[index] ? variants[index]->szInternalName : nullptr;
    }

    // Takes what it replaces, then gives it as a class gives a gun, with the flag that lets a gun the map didn't precache
    // be registered, and the player takes it out
    void GiveGun(std::int32_t client, playerState_s& player, const Components::LiveGame::GunRequest& gun)
    {
        WeaponZones::FreeAmmoSlots(player);
        const auto registerAnyWeapon = reinterpret_cast<std::int32_t*>(GetGameAddresses().g_registerAnyWeapon());
        const auto hand = GetWeaponName(player.weapon);
        if (gun.replace && hand && gun.weapon != hand)
            LiveActors::CallMethod(client, "takeweapon", {std::string(hand)});
        *registerAnyWeapon = 1;
        LiveActors::CallMethod(client, "giveweapon",
                               {gun.weapon, 0, static_cast<std::int32_t>(gun.camo & RENDER_OPTIONS_CAMO_BITS)});
        *registerAnyWeapon = 0;
        LiveActors::CallMethod(client, "givemaxammo", {gun.weapon});
        LiveActors::CallMethod(client, "switchtoweapon", {gun.weapon});
        LOG_INFO("Live game: gave {} ({})", gun.weapon, gun.replace ? "in place of the gun in hand" : "kept the others");
    }

    // The game's own function, which does what a streak does: the weapon in the fourth action slot, the notice and its
    // sound, and what the scripts count
    void GiveKillstreak(std::int32_t client, const std::string& type)
    {
        const auto function = giveKillstreak.load();
        if (!function)
        {
            Components::LiveGame::SetProblem("This game has no killstreaks");
            return;
        }
        const auto& addresses = GetGameAddresses();
        reinterpret_cast<void(__cdecl*)(const char*, std::int32_t)>(addresses.Scr_AddString())(type.c_str(), 0);
        const auto thread = reinterpret_cast<std::uint16_t(__cdecl*)(void*, std::int32_t, std::int32_t)>(
            addresses.Scr_ExecEntThread())(LiveActors::GetEntity(client), function, 1);
        reinterpret_cast<void(__cdecl*)(std::uint16_t, std::int32_t)>(addresses.Scr_FreeThread())(thread, 0);
        LOG_INFO("Live game: gave the killstreak {}", type);
    }

    // As the script's delete does: what waits on it hears it's gone, and the game frees the body and its table entry
    void FreeBody(std::uint8_t* entity)
    {
        const auto& addresses = GetGameAddresses();
        reinterpret_cast<void(__cdecl*)(void*, std::uint16_t, std::uint32_t)>(addresses.Scr_Notify())(
            entity, Functions::SL_GetString("death"), 0);
        reinterpret_cast<void(__cdecl*)(void*)>(addresses.G_FreeEntity())(entity);
    }

    void RunBodies(const Components::LiveGame::Settings& settings)
    {
        const auto clear = Components::LiveGame::TakeClearBodies();
        const auto now = Clock::now();
        std::int32_t count = 0;
        for (std::int32_t i = 0; i < BODY_COUNT; ++i)
        {
            const auto entity = LiveActors::GetEntity(static_cast<std::int32_t>(FIRST_CORPSE_ENTITY) + i);
            auto& body = bodies[i];
            if (!*reinterpret_cast<bool*>(entity + GENTITY_IN_USE) ||
                *reinterpret_cast<std::uint16_t*>(entity + GENTITY_TYPE) != ET_PLAYER_CORPSE)
            {
                body.reset();
                continue;
            }
            const auto born = *reinterpret_cast<std::int32_t*>(entity + GENTITY_CORPSE_TIME);
            if (!body || body->born != born)
                body = Body{born, now};
            if (clear || Logic::IsBodyDue(std::chrono::duration_cast<std::chrono::milliseconds>(now - body->seen),
                                          Logic::GetBodyTimer(settings.bodyTimer, settings.bodySeconds)))
            {
                FreeBody(entity);
                body.reset();
                continue;
            }
            ++count;
        }
        bodyCount = count;
    }

    // what a test asks to be done on this thread
    std::mutex serverCallsMutex;
    std::vector<std::function<void()>> serverCalls;

    void QueueOnServer(std::function<void()> call)
    {
        std::lock_guard lock(serverCallsMutex);
        serverCalls.push_back(std::move(call));
    }

    void RunServerFrame()
    {
        if (!LiveActors::IsLive())
            return;
        const auto settings = Components::LiveGame::GetSettings();
        RunBodies(settings);
        {
            std::lock_guard lock(serverCallsMutex);
            for (const auto& call : serverCalls)
                call();
            serverCalls.clear();
        }

        const auto client = host.load();
        const auto player = client >= 0 ? LiveActors::GetPlayerState(client) : nullptr;
        if (!player)
            return;
        CountRaise(serverRaises, player->weapAnim);
        if (settings.canswap)
            ClearFirstRaises(*player);

        // (what is asked of a dead player waits for it to come back)
        if (LiveActors::GetHealth(client) <= 0)
            return;
        {
            std::unique_lock lock(gunsMutex, std::try_to_lock);
            if (lock)
            {
                for (const auto& gun : readyGuns)
                    GiveGun(client, *player, gun);
                readyGuns.clear();
            }
        }
        for (const auto& type : Components::LiveGame::TakeKillstreakRequests())
            GiveKillstreak(client, type);
    }

    // Each gun's zones and whether the game has it, as an actor's gun. Main thread.
    void CheckGuns()
    {
        for (auto& gun : Components::LiveGame::TakeGunRequests())
        {
            std::string problem;
            if (Components::WeaponCatalog::Get().Has(gun.weapon))
                WeaponZones::LoadForPlayer(gun.weapon, &problem);
            if (!WeaponRig::FindVariant(gun.weapon) && problem.empty())
                problem = "The game has no " + gun.weapon;
            Components::LiveGame::SetProblem(problem);
            if (!problem.empty())
            {
                LOG_ERROR("Live game: {}", problem);
                continue;
            }
            std::lock_guard lock(gunsMutex);
            readyGuns.push_back(std::move(gun));
        }
    }

    void RunFrame()
    {
        if (!LiveActors::IsLive())
        {
            host = -1;
            bodyCount = 0;
            Components::LiveGame::DropRequests();
            std::lock_guard lock(gunsMutex);
            readyGuns.clear();
            return;
        }
        const auto cg = GetClientGlobals();
        host = cg && cg->snap ? static_cast<std::int32_t>(cg->snap->ps.clientNum) : -1;
        if (cg)
            CountRaise(clientRaises, cg->predictedPlayerState.weapAnim);
        CheckGuns();
    }

    std::int32_t GetBodyCount()
    {
        return bodyCount;
    }

    Raises GetRaises(bool client)
    {
        const auto& count = client ? clientRaises : serverRaises;
        return {count.first, count.plain, count.empty};
    }

    void ResetRaises()
    {
        for (auto* count : {&serverRaises, &clientRaises})
            count->first = count->plain = count->empty = 0;
        std::lock_guard lock(animationsMutex);
        recentAnimations.clear();
    }

    std::vector<std::int32_t> GetAnimations()
    {
        std::lock_guard lock(animationsMutex);
        return recentAnimations;
    }

    BulletHit GetLastBulletHit()
    {
        std::lock_guard lock(hitMutex);
        return lastHit;
    }

    std::array<std::int32_t, 2> GetBulletShots()
    {
        return {bulletShots, bulletDirect};
    }

    void ResetBulletCounts()
    {
        bulletShots = 0;
        bulletDirect = 0;
        std::lock_guard lock(hitMutex);
        lastHit = {};
    }

    // What a shot of a gun fires from (weaponParms): G_LocationalTrace's results are a trace with room behind it
    struct BulletTrace
    {
        trace_t trace;
        std::uint8_t rest[0x18];
    };

    glm::vec3 ToVec(const float* value)
    {
        return {value[0], value[1], value[2]};
    }

    // How far along a line it gets, stopping at the world and at bodies as a bullet does
    float TraceBullet(const glm::vec3& start, const glm::vec3& end, std::int32_t ignored, std::int32_t mask,
                      const void* priorities)
    {
        BulletTrace result{};
        const float from[3] = {start.x, start.y, start.z};
        const float to[3] = {end.x, end.y, end.z};
        reinterpret_cast<void(__cdecl*)(void*, const float*, const float*, std::int32_t, std::int32_t, const void*)>(
            GetGameAddresses().G_LocationalTrace())(&result, from, to, ignored, mask, priorities);
        return result.trace.fraction;
    }

    // Where a pellet of a shot lands: the same line the game worked out, from the same numbers (Bullet_Endpos depends on
    // nothing else, Bullet_Fire, 0x414C20), so the point is the one the shot's mark is at
    glm::vec3 FindLanding(const WeaponParms& weapon, float spread, std::int32_t seed, float range, std::int32_t shooter,
                          const void* priorities)
    {
        float end[3], direction[3];
        reinterpret_cast<void(__cdecl*)(std::int32_t, float, float*, float*, const WeaponParms*, float)>(
            GetGameAddresses().Bullet_Endpos())(seed, spread, end, direction, &weapon, range);
        const auto start = ToVec(weapon.muzzleTrace);
        const auto stop = ToVec(end);
        return glm::mix(start, stop, TraceBullet(start, stop, shooter, BULLET_TRACE_MASK, priorities));
    }

    std::atomic<bool> walls = true;

    void SetWallsStopBullets(bool stop)
    {
        walls = stop;
    }

    // whether nothing stands between where a shot landed and a chest (mask: no bodies)
    bool CanReach(const glm::vec3& landing, const glm::vec3& chest, std::int32_t shooter)
    {
        constexpr float OFF_THE_SURFACE = 2.0f;
        if (!walls)
            return true;
        const auto toward = chest - landing;
        const auto length = glm::length(toward);
        if (length < OFF_THE_SURFACE)
            return true;
        const auto start = landing + toward / length * OFF_THE_SURFACE;
        return TraceBullet(start, chest, shooter, MASK_SHOT_NO_CHARACTERS, nullptr) >= 1.0f;
    }

    struct Enemy
    {
        std::int32_t client = -1;
        std::int32_t health = 0;
        glm::vec3 origin{0.0f};
    };

    std::vector<Enemy> FindEnemies(std::int32_t shooter)
    {
        std::vector<Enemy> enemies;
        const auto maxClients = Functions::FindDvar("sv_maxclients");
        const auto count = std::min<std::int32_t>(maxClients ? maxClients->current.integer : 0, MAX_CLIENTS);
        const auto team = LiveActors::GetTeam(shooter);
        for (std::int32_t i = 0; i < count; ++i)
        {
            if (i == shooter || !LiveActors::IsInGame(i))
                continue;
            const auto player = LiveActors::GetPlayerState(i);
            const auto health = LiveActors::GetHealth(i);
            if (!player || health <= 0 || !Logic::IsEnemy(team, LiveActors::GetTeam(i), LiveActors::NO_TEAM))
                continue;
            enemies.push_back({i, health, ToVec(player->origin)});
        }
        return enemies;
    }

    // The hit of an Explosive Bullet: a bullet's in every way but its path. The game's own damage call, with the gun that
    // fired, the kind of bullet it fires, on the chest, from the shooter, so the scripts count it, the kill feed names the
    // gun and the death is chosen as from any shot. No effect and nothing explosive.
    void Strike(void* attacker, const WeaponParms& weapon, const Enemy& enemy, const glm::vec3& landing, bool kills)
    {
        constexpr std::int32_t ALWAYS_KILLS = 100000;
        constexpr std::int32_t DFLAGS_ARMOR_PIERCING = 2;
        const auto def = weapon.def;
        const auto chest = enemy.origin + glm::vec3(0.0f, 0.0f, Logic::CHEST_HEIGHT);
        const auto muzzle = ToVec(weapon.muzzleTrace);
        const auto damage = kills ? ALWAYS_KILLS
                                  : Logic::GetDamage({def->iDamage, def->iMinDamage, def->fNearRange, def->fFarRange},
                                                     glm::distance(muzzle, chest));
        const auto way = glm::normalize(chest - muzzle);
        const float direction[3] = {way.x, way.y, way.z};
        const float point[3] = {chest.x, chest.y, chest.z};
        const auto kind = Logic::GetBulletKind(def->bRifleBullet);
        const auto weaponIndex = *reinterpret_cast<std::uint16_t*>(static_cast<std::uint8_t*>(attacker) + GENTITY_WEAPON);
        reinterpret_cast<void(__cdecl*)(void*, void*, void*, const float*, const float*, std::int32_t, std::int32_t,
                                        std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                                        std::int32_t)>(GetGameAddresses().G_Damage())(
            LiveActors::GetEntity(enemy.client), attacker, attacker, direction, point, damage,
            def->bArmorPiercing ? DFLAGS_ARMOR_PIERCING : 0, kind, weaponIndex, Logic::HIT_TORSO_UPPER, 0, 0, 0);

        std::lock_guard lock(hitMutex);
        lastHit.client = enemy.client;
        lastHit.damage = damage;
        lastHit.kind = kind;
        lastHit.weapon = GetWeaponName(weaponIndex) ? GetWeaponName(weaponIndex) : "";
        lastHit.point = {landing.x, landing.y, landing.z};
        ++lastHit.count;
    }

    // After the shot itself has done what it does: when it hit no one, the enemy nearest to where it landed takes the hit
    void RunExplosiveBullet(void* attacker, float spread, const WeaponParms& weapon, std::int32_t gameTime,
                            std::span<const Enemy> before, const Components::LiveGame::Settings& settings)
    {
        ++bulletShots;
        for (const auto& enemy : before)
            if (LiveActors::GetHealth(enemy.client) < enemy.health)
            {
                ++bulletDirect;
                return;
            }

        const auto def = weapon.def;
        const auto shooter = *static_cast<std::int32_t*>(attacker);
        const auto range = Functions::FindDvar("bulletrange");
        const auto spreads = def->weapClass == WEAPCLASS_SPREAD;
        // (where the view aims is the line with no spread: the same whatever the seed, so one is enough)
        const auto pellets = spreads && !settings.fromAim ? def->shotCount : 1;
        const auto cone = settings.fromAim ? 0.0f : spread;
        const auto distance = spreads              ? def->fFarRange
                              : range != nullptr ? range->current.value
                                                 : Components::ShotLogic::SHOT_RANGE;
        const auto priorities = def->bRifleBullet ? reinterpret_cast<const void*>(GetGameAddresses().bulletPriorityRifle())
                                                  : reinterpret_cast<const void*>(GetGameAddresses().bulletPriorityPistol());

        std::vector<glm::vec3> origins;
        origins.reserve(before.size());
        for (const auto& enemy : before)
            origins.push_back(enemy.origin);

        const Enemy* nearest = nullptr;
        glm::vec3 landing(0.0f);
        auto best = settings.radius;
        for (std::int32_t pellet = 0; pellet < pellets; ++pellet)
        {
            const auto point = FindLanding(weapon, cone, gameTime + pellet, distance, shooter, priorities);
            for (const auto index : Logic::FindWithin(point, settings.radius, origins))
            {
                const auto reach = Logic::GetDistanceToBody(point, origins[index]);
                const auto chest = origins[index] + glm::vec3(0.0f, 0.0f, Logic::CHEST_HEIGHT);
                if (reach > best || !CanReach(point, chest, shooter))
                    continue;
                best = reach;
                nearest = &before[index];
                landing = point;
                break;
            }
        }
        if (nearest)
            Strike(attacker, weapon, *nearest, landing, settings.bulletKills);
    }

    // Bullet_Fire(attacker, spread, weapon, weaponEntity, gameTime): only bullets come here, whoever fires them, so no
    // launcher or grenade ever does; a turret or a vehicle fires with an entity of its own
    typedef void(__cdecl* Bullet_Fire_t)(void* attacker, float spread, const WeaponParms* weapon, void* weaponEntity,
                                         std::int32_t gameTime);
    Bullet_Fire_t Bullet_Fire_Trampoline = nullptr;

    void __cdecl Bullet_Fire_Hook(void* attacker, float spread, const WeaponParms* weapon, void* weaponEntity,
                                  std::int32_t gameTime)
    {
        const auto client = host.load();
        const auto mine = LiveActors::IsLive() && client >= 0 && attacker == LiveActors::GetEntity(client) &&
                          weaponEntity == attacker;
        const auto settings = mine ? Components::LiveGame::GetSettings() : Components::LiveGame::Settings{};
        if (!settings.explosiveBullet || !Logic::FiresBullets(weapon->def->weapType))
        {
            Bullet_Fire_Trampoline(attacker, spread, weapon, weaponEntity, gameTime);
            return;
        }
        const auto before = FindEnemies(client);
        Bullet_Fire_Trampoline(attacker, spread, weapon, weaponEntity, gameTime);
        RunExplosiveBullet(attacker, spread, *weapon, gameTime, before, settings);
    }

    // The scripts' load, where the game looks up its callbacks
    typedef void(__cdecl* GScr_LoadGameTypeScript_t)();
    GScr_LoadGameTypeScript_t GScr_LoadGameTypeScript_Trampoline = nullptr;

    void __cdecl GScr_LoadGameTypeScript_Hook()
    {
        GScr_LoadGameTypeScript_Trampoline();
        const auto& addresses = GetGameAddresses();
        const auto loaded =
            reinterpret_cast<std::int32_t(__cdecl*)(std::int32_t, const char*)>(addresses.Scr_LoadScript())(
                0, HARDPOINTS_SCRIPT);
        giveKillstreak =
            loaded ? reinterpret_cast<std::int32_t(__cdecl*)(std::int32_t, const char*, const char*)>(
                         addresses.Scr_GetFunctionHandle())(0, HARDPOINTS_SCRIPT, "giveKillstreak")
                   : 0;
        LOG_INFO("Live game: giveKillstreak is at {:#x}", giveKillstreak.load());
    }

    void Install()
    {
        HookManager::CreateHook(GetGameAddresses().Bullet_Fire(), (std::uintptr_t)Bullet_Fire_Hook,
                                (std::uintptr_t*)&Bullet_Fire_Trampoline);
        HookManager::CreateHook(GetGameAddresses().GScr_LoadGameTypeScript(),
                                (std::uintptr_t)GScr_LoadGameTypeScript_Hook,
                                (std::uintptr_t*)&GScr_LoadGameTypeScript_Trampoline);
    }
}  // namespace IWXMVM::T5::LiveGame
