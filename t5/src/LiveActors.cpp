#include "StdInclude.hpp"
#include "LiveActors.hpp"

#include "Actors.hpp"
#include "Addresses.hpp"
#include "ContentPacks.hpp"
#include "Functions.hpp"
#include "LiveGame.hpp"
#include "Mod.hpp"
#include "Structures.hpp"
#include "WeaponRig.hpp"
#include "WeaponZones.hpp"
#include "Components/LiveActors.hpp"
#include "Components/Loadouts.hpp"
#include "Components/PlayerAnimation.hpp"
#include "Components/WeaponCatalog.hpp"
#include "Hooks/Loadouts.hpp"
#include "Hooks/PlayerAnimation.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::T5::LiveActors
{
    using namespace Structures;
    namespace Logic = Components::LiveActorLogic;
    using Clock = std::chrono::steady_clock;

    // A test client is added by the game's own SV_AddTestClient, and from then on it is a player: it joins through the
    // answers the menus give, the scripts give it a class and spawn it, SV_BotUserMove makes up its input from the
    // map's path nodes, and it takes damage, dies, comes back and shows in a film like any other. All that is done here
    // is to steer that: the first frames of a life get its gun and place, and SV_BotUserMove's result is cut down to
    // what the actor's mode lets through. Its name and look are the game's.
    //
    // The game's server runs on a thread of its own, scripts and all, beside the main thread. Everything that touches
    // it runs on that thread, at the start of the step where it moves the bots (SV_BotsThink), where no script is
    // running: the script machine has one stack, and a parameter pushed from the main thread lands in the middle of
    // whatever script is running (a crash a few kicks later). Zones load on the main thread (they deadlock on the
    // server's), so that one sees to what a gun needs and what a new map resets, and the two share the state below.

    struct Slot
    {
        std::uint32_t id = 0;
        std::int32_t number = -1;  // its client and entity number, -1 until added
        Clock::time_point added;
        std::optional<Clock::time_point> teamChosen, classChosen;
        bool spawned = false;  // it has been alive
        bool alive = false;
        std::optional<Clock::time_point> aliveSince;
        bool settled = false;                     // this life got its gun and place
        std::optional<Types::LiveActor> applied;  // what it has of the choices
        std::optional<Types::LiveActor> armed;    // the choices its gun is from, in this life
        bool named = false;                       // its clan tag is gone
        bool placed = false;                      // it has been put at its place, this client
        std::string problem;
    };
    std::mutex stateMutex;  // the two threads: the server's only tries it, the main thread's work never holds it long
    std::vector<Slot> slots;
    // the guns actors hold, each with why it can't be held, empty when it can (the main thread looks them up)
    std::map<std::string, std::string> weapons;
    // what the harness asked to answer for the player: its client, the menu and the response
    std::vector<std::tuple<std::int32_t, std::string, std::string>> hostAnswers;
    std::optional<Clock::time_point> lastAdd;
    inline constexpr std::chrono::seconds FULL_GAME_RETRY{5};
    std::optional<Types::LiveActorSkill> appliedSkill;
    std::string mapName;

    std::atomic<bool> live = false;
    // the hook's view of the actors: their mode by client number, -1 for no actor
    std::array<std::atomic<std::int32_t>, MAX_CLIENTS> clientModes;

    std::uint8_t* GetServerClient(std::int32_t number)
    {
        const auto clients = *reinterpret_cast<std::uint8_t**>(GetGameAddresses().svs_clients());
        return clients ? clients + number * SERVER_CLIENT_SIZE : nullptr;
    }

    std::uint8_t* GetEntity(std::int32_t number)
    {
        const auto& addresses = GetGameAddresses();
        return *reinterpret_cast<std::uint8_t**>(addresses.sv_gentities()) +
               number * *reinterpret_cast<std::int32_t*>(addresses.sv_gentitySize());
    }

    playerState_s* GetPlayerState(std::int32_t number)
    {
        return *reinterpret_cast<playerState_s**>(GetEntity(number) + GENTITY_CLIENT);
    }

    std::int32_t GetHealth(std::int32_t number)
    {
        return *reinterpret_cast<std::int32_t*>(GetEntity(number) + GENTITY_HEALTH);
    }

    std::int32_t GetTeam(std::int32_t number)
    {
        const auto state = reinterpret_cast<const std::uint8_t*>(GetPlayerState(number));
        return state ? *reinterpret_cast<const std::int32_t*>(state + GCLIENT_TEAM) : Logic::TEAM_FREE;
    }

    bool IsInGame(std::int32_t number)
    {
        const auto client = GetServerClient(number);
        return client && *reinterpret_cast<std::int32_t*>(client) == SERVER_CLIENT_ACTIVE;
    }

    // whether the client is the test client it was when it was added
    bool IsTestClient(std::int32_t number)
    {
        const auto client = GetServerClient(number);
        return client && *reinterpret_cast<std::int32_t*>(client) == SERVER_CLIENT_ACTIVE &&
               *reinterpret_cast<std::int32_t*>(client + SERVER_CLIENT_IS_BOT) != 0;
    }

    // the team of the player of this game, from the first client that isn't a bot and has one
    std::int32_t GetHostTeam(std::int32_t maxClients)
    {
        for (std::int32_t i = 0; i < maxClients; ++i)
        {
            const auto client = GetServerClient(i);
            if (!client || *reinterpret_cast<std::int32_t*>(client) != SERVER_CLIENT_ACTIVE ||
                *reinterpret_cast<std::int32_t*>(client + SERVER_CLIENT_IS_BOT) != 0)
                continue;
            const auto state = reinterpret_cast<const std::uint8_t*>(GetPlayerState(i));
            const auto team = state ? *reinterpret_cast<const std::int32_t*>(state + GCLIENT_TEAM) : Logic::TEAM_FREE;
            if (team == Logic::TEAM_AXIS || team == Logic::TEAM_ALLIES)
                return team;
        }
        return Logic::TEAM_FREE;
    }

    // What a script gets to call on a player. They are called the way the script machine does: the parameters go on
    // its stack, the last first, and the function reads them from there as the parameters of the call it is in.
    struct ScriptMethod
    {
        const char* name;
        void(__cdecl* function)(std::uint32_t entity);
        std::int32_t developerOnly;
    };
    inline constexpr std::size_t PLAYER_METHOD_COUNT = 166;

    void PushScriptValue(const ScriptValue& value)
    {
        const auto& addresses = GetGameAddresses();
        if (const auto text = std::get_if<std::string>(&value))
            reinterpret_cast<void(__cdecl*)(const char*, std::int32_t)>(addresses.Scr_AddString())(text->c_str(), 0);
        else if (const auto number = std::get_if<std::int32_t>(&value))
            reinterpret_cast<void(__cdecl*)(std::int32_t, std::int32_t)>(addresses.Scr_AddInt())(*number, 0);
        else
            reinterpret_cast<void(__cdecl*)(const float*, std::int32_t)>(addresses.Scr_AddVector())(
                std::get<std::array<float, 3>>(value).data(), 0);
    }

    void CallMethod(std::int32_t number, std::string_view name, std::vector<ScriptValue> values)
    {
        const auto& addresses = GetGameAddresses();
        const auto methods = reinterpret_cast<const ScriptMethod*>(addresses.player_methods());
        const auto method = std::ranges::find_if(methods, methods + PLAYER_METHOD_COUNT,
                                                 [&](const ScriptMethod& entry) { return name == entry.name; });
        if (method == methods + PLAYER_METHOD_COUNT)
        {
            LOG_ERROR("The game has no player method {}", name);
            return;
        }

        std::ranges::for_each(values | std::views::reverse, PushScriptValue);
        // (what was pushed so far are parameters for a call to come, then those of the call being made)
        const auto outParameters = reinterpret_cast<std::uint32_t*>(addresses.scr_outparamcount());
        const auto inParameters = outParameters - 1;
        *outParameters = *inParameters;
        *inParameters = 0;
        method->function(static_cast<std::uint32_t>(number));
        reinterpret_cast<void(__cdecl*)(std::int32_t)>(addresses.Scr_ClearOutParams())(0);
    }

    void Notify(std::int32_t number, std::uint16_t name, std::string_view first, std::string_view second)
    {
        const auto& addresses = GetGameAddresses();
        const auto add = reinterpret_cast<void(__cdecl*)(const char*, std::int32_t)>(addresses.Scr_AddString());
        add(std::string(second).c_str(), 0);
        add(std::string(first).c_str(), 0);
        reinterpret_cast<void(__cdecl*)(void*, std::uint16_t, std::uint32_t)>(addresses.Scr_Notify())(GetEntity(number),
                                                                                                      name, 2);
    }

    void AnswerMenu(std::int32_t number, std::string_view menu, std::string_view response)
    {
        // (script strings are the map's: never kept from one to the next, or the script hears something else)
        Notify(number, Functions::SL_GetString("menuresponse"), menu, response);
    }

    // Takes what the scripts gave it, then gives the chosen gun (and the knife, what a bot melees with), as a class
    // does
    void GiveWeapon(const Slot& slot, const Types::LiveActor& actor)
    {
        if (actor.weapon.empty())
            return;

        const auto registerAnyWeapon = reinterpret_cast<std::int32_t*>(GetGameAddresses().g_registerAnyWeapon());
        *registerAnyWeapon = 1;
        CallMethod(slot.number, "takeallweapons");
        CallMethod(slot.number, "giveweapon", {std::string("knife_mp")});
        CallMethod(slot.number, "giveweapon",
                   {actor.weapon, 0, static_cast<std::int32_t>(actor.camo & RENDER_OPTIONS_CAMO_BITS)});
        *registerAnyWeapon = 0;
        CallMethod(slot.number, "givemaxammo", {actor.weapon});
        CallMethod(slot.number, "setspawnweapon", {actor.weapon});
    }

    // Still and Guard go back to their place each life; Roam starts there, and runs from a spawn point after that
    void PlaceActor(Slot& slot, const Types::LiveActor& actor)
    {
        if (!actor.origin || (actor.mode == Types::LiveActorMode::Roam && slot.placed))
            return;
        slot.placed = true;
        CallMethod(slot.number, "setorigin", {*actor.origin});
        CallMethod(slot.number, "setplayerangles", {std::array{0.0f, actor.yaw, 0.0f}});
    }

    // What the kill feed and the scoreboard show: the game's bot name, as in Combat Training, without the clan tag of
    // the developers' names it gives bots (the client's state in the snapshots has it)
    void ApplyName(Slot& slot)
    {
        if (slot.named)
            return;
        const auto state = reinterpret_cast<std::uint8_t*>(GetPlayerState(slot.number));
        if (!state)
            return;
        slot.named = true;
        *reinterpret_cast<char*>(state + GCLIENT_CLAN) = 0;
    }

    void Drop(Slot& slot)
    {
        if (slot.number < 0)
            return;
        clientModes[slot.number] = -1;
        if (IsTestClient(slot.number))
            Functions::Cbuf_AddText(std::format("clientkick {}", slot.number));
        slot.number = -1;
    }

    // Puts a slot right after its client went away by itself, as a restart of the server takes test clients
    void Forget(Slot& slot)
    {
        clientModes[slot.number] = -1;
        const auto id = slot.id;
        slot = {};
        slot.id = id;
    }

    void AddClient(Slot& slot, std::int32_t maxClients)
    {
        const auto now = Clock::now();
        if (lastAdd && now < *lastAdd + Logic::BETWEEN_ADDS)
            return;
        lastAdd = now;

        const auto entity = reinterpret_cast<std::uint8_t*(__cdecl*)()>(GetGameAddresses().SV_AddTestClient())();
        if (!entity)
        {
            slot.problem = std::format("No room: the game takes {} players", maxClients);
            lastAdd = now + FULL_GAME_RETRY;
            return;
        }
        slot.problem.clear();
        slot.number = *reinterpret_cast<std::int32_t*>(entity);
        slot.added = now;
        LOG_INFO("Live actor {} is client {}", slot.id, slot.number);
    }

    void RunJoin(Slot& slot, const Types::LiveActor& actor, std::int32_t maxClients)
    {
        const auto now = Clock::now();
        const auto since = [&](const std::optional<Clock::time_point>& time) {
            return time ? std::optional(std::chrono::duration_cast<std::chrono::milliseconds>(now - *time))
                        : std::nullopt;
        };
        const Logic::Join join{std::chrono::duration_cast<std::chrono::milliseconds>(now - slot.added),
                               since(slot.teamChosen), since(slot.classChosen), slot.spawned};
        switch (Logic::GetJoinStep(join))
        {
            case Logic::JoinStep::Team:
                AnswerMenu(slot.number, Logic::MENU_TEAM, Logic::GetTeamResponse(actor.side, GetHostTeam(maxClients)));
                slot.teamChosen = now;
                break;
            case Logic::JoinStep::Class:
                AnswerMenu(slot.number, Logic::MENU_CLASS, Logic::CLASS_RESPONSE);
                slot.classChosen = now;
                break;
            default:
                break;
        }
    }

    // whether its gun can be given yet; a campaign or Zombies gun's zones hold it up (see CheckWeapons)
    bool IsWeaponReady(Slot& slot, const Types::LiveActor& actor)
    {
        if (actor.weapon.empty())
            return true;
        const auto weapon = weapons.find(actor.weapon);
        if (weapon == weapons.end())
            return false;
        slot.problem = weapon->second;
        return slot.problem.empty();
    }

    void RunLife(Slot& slot, const Types::LiveActor& actor)
    {
        const auto now = Clock::now();
        const bool alive = GetHealth(slot.number) > 0;
        if (alive && !slot.alive)
        {
            slot.spawned = true;
            slot.aliveSince = now;
            slot.settled = false;
        }
        slot.alive = alive;
        if (!alive)
            return;

        if (!slot.settled)
        {
            if (now - *slot.aliveSince < Logic::SPAWN_SETTLE)
                return;
            // (what is left of a bot's class: an actor has no perks it wasn't given, Second Chance among them)
            CallMethod(slot.number, "clearperks");
            PlaceActor(slot, actor);
            slot.armed.reset();
            slot.settled = true;
        }
        else if (slot.applied && (Logic::NeedsPlacing(*slot.applied, actor) || slot.applied->mode != actor.mode))
        {
            PlaceActor(slot, actor);
        }

        // (a gun whose zones are still loading is given once they are)
        if ((!slot.armed || Logic::NeedsWeapon(*slot.armed, actor)) && IsWeaponReady(slot, actor))
        {
            GiveWeapon(slot, actor);
            slot.armed = actor;
        }
    }

    void ReportState(const Slot& slot)
    {
        Types::LiveActorState state;
        state.problem = slot.problem;
        if (slot.number >= 0)
        {
            state.client = slot.number;
            const auto client = GetServerClient(slot.number);
            state.name = reinterpret_cast<const char*>(client + SERVER_CLIENT_NAME);
            state.alive = slot.alive;
            state.health = GetHealth(slot.number);
            if (const auto ps = GetPlayerState(slot.number))
            {
                std::ranges::copy(ps->origin, state.origin.begin());
                const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
                if (ps->weapon > 0 && ps->weapon < MAX_WEAPON_VARIANTS && variants[ps->weapon] &&
                    variants[ps->weapon]->szInternalName)
                    state.weapon = variants[ps->weapon]->szInternalName;
            }
        }
        Components::LiveActors::SetState(slot.id, std::move(state));
    }

    void ApplySkill()
    {
        const auto skill = Components::LiveActors::GetSkill();
        if (appliedSkill == skill)
            return;
        appliedSkill = skill;
        // (written as scripts write them: the console refuses these dvars without cheats, as in a private match)
        for (const auto& [name, value] : Logic::GetSkillDvars(skill))
        {
            const auto dvar = Functions::FindDvar(name);
            if (!dvar)
                continue;
            const std::string text(value);
            if (dvar->type == DVAR_TYPE_FLOAT)
                dvar->current.value = std::stof(text);
            else if (dvar->type == DVAR_TYPE_INT)
                dvar->current.integer = std::stoi(text);
            else
                dvar->current.enabled = text != "0";
        }
    }

    void ForgetSlots()
    {
        for (auto& slot : slots)
            if (slot.number >= 0)
                clientModes[slot.number] = -1;
        slots.clear();
        weapons.clear();
        hostAnswers.clear();
        appliedSkill.reset();
        lastAdd.reset();
    }

    bool IsHosting()
    {
        const auto running = Functions::FindDvar("sv_running");
        return running && running->current.enabled;
    }

    // how many actors this game can have: those there are and its free clients
    std::int32_t CountRoom(std::int32_t maxClients)
    {
        std::int32_t taken = 0;
        for (std::int32_t i = 0; i < maxClients; ++i)
        {
            const auto client = GetServerClient(i);
            taken += client && *reinterpret_cast<std::int32_t*>(client) != 0 &&
                     !std::ranges::any_of(slots, [&](const Slot& slot) { return slot.number == i; });
        }
        return Logic::GetRoom(maxClients, taken);
    }

    // Whether a line stops at what a player's body is stopped by
    bool IsBlocked(const glm::vec3& start, const glm::vec3& end)
    {
        trace_t result{};
        const float from[3] = {start.x, start.y, start.z};
        const float to[3] = {end.x, end.y, end.z};
        Functions::CG_LocationalTrace(&result, from, to, ENTITYNUM_NONE, MASK_PLAYER_SOLID);
        return result.fraction < 1.0f;
    }

    // The way out of what a standing player's box at this point of the ground touches, or none if it fits: the way
    // from what was hit, or nothing to go by for the head, or a floor that only bullets stop at. A body put inside a
    // wall or under a ledge is stuck, and the game nudges it down through the floor a unit a frame, until the fall
    // under the map kills it; past the map's edge the ground is only there to look at, and it falls through.
    std::optional<glm::vec2> FindWayOut(const glm::vec3& ground)
    {
        // (the box is 15 each way, and a wall that isn't square to the map meets its corners first)
        constexpr float HALF_WIDTH = 17.0f;
        constexpr std::array HEIGHTS{20.0f, 40.0f, 68.0f};
        constexpr std::array<glm::vec2, 8> AROUND{
            {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}}};
        constexpr float HEAD = 72.0f;
        constexpr float FLOOR = 16.0f;

        glm::vec2 way(0.0f);
        bool blocked = IsBlocked(ground + glm::vec3(0, 0, HEIGHTS[0]), ground + glm::vec3(0, 0, HEAD)) ||
                       !IsBlocked(ground + glm::vec3(0, 0, FLOOR), ground - glm::vec3(0, 0, FLOOR));
        for (const auto height : HEIGHTS)
        {
            const auto middle = ground + glm::vec3(0, 0, height);
            for (const auto& side : AROUND)
                if (IsBlocked(middle, middle + glm::vec3(side * HALF_WIDTH, 0.0f)))
                {
                    way -= glm::normalize(side);
                    blocked = true;
                }
        }
        return blocked ? std::optional(way) : std::nullopt;
    }

    // Ground near this point that a player fits on, stepping away from what it touches
    std::optional<glm::vec3> StepClear(glm::vec3 ground)
    {
        constexpr std::int32_t MOST_STEPS = 12;
        constexpr float STEP = 10.0f;
        constexpr float ABOVE = 40.0f;
        for (std::int32_t step = 0; step < MOST_STEPS; ++step)
        {
            const auto way = FindWayOut(ground);
            if (!way)
                return ground;
            if (glm::length(*way) < 0.01f)
                return std::nullopt;
            const auto floor =
                Actors::FindGround(ground + glm::vec3(glm::normalize(*way) * STEP, ABOVE), {0.0f, 0.0f, -1.0f});
            if (!floor)
                return std::nullopt;
            ground = *floor;
        }
        return std::nullopt;
    }

    // The ground nearest this point that a player fits on, coming back along the line from the view if it has to:
    // that line is open air, so what is under it is the map, as far out as the view went
    glm::vec3 FindRoomToStand(const glm::vec3& ground, const glm::vec3& view)
    {
        constexpr float STRIDE = 48.0f;
        const auto distance = glm::distance(ground, view);
        for (float back = 0.0f; back < distance; back += STRIDE)
        {
            const auto floor = back == 0.0f
                                   ? std::optional(ground)
                                   : Actors::FindGround(glm::mix(ground, view, back / distance), {0.0f, 0.0f, -1.0f});
            if (!floor)
                continue;
            if (const auto clear = StepClear(*floor))
                return *clear;
        }
        return ground;
    }

    // The ground where the view looks, or a point of it, facing the player: where an actor goes. Main thread.
    void RunPlaceRequests()
    {
        const auto requests = Components::LiveActors::TakePlaceRequests();
        const auto cg = GetClientGlobals();
        if (requests.empty() || !cg)
            return;
        const auto& view = cg->refdef;
        const glm::vec3 origin(view.vieworg[0], view.vieworg[1], view.vieworg[2]);
        const glm::vec3 forward(view.viewaxis[0][0], view.viewaxis[0][1], view.viewaxis[0][2]);
        const glm::vec3 left(view.viewaxis[1][0], view.viewaxis[1][1], view.viewaxis[1][2]);
        const glm::vec3 up(view.viewaxis[2][0], view.viewaxis[2][1], view.viewaxis[2][2]);
        for (const auto& request : requests)
        {
            auto actor = Components::LiveActors::GetActor(request.id);
            if (!actor)
                continue;
            auto direction = forward;
            if (request.at)
                direction += -left * (*request.at)[0] * view.tanHalfFovX + up * (*request.at)[1] * view.tanHalfFovY;
            auto ground = Actors::FindGround(origin, direction);
            // (the sky is no place to stand: a few steps ahead instead)
            constexpr float MOST_DISTANCE = 3000.0f;
            constexpr float STEPS_AHEAD = 250.0f;
            if (!ground || glm::distance(*ground, origin) > MOST_DISTANCE)
            {
                const glm::vec3 flat(forward.x, forward.y, 0.0f);
                ground = Actors::FindGround(origin + flat * STEPS_AHEAD, {0.0f, 0.0f, -1.0f});
            }
            if (!ground)
                continue;
            ground = FindRoomToStand(*ground, origin);
            actor->origin = std::array{ground->x, ground->y, ground->z + 2.0f};
            actor->yaw = glm::degrees(std::atan2(origin.y - ground->y, origin.x - ground->x));
            Components::LiveActors::Update(*actor);
        }
    }

    // The actor the view looks at, else the newest, goes. Main thread.
    void RunRemoveRequest()
    {
        if (!Components::LiveActors::TakeRemoveLooked())
            return;
        const auto actors = Components::LiveActors::GetActors();
        const auto cg = GetClientGlobals();
        if (actors.empty() || !cg)
            return;
        const auto& view = cg->refdef;
        const glm::vec3 origin(view.vieworg[0], view.vieworg[1], view.vieworg[2]);
        const glm::vec3 forward(view.viewaxis[0][0], view.viewaxis[0][1], view.viewaxis[0][2]);
        constexpr float WITHIN_TEN_DEGREES = 0.985f;
        constexpr float CHEST_HEIGHT = 40.0f;
        auto id = actors.back().id;
        auto best = WITHIN_TEN_DEGREES;
        for (const auto& actor : actors)
        {
            const auto state = Components::LiveActors::GetState(actor.id);
            if (!state.alive)
                continue;
            const glm::vec3 chest(state.origin[0], state.origin[1], state.origin[2] + CHEST_HEIGHT);
            const auto facing = glm::dot(forward, glm::normalize(chest - origin));
            if (facing > best)
            {
                best = facing;
                id = actor.id;
            }
        }
        Components::LiveActors::Remove(id);
    }

    // Each new gun's zones and whether the game has it. Main thread.
    void CheckWeapons(const std::vector<Types::LiveActor>& actors)
    {
        for (const auto& actor : actors)
        {
            if (actor.weapon.empty())
                continue;
            {
                std::lock_guard lock(stateMutex);
                if (weapons.contains(actor.weapon))
                    continue;
            }
            std::string problem;
            if (Components::WeaponCatalog::Get().Has(actor.weapon))
                WeaponZones::LoadForPlayer(actor.weapon, &problem);
            if (!WeaponRig::FindVariant(actor.weapon) && problem.empty())
                problem = "The game has no " + actor.weapon;
            std::lock_guard lock(stateMutex);
            weapons[actor.weapon] = problem;
        }
    }

    void RunFrame()
    {
        const auto state = Mod::GetGameInterface()->GetGameState();
        const bool hosting = state == Types::GameState::InGame && IsHosting();
        live = hosting;

        std::unique_lock lock(stateMutex);
        if (state == Types::GameState::InDemo)
        {
            // (a demo's load has reset what was built)
            ForgetSlots();
            Components::LiveActors::Clear();
            mapName.clear();
            return;
        }
        if (!hosting)
        {
            // their clients went with the server; they join again if the same map comes back
            for (auto& slot : slots)
                if (slot.number >= 0)
                    Forget(slot);
            return;
        }

        // the bots stay with their map; a game on another map starts without them (nothing is saved
        // of them). What the last demo's or game's map had goes too.
        const auto map = Functions::FindDvar("mapname");
        const std::string currentMap = map && map->current.string ? map->current.string : "";
        if (currentMap != mapName)
        {
            mapName = currentMap;
            ForgetSlots();
            Actors::ResetForMap();
            Hooks::Loadouts::ResetLive();
            Hooks::PlayerAnimation::ResetLive();
            Components::LiveActors::Clear();
            Components::Loadouts::ResetLoadouts();
            Components::PlayerAnimation::ResetAnimations();
        }
        lock.unlock();

        RunPlaceRequests();
        RunRemoveRequest();
        CheckWeapons(Components::LiveActors::GetActors());
    }

    // Adds what is missing and brings the actors up to what is chosen. Server thread, each frame.
    void RunServerFrame()
    {
        if (!live)
            return;
        std::unique_lock lock(stateMutex, std::try_to_lock);
        if (!lock)
            return;

        for (const auto& [number, menu, response] : hostAnswers)
            AnswerMenu(number, menu, response);
        hostAnswers.clear();

        const auto actors = Components::LiveActors::GetActors();
        if (actors.empty() && slots.empty())
            return;
        ApplySkill();

        const auto maxClients = Functions::FindDvar("sv_maxclients");
        const auto clientCount = maxClients ? maxClients->current.integer : 0;
        for (auto& slot : slots)
            if (std::ranges::find(actors, slot.id, &Types::LiveActor::id) == actors.end())
                Drop(slot);
        std::erase_if(slots, [&](const Slot& slot) {
            return std::ranges::find(actors, slot.id, &Types::LiveActor::id) == actors.end();
        });

        for (const auto& actor : actors)
        {
            auto slot = std::ranges::find(slots, actor.id, &Slot::id);
            if (slot == slots.end())
            {
                slots.push_back({.id = actor.id});
                slot = slots.end() - 1;
            }
            if (slot->number >= 0 && !IsTestClient(slot->number))
                Forget(*slot);
            if (slot->number < 0)
            {
                AddClient(*slot, clientCount);
                if (slot->number < 0)
                {
                    ReportState(*slot);
                    continue;
                }
            }

            clientModes[slot->number] = static_cast<std::int32_t>(actor.mode);
            if (slot->applied && Logic::NeedsTeam(*slot->applied, actor))
            {
                slot->teamChosen.reset();
                slot->classChosen.reset();
                slot->spawned = false;
            }
            RunJoin(*slot, actor, clientCount);
            RunLife(*slot, actor);
            ApplyName(*slot);
            slot->applied = actor;
            ReportState(*slot);
        }
        Components::LiveActors::SetRoom(CountRoom(clientCount));
    }

    bool IsLive()
    {
        return live;
    }

    std::int32_t GetHostHealth()
    {
        const auto cg = GetClientGlobals();
        return cg && cg->snap && IsLive() ? GetHealth(cg->snap->ps.clientNum) : 0;
    }

    void JoinHost(std::string_view menu, std::string_view response)
    {
        if (const auto cg = GetClientGlobals(); cg && cg->snap && IsLive())
        {
            std::lock_guard lock(stateMutex);
            hostAnswers.emplace_back(cg->snap->ps.clientNum, menu, response);
        }
    }

    bool IsActor(std::int32_t client)
    {
        return client >= 0 && static_cast<std::size_t>(client) < MAX_CLIENTS && clientModes[client] >= 0;
    }

    // Each server frame of a test client. A Still actor doesn't get to think: it keeps the view it was given (the cmd's
    // angles stay what the game set its view from) and sends nothing; the others send what the bot code made up, less
    // what their mode doesn't let through. A dead actor always thinks, which is what brings it back.
    typedef void(__cdecl* SV_BotUserMove_t)(std::uint8_t* client, usercmd_s* cmd);
    SV_BotUserMove_t SV_BotUserMove_Trampoline = nullptr;

    void __cdecl SV_BotUserMove_Hook(std::uint8_t* client, usercmd_s* cmd)
    {
        const auto& addresses = GetGameAddresses();
        const auto clients = *reinterpret_cast<std::uint8_t**>(addresses.svs_clients());
        const auto number = (client - clients) / static_cast<std::ptrdiff_t>(SERVER_CLIENT_SIZE);
        const auto chosen =
            number >= 0 && number < static_cast<std::ptrdiff_t>(MAX_CLIENTS) ? clientModes[number].load() : -1;
        if (chosen < 0)
        {
            SV_BotUserMove_Trampoline(client, cmd);
            return;
        }

        const auto mode = static_cast<Types::LiveActorMode>(chosen);
        const bool alive = GetHealth(static_cast<std::int32_t>(number)) > 0;
        if (Logic::RunsBotCode(mode, alive))
        {
            SV_BotUserMove_Trampoline(client, cmd);
        }
        else
        {
            std::memcpy(cmd, client + SERVER_CLIENT_LAST_CMD, sizeof(usercmd_s));
            cmd->serverTime = *reinterpret_cast<std::int32_t*>(addresses.svs_time());
            // (0 while a grenade leaves its hand: asked for, it would put its gun away for good)
            if (const auto ps = GetPlayerState(static_cast<std::int32_t>(number)); ps && ps->weapon != 0)
                cmd->weapon = ps->weapon;
        }
        const auto filtered = Logic::FilterInput(alive ? mode : Types::LiveActorMode::Roam,
                                                 {cmd->forward, cmd->right, cmd->buttons, cmd->buttons2});
        cmd->forward = filtered.forward;
        cmd->right = filtered.right;
        cmd->buttons = filtered.buttons;
        cmd->buttons2 = filtered.buttons2;
    }

    // (the player of this game has a share of that step too, see LiveGame)
    typedef void(__cdecl* SV_BotsThink_t)();
    SV_BotsThink_t SV_BotsThink_Trampoline = nullptr;

    void __cdecl SV_BotsThink_Hook()
    {
        RunServerFrame();
        LiveGame::RunServerFrame();
        SV_BotsThink_Trampoline();
    }

    void Install()
    {
        for (auto& mode : clientModes)
            mode = -1;
        HookManager::CreateHook(GetGameAddresses().SV_BotUserMove(), (std::uintptr_t)SV_BotUserMove_Hook,
                                (std::uintptr_t*)&SV_BotUserMove_Trampoline);
        HookManager::CreateHook(GetGameAddresses().SV_BotsThink(), (std::uintptr_t)SV_BotsThink_Hook,
                                (std::uintptr_t*)&SV_BotsThink_Trampoline);
    }
}  // namespace IWXMVM::T5::LiveActors
