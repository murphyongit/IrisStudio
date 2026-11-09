#include "StdInclude.hpp"
#include "PlayerAnimation.hpp"

#include <bitset>

#include "Components/ActorLogic.hpp"
#include "Components/PlayerAnimation.hpp"
#include "Events.hpp"
#include "Mod.hpp"
#include "Utilities/HookManager.hpp"
#include "../Actors.hpp"
#include "../Addresses.hpp"
#include "../AnimationPose.hpp"
#include "../AnimationStore.hpp"
#include "../Functions.hpp"
#include "Loadouts.hpp"
#include "../Patches.hpp"
#include "../Structures.hpp"

namespace IWXMVM::T5::Hooks::PlayerAnimation
{
    using namespace Structures;

    // Every frame CG_AddPacketEntity interpolates each entity of the snapshot, updates its model and draws it, all from
    // the entity's latest state. Changing the state before that changes how the corpse is drawn without touching the
    // demo. A new snapshot puts the game's own values back, which is why everything is applied again every frame.

    std::optional<std::string_view> GetAnimationName(const animScriptData_t& data, std::uint32_t anim)
    {
        const auto index = anim & ~static_cast<std::uint32_t>(ANIM_TOGGLEBIT);
        if (index >= data.numAnimations)
            return std::nullopt;

        const auto& name = data.animations[index].name;
        const auto length = strnlen(name, sizeof(name));
        if (length == 0 || length == sizeof(name))
            return std::nullopt;

        return std::string_view{name, length};
    }

    // The death animations are the body animations of mp/playeranim.script with "death" in their name. pf_death is
    // a facial animation.
    bool IsDeathAnimation(std::string_view name)
    {
        return name.starts_with("pb_") && name.find("death") != std::string_view::npos;
    }

    // Core's UI reads the list on the render thread, so it is only replaced when it actually changes, which takes a
    // demo with another playeranim.script
    std::optional<std::uint32_t> listedAnimationCount;

    void UpdateAnimationList(const animScriptData_t& data)
    {
        if (listedAnimationCount == data.numAnimations)
            return;

        listedAnimationCount = data.numAnimations;

        std::vector<std::pair<std::string_view, std::uint32_t>> anims;
        for (std::uint32_t i = 0; i < data.numAnimations; ++i)
        {
            if (const auto name = GetAnimationName(data, i); name && IsDeathAnimation(*name))
                anims.emplace_back(*name, i);
        }

        if (anims.empty())
            LOG_ERROR("Found no death animations in {} player animations", data.numAnimations);
        else if (anims != Components::PlayerAnimation::GetAnimations())
            Components::PlayerAnimation::PopulateAnimationData(anims);
    }

    // The animation the game gave a corpse, kept so that resetting puts it back while the demo is paused, when no
    // new snapshot arrives to do it
    struct CorpseAnimation
    {
        std::uint16_t original;
        std::uint16_t written;
    };
    std::array<std::optional<CorpseAnimation>, MAX_GENTITIES> corpseAnimations;

    struct DeathAnimation
    {
        bool newCorpse;  // the server gave the corpse another animation, so it's another player's corpse
        bool replaced;   // the corpse plays another animation than the one it died with
        std::uint32_t original;  // the animation it died with
        bool catalog = false;    // the one it plays is of the catalog, from a tree of its own (see CorpseTree)
    };

    // Any animation of the catalog as a corpse's death. legsAnim can only name the
    // animations of playeranim.script, which the game goes by for everything about the corpse but how it is posed. So
    // it gets another of the players' deaths, and the corpse's tree a copy of the players' animations in which that
    // entry is the chosen animation. The game then starts it and blends into it as into that death, and the corpse
    // moves along it as along a replaced death (see MoveCorpse). A second entry lines up the torso_stabilizer of the
    // body or its rig, as animations made for SP bodies need (see Actors.cpp). It plays out, held at its end, as an
    // actor's does: physics would take the body over early in it.
    struct CorpseTree
    {
        XAnim_s* players = nullptr;
        std::vector<std::uint8_t> anims;  // the copy
        std::string animation;
        std::uint32_t entry = 0, spineEntry = 0;
        bool hasSpine = false;
        AnimationStore::Reference stored;
    };
    std::map<XAnimTree_s*, CorpseTree> corpseTrees;

    inline constexpr std::size_t XANIM_HEADER_SIZE = 0x18;
    inline constexpr std::size_t XANIM_ENTRY_SIZE = 0x10;
    inline constexpr std::size_t XANIM_DEBUG_NAMES = 0x10;
    inline constexpr float SPINE_BLEND_SECONDS = 0.1f;

    using XAnimSetGoalWeight_t = void(__cdecl*)(DObj* obj, std::uint32_t index, float weight, float blendTime,
                                                float rate, std::uint32_t notifyName, std::uint32_t notifyType,
                                                std::int32_t restart, std::int32_t);

    void CopyEntry(CorpseTree& own, std::uint32_t index)
    {
        std::memcpy(own.anims.data() + XANIM_HEADER_SIZE + index * XANIM_ENTRY_SIZE,
                    reinterpret_cast<const std::uint8_t*>(own.players) + XANIM_HEADER_SIZE + index * XANIM_ENTRY_SIZE,
                    XANIM_ENTRY_SIZE);
    }

    // the corpse's tree back to the players' animations
    void ReleaseTree(DObj& obj)
    {
        const auto own = corpseTrees.find(obj.tree);
        if (own == corpseTrees.end() || own->second.animation.empty())
            return;
        if (own->second.hasSpine)
            reinterpret_cast<XAnimSetGoalWeight_t>(GetGameAddresses().XAnimSetGoalWeight())(
                &obj, own->second.spineEntry, 0.0f, 0.0f, 0.0f, 0, 0, 0, -1);
        CopyEntry(own->second, own->second.entry);
        CopyEntry(own->second, own->second.spineEntry);
        obj.tree->anims = own->second.players;
        own->second.animation.clear();
        own->second.hasSpine = false;
        own->second.stored = {};
    }

    // The entry of the players' animations the corpse plays a catalog animation in; none until it has a tree or for an
    // animation that isn't there
    std::optional<std::uint32_t> UseCatalogDeath(DObj* obj, const std::string& animation, std::uint32_t original)
    {
        if (!obj || !obj->tree || !obj->tree->anims)
            return std::nullopt;

        auto& own = corpseTrees[obj->tree];
        const auto copy = reinterpret_cast<XAnim_s*>(own.anims.data());
        if (!own.animation.empty() && obj->tree->anims != copy)
            own.animation.clear();  // the game gave the tree the players' animations again
        if (own.animation == animation && obj->tree->anims == copy)
            return own.entry;

        auto found = AnimationStore::Find(animation);
        if (!found.parts)
            return std::nullopt;

        if (obj->tree->anims != copy)
        {
            own.players = obj->tree->anims;
            const auto bytes = XANIM_HEADER_SIZE + own.players->size * XANIM_ENTRY_SIZE;
            own.anims.resize(bytes);
            std::memcpy(own.anims.data(), own.players, bytes);
            // (the names are the game's, for its debugging)
            std::memset(own.anims.data() + XANIM_DEBUG_NAMES, 0, sizeof(void*));
        }
        else
        {
            CopyEntry(own, own.entry);
            CopyEntry(own, own.spineEntry);
        }

        // two of the other deaths, not the one played until now: the game starts an animation for a change of entry
        const auto& deaths = Components::PlayerAnimation::GetAnimations();
        std::vector<std::uint32_t> spare;
        for (const auto& [name, index] : deaths)
            if (index != original && index != own.entry && spare.size() < 2)
                spare.push_back(index);
        if (spare.size() < 2)
            return std::nullopt;

        const auto anims = reinterpret_cast<XAnim_s*>(own.anims.data());
        own.entry = spare[0];
        own.spineEntry = spare[1];
        Actors::SetAnimationEntry(anims, own.entry, found.parts);
        const auto spine = Actors::GetLinedSpine(*obj, *found.parts);
        if (spine)
            Actors::SetAnimationEntry(anims, own.spineEntry, spine);
        own.hasSpine = spine != nullptr;
        own.animation = animation;
        own.stored = std::move(found.stored);
        obj->tree->anims = anims;
        return own.entry;
    }

    DeathAnimation UpdateDeathAnimation(const animScriptData_t& data, entityState_s& es)
    {
        auto& corpse = corpseAnimations[es.number];
        bool newCorpse = !corpse;
        if (!corpse || es.legsAnim != corpse->written)
        {
            newCorpse = newCorpse || es.legsAnim != corpse->original;
            corpse = CorpseAnimation{es.legsAnim, es.legsAnim};
        }

        const std::uint32_t original = corpse->original & ~ANIM_TOGGLEBIT;
        const auto name = GetAnimationName(data, corpse->original);
        if (!name || name->find("death") == std::string_view::npos)
            return {newCorpse, false, original};

        // keeps the original animation while no animation is selected
        std::uint32_t anim = corpse->original;
        Components::PlayerAnimation::ApplyAnimation(es.clientNum, *name, anim);

        const auto obj = Functions::Com_GetClientDObj(es.number, 0);
        const auto catalog = Components::PlayerAnimation::GetCatalogAnimation(es.clientNum);
        const auto entry = catalog.empty() ? std::nullopt : UseCatalogDeath(obj, catalog, original);
        if (entry)
        {
            anim = *entry;
            if (const auto own = corpseTrees.find(obj->tree); own->second.hasSpine)
                reinterpret_cast<XAnimSetGoalWeight_t>(GetGameAddresses().XAnimSetGoalWeight())(
                    obj, own->second.spineEntry, 1.0f, SPINE_BLEND_SECONDS, 1.0f, 0, 0, 0, -1);
        }
        else if (obj && obj->tree)
        {
            ReleaseTree(*obj);
        }
        corpse->written = es.legsAnim = static_cast<std::uint16_t>(anim);

        return {newCorpse, (corpse->written & ~ANIM_TOGGLEBIT) != original, original, entry.has_value()};
    }

    // The server moves a corpse along the death animation it died with, so with another animation the body would
    // still go wherever the original one took it. Corpses with a replaced animation are moved along that animation
    // instead, from where their death started, kept out of walls and on the floor (see PlaceOnFloor).
    struct CorpseMotion
    {
        std::int32_t lastSeen;                         // cg.time it was last drawn at
        float start[3];                                // where its death started (see DeathStart)
        float yaw;                                     // which way it was facing then
        float serverPosition[3];                       // the latest position from the server
        std::optional<std::array<float, 3>> moved;     // where it was moved to the last time
    };
    std::array<std::optional<CorpseMotion>, MAX_GENTITIES> corpseMotions;

    std::optional<std::array<float, 2>> GetAnimationPosition(const centity_t& cent, const CorpseMotion& motion)
    {
        const auto obj = Functions::Com_GetClientDObj(cent.nextState.number, 0);
        if (!obj || !obj->tree || !obj->tree->anims)
            return std::nullopt;

        // legsAnim indexes the animations of playeranim.script, which are the animations of the corpse's tree
        const std::uint32_t anim = cent.nextState.legsAnim & ~ANIM_TOGGLEBIT;
        const float time = Functions::XAnimGetTime(obj->tree, anim);

        float rot[2]{};
        float trans[3]{};
        Functions::XAnimGetRelDelta(obj->tree->anims, anim, rot, trans, 0.0f, time);

        const float yaw = glm::radians(motion.yaw);
        return std::array{
            motion.start[0] + std::cos(yaw) * trans[0] - std::sin(yaw) * trans[1],
            motion.start[1] + std::sin(yaw) * trans[0] + std::cos(yaw) * trans[1],
        };
    }

    // Further than a death animation moves a body, even over the seconds a skip forward jumps past. It takes another
    // corpse taking the place of the one that was there.
    constexpr float CORPSE_JUMP_DISTANCE = 200.0f;

    // Where the deaths seen started, by corpse entity. A rewind brings corpses back partway through their death, and
    // the game plays their animation again from the start, wherever the server has moved them to by then. Moving a
    // body along the new animation from there would take it further than before the rewind, into walls and cars, so
    // a death seen before starts where it did then.
    struct DeathStart
    {
        std::int32_t client;
        std::uint32_t animation;  // the one it died with
        float yaw;                // the same to the bit for the same death
        float start[3];
    };
    std::array<std::optional<DeathStart>, MAX_GENTITIES> deathStarts;

    void FindStart(const entityState_s& es, CorpseMotion& motion, std::uint32_t originalAnimation)
    {
        auto& known = deathStarts[es.number];
        if (known && known->client == es.clientNum && known->animation == originalAnimation &&
            known->yaw == motion.yaw &&
            std::hypot(known->start[0] - motion.start[0], known->start[1] - motion.start[1]) < CORPSE_JUMP_DISTANCE)
        {
            std::ranges::copy(known->start, motion.start);
            return;
        }
        known = DeathStart{es.clientNum, originalAnimation, motion.yaw,
                           {motion.start[0], motion.start[1], motion.start[2]}};
    }

    // sets the position the game interpolates the corpse from and to
    void SetCorpsePosition(centity_t& cent, const std::array<float, 3>& position)
    {
        for (int i = 0; i < 3; ++i)
            cent.currentState.pos.trBase[i] = cent.nextState.pos.trBase[i] = position[i];
    }

    // A corpse's origin is at its feet. The floor is looked for from a step above it to well below it.
    constexpr float FLOOR_SEARCH_ABOVE = 24.0f;
    constexpr float FLOOR_SEARCH_BELOW = 256.0f;

    std::optional<float> GetFloorHeight(const std::array<float, 3>& position, std::int32_t corpse)
    {
        const float start[3] = {position[0], position[1], position[2] + FLOOR_SEARCH_ABOVE};
        const float end[3] = {position[0], position[1], position[2] - FLOOR_SEARCH_BELOW};
        trace_t trace{};
        Functions::CG_LocationalTrace(&trace, start, end, corpse, MASK_SHOT_NO_CHARACTERS);
        if (trace.hitType == 0 || trace.fraction >= 1.0f)
            return std::nullopt;
        return start[2] + (end[2] - start[2]) * trace.fraction;
    }

    // how far a body stays off a wall it slides into, and the height it slides at, above a step
    constexpr float WALL_DISTANCE = 12.0f;
    constexpr float WALL_TRACE_HEIGHT = 24.0f;

    // The server keeps a corpse on the floor along the path of the animation it died with, so the height it gives is
    // the floor's under that path. A body moved along another animation would float or sink wherever the floor
    // under it is higher or lower, as on stairs and slopes, and slide through walls. It's stopped at the first wall
    // between where it showed up and where the animation takes it, and its height changes by how much higher the
    // floor is there than under the server's position, which keeps the server's height while the body falls.
    std::array<float, 3> PlaceOnFloor(const CorpseMotion& motion, std::array<float, 2> position, std::int32_t corpse)
    {
        const float wallStart[3] = {motion.start[0], motion.start[1], motion.start[2] + WALL_TRACE_HEIGHT};
        const float wallEnd[3] = {position[0], position[1], motion.start[2] + WALL_TRACE_HEIGHT};
        trace_t wall{};
        Functions::CG_LocationalTrace(&wall, wallStart, wallEnd, corpse, MASK_SHOT_NO_CHARACTERS);
        if (wall.hitType != 0 && wall.fraction < 1.0f)
        {
            const float length = std::hypot(wallEnd[0] - wallStart[0], wallEnd[1] - wallStart[1]);
            const float fraction = length > 0.0f ? std::max(0.0f, wall.fraction - WALL_DISTANCE / length) : 0.0f;
            for (int i = 0; i < 2; ++i)
                position[i] = wallStart[i] + (wallEnd[i] - wallStart[i]) * fraction;
        }

        const std::array server{motion.serverPosition[0], motion.serverPosition[1], motion.serverPosition[2]};
        std::array moved{position[0], position[1], server[2]};
        const auto serverFloor = GetFloorHeight(server, corpse);
        const auto movedFloor = serverFloor ? GetFloorHeight(moved, corpse) : std::nullopt;
        if (serverFloor && movedFloor)
            moved[2] += *movedFloor - *serverFloor;
        return moved;
    }

    // returns whether this is a corpse that wasn't there before
    bool MoveCorpse(centity_t& cent, const DeathAnimation& animation)
    {
        const auto& es = cent.nextState;
        const auto time = GetClientGlobals()->time;
        auto& motion = corpseMotions[es.number];

        // what isn't the position written last frame was put there by a new snapshot
        const bool fromServer = !motion || !motion->moved || (*motion->moved)[0] != es.pos.trBase[0] ||
                                (*motion->moved)[1] != es.pos.trBase[1] || (*motion->moved)[2] != es.pos.trBase[2];

        // time going back is a rewind, after which the corpses show up again
        if (motion && (animation.newCorpse || time < motion->lastSeen ||
                       (fromServer && std::hypot(es.pos.trBase[0] - motion->serverPosition[0],
                                                 es.pos.trBase[1] - motion->serverPosition[1]) > CORPSE_JUMP_DISTANCE)))
        {
            motion.reset();
        }

        const bool newCorpse = !motion;
        if (newCorpse)
        {
            motion = CorpseMotion{.start = {es.pos.trBase[0], es.pos.trBase[1], es.pos.trBase[2]},
                                  .yaw = es.apos.trBase[1],
                                  .serverPosition = {es.pos.trBase[0], es.pos.trBase[1], es.pos.trBase[2]}};
            FindStart(es, *motion, animation.original);
        }
        motion->lastSeen = time;

        if (fromServer)
        {
            for (int i = 0; i < 3; ++i)
                motion->serverPosition[i] = es.pos.trBase[i];
        }

        if (!animation.replaced)
        {
            // back to the server's position, which a paused demo wouldn't bring back by itself
            if (motion->moved)
            {
                SetCorpsePosition(cent, {motion->serverPosition[0], motion->serverPosition[1],
                                         motion->serverPosition[2]});
                motion->moved.reset();
            }
            return newCorpse;
        }

        // (physics doesn't take a catalog animation's body over, see CorpseTree)
        if (animation.catalog)
            cent.currentState.pos.trType = cent.nextState.pos.trType = TR_STATIONARY;

        // Once the body is a ragdoll, physics moves it. It stays where the animation had taken it, and one that was
        // already a ragdoll when its animation was replaced isn't moved at all.
        if (Functions::BG_IsRagdollTrajectory(&cent.currentState.pos))
        {
            if (motion->moved)
                SetCorpsePosition(cent, *motion->moved);
            return newCorpse;
        }

        if (const auto position = GetAnimationPosition(cent, *motion))
        {
            const auto placed = PlaceOnFloor(*motion, *position, es.number);
            SetCorpsePosition(cent, placed);
            motion->moved = placed;
        }
        return newCorpse;
    }

    std::bitset<MAX_GENTITIES> hiddenEntities;

    void SetHidden(entityState_s& es, bool hide)
    {
        if (hide)
        {
            es.eFlags |= EF_NODRAW;
            hiddenEntities.set(es.number);
        }
        else if (hiddenEntities.test(es.number))
        {
            es.eFlags &= ~EF_NODRAW;
            hiddenEntities.reset(es.number);
        }
    }

    // the last weapon each player held, by clientNum
    std::array<std::uint16_t, 256> playerWeapons{};

    // The weapon a corpse gets, which is the one its player held when the corpse showed up; the player may hold
    // another one after respawning. partBits are the parts the game hides on it, which are the attachments the weapon
    // doesn't have. It works them out whenever the parts in the entity state don't match the model's, which every new
    // snapshot would cause by putting the server's (none) back, so they are put back from here instead.
    struct CorpseWeapon
    {
        std::uint16_t weapon;
        std::optional<std::array<std::uint32_t, 5>> partBits;
    };
    std::array<std::optional<CorpseWeapon>, MAX_GENTITIES> corpseWeapons;

    // T5 hands corpses over to ragdoll physics early in their death animation, and the weapon's bone isn't part of
    // the ragdoll, so the weapon would stay behind wherever the animation left it. While weapons are attached, corpses
    // are animated all the way instead, the same as with ragdoll_enable 0 (which is saved to the config, so it isn't
    // touched).
    void AttachWeaponsToCorpses(bool attach)
    {
        auto& patches = Patches::GetGamePatches();
        if (attach)
        {
            patches.CG_CorpseWeapon.Apply();
            patches.BG_IsRagdollTrajectory.Apply();
        }
        else
        {
            patches.CG_CorpseWeapon.Revert();
            patches.BG_IsRagdollTrajectory.Revert();
        }
    }

    // The left gun of a dual wield weapon hangs from tag_weapon_left, which the death animations either leave at its
    // rest position (a hand's length off the left wrist, floating next to the body) or move along with a weapon that
    // isn't there. With weapons attached the bone is held where the dual wield animations hold it instead
    // (ActorLogic::LEFT_GUN_GRIP_POSITION), relative to j_wrist_le, its parent. That's done the way the game aims a
    // player's upper body: an offset set in CG_DoControllers, which can run on any thread that works out bones, so it
    // only reads what is set here.
    struct LeftGunGrip
    {
        std::atomic<std::int32_t> bone = -1;  // -1 while the bone is left alone; set after trans
        float trans[3]{};                     // from the bone's rest position to the grip
        const DObj* obj = nullptr;            // main thread only, the model the bone was looked up in
    };
    std::array<LeftGunGrip, MAX_GENTITIES> leftGunGrips;

    // the script string of tag_weapon_left, which belongs to the map's script strings
    std::optional<std::uint16_t> leftGunTag;

    void ReleaseLeftGun(std::int32_t number)
    {
        leftGunGrips[number].bone = -1;
        leftGunGrips[number].obj = nullptr;
    }

    void GripLeftGun(std::int32_t number)
    {
        auto& grip = leftGunGrips[number];
        const auto obj = Functions::Com_GetClientDObj(number, 0);
        if (obj == grip.obj && grip.bone >= 0)
            return;

        ReleaseLeftGun(number);
        if (!obj)
            return;

        if (!leftGunTag)
            leftGunTag = Functions::SL_GetString("tag_weapon_left");

        const auto bone = Functions::DObjGetBoneIndex(obj, *leftGunTag);
        const auto rest = bone ? AnimationPose::GetRestPosition(*obj, *bone) : std::nullopt;
        if (!rest)
            return;

        for (int i = 0; i < 3; ++i)
            grip.trans[i] = ActorLogic::LEFT_GUN_GRIP_POSITION[i] - (*rest)[i];
        grip.obj = obj;
        grip.bone = *bone;
    }

    typedef void(__cdecl* DObjCalcAnim_t)(DObj* obj, int* partBits);
    DObjCalcAnim_t DObjCalcAnim_Trampoline;
    void __cdecl DObjCalcAnim_Hook(DObj* obj, int* partBits)
    {
        std::array<std::uint32_t, 5> worked;
        std::copy(std::begin(obj->animPartBits), std::end(obj->animPartBits), worked.begin());
        DObjCalcAnim_Trampoline(obj, partBits);
        // actors' heads that look at something, turned from where the animations put them
        Actors::BlendHeads(obj, partBits, worked);
    }

    typedef void(__cdecl* DObjCalcSkel_t)(DObj* obj, int* partBits);
    DObjCalcSkel_t DObjCalcSkel_Trampoline;
    void __cdecl DObjCalcSkel_Hook(DObj* obj, int* partBits)
    {
        std::array<std::uint32_t, 5> composed;
        std::copy(std::begin(obj->calcSkelBits), std::end(obj->calcSkelBits), composed.begin());
        DObjCalcSkel_Trampoline(obj, partBits);
        // actors' left hands on their guns, from where the animations put them
        Actors::GripGuns(obj, composed);
    }

    typedef void(__cdecl* CG_DoControllers_t)(const cpose_t* pose, DObj* obj, int* partBits);
    CG_DoControllers_t CG_DoControllers_Trampoline;
    void __cdecl CG_DoControllers_Hook(const cpose_t* pose, DObj* obj, int* partBits)
    {
        Actors::AskGripBones(obj, partBits);
        CG_DoControllers_Trampoline(pose, obj, partBits);
        // the bones actors' controllers turn: heads looking at something, killstreaks' bodies and wheels
        Actors::ControlJoints(obj, partBits);

        // corpses have no offsets of their own; an entity's pose is the start of its centity_t
        const auto entities = GetEntities();
        if (!pose || pose->eType != ET_PLAYER_CORPSE || !entities)
            return;

        const auto number = reinterpret_cast<const centity_t*>(pose) - entities;
        if (number < 0 || number >= static_cast<std::ptrdiff_t>(MAX_GENTITIES))
            return;

        const auto& grip = leftGunGrips[number];
        if (const auto bone = grip.bone.load(); bone >= 0)
        {
            const auto& gripAngles = ActorLogic::LEFT_GUN_GRIP_ANGLES;
            const float angles[3] = {gripAngles.x, gripAngles.y, gripAngles.z};
            Functions::DObjSetLocalTag(obj, partBits, bone, grip.trans, angles);
        }
    }

    void AttachWeapon(entityState_s& es)
    {
        const auto& corpseWeapon = corpseWeapons[es.number];
        if (!corpseWeapon)
            return;

        // The model code takes the gun from primaryWeapon and the parts to hide from weapon, which the server
        // clears for corpses. Without it the gun would show every attachment it can have.
        es.primaryWeapon = es.weapon = corpseWeapon->weapon;

        if (corpseWeapon->partBits)
            std::ranges::copy(*corpseWeapon->partBits, es.partBits);
    }

    void ProcessEntity(centity_t& cent)
    {
        auto& es = cent.nextState;
        const bool inDemo = Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
        if (!inDemo)
        {
            AttachWeaponsToCorpses(false);
            return;
        }

        auto cg = GetClientGlobals();
        if (!cg || !cg->animScriptData || cg->animScriptData->numAnimations > MAX_ANIMSCRIPT_ANIMATIONS ||
            static_cast<std::uint32_t>(es.number) >= MAX_GENTITIES)
            return;

        const auto& data = *cg->animScriptData;
        UpdateAnimationList(data);

        // the demo's own player isn't one of the entities
        if (cg->snap && cg->snap->ps.weapon != 0)
            playerWeapons[cg->snap->ps.clientNum] = cg->snap->ps.weapon;

        const bool attachWeapon = inDemo && Components::PlayerAnimation::AttachWeaponToCorpse();
        AttachWeaponsToCorpses(attachWeapon);

        // players and corpses have the player animations and a body for previewing them, but for a corpse's own
        const auto source = es.eType == ET_PLAYER || es.eType == ET_PLAYER_CORPSE
                                ? Functions::Com_GetClientDObj(es.number, 0)
                                : nullptr;
        if (source && (!source->tree || !corpseTrees.contains(source->tree) ||
                       corpseTrees[source->tree].animation.empty()))
            AnimationPose::SetSource(source);

        switch (es.eType)
        {
            case ET_PLAYER:
                if (es.primaryWeapon != 0)
                    playerWeapons[es.clientNum] = es.primaryWeapon;
                break;
            case ET_PLAYER_CORPSE:
            {
                if (MoveCorpse(cent, UpdateDeathAnimation(data, es)))
                {
                    corpseWeapons[es.number].reset();
                    ReleaseLeftGun(es.number);
                }

                if (!corpseWeapons[es.number] && playerWeapons[es.clientNum] != 0)
                    corpseWeapons[es.number] = CorpseWeapon{playerWeapons[es.clientNum]};

                if (attachWeapon)
                    AttachWeapon(es);

                SetHidden(es, inDemo && Components::PlayerAnimation::HideAllCorpses());
                break;
            }
            case ET_ITEM:
                // the weapon the player dropped, which is in the corpse's hands now
                SetHidden(es, attachWeapon);
                break;
            default:
                break;
        }
    }

    // after the game has updated the entity's model
    void ProcessEntityModel(const centity_t& cent)
    {
        const auto& es = cent.nextState;
        if (es.eType != ET_PLAYER_CORPSE || static_cast<std::uint32_t>(es.number) >= MAX_GENTITIES)
            return;

        auto& corpseWeapon = corpseWeapons[es.number];
        if (Components::PlayerAnimation::AttachWeaponToCorpse() && corpseWeapon && es.weapon == corpseWeapon->weapon)
        {
            corpseWeapon->partBits.emplace();
            std::ranges::copy(es.partBits, corpseWeapon->partBits->begin());

            GripLeftGun(es.number);
        }
        else
        {
            ReleaseLeftGun(es.number);
        }
    }

    typedef void(__cdecl* CG_AddPacketEntity_t)(int localClientNum, int entityNum);
    CG_AddPacketEntity_t CG_AddPacketEntity_Trampoline;
    void __cdecl CG_AddPacketEntity_Hook(int localClientNum, int entityNum)
    {
        // numbers from MAX_GENTITIES up are the client's own entities, which are kept elsewhere
        const auto entities = GetEntities();
        const auto cent =
            entities && static_cast<std::uint32_t>(entityNum) < MAX_GENTITIES ? &entities[entityNum] : nullptr;

        if (cent)
        {
            ProcessEntity(*cent);
            Loadouts::ProcessEntity(*cent);
        }

        CG_AddPacketEntity_Trampoline(localClientNum, entityNum);

        if (cent)
        {
            Loadouts::ProcessEntityModel(*cent);
            ProcessEntityModel(*cent);
        }
    }

    std::string GetCatalogDeath(std::int32_t number)
    {
        const auto obj = Functions::Com_GetClientDObj(number, 0);
        const auto own = obj ? corpseTrees.find(obj->tree) : corpseTrees.end();
        return own != corpseTrees.end() ? own->second.animation : std::string();
    }

    bool ikTimesForgotten = true;

    void ForgetIkTimes()
    {
        // The game's IK keeps, for each player, when its gun last changed and when it last moved and stood still: the
        // left hand goes from the old gun's grip to the new over the 100 ms after a change, and the feet are planted
        // once the player has stood still for 200 ms. Its time only goes forward, so a rewind past a change left the
        // change ahead of the demo, and the hand held the old gun's grip (a claymore has none) until the demo caught up
        // with it. The states are kept from one film to the next too, with the last film's times, which can be far ahead
        // of the next one's. 0 is what the game starts with: the gun in hand is taken at once.
        const auto states = Structures::GetIKStates();
        if (!ikTimesForgotten || !states)
            return;
        for (std::size_t i = 0; i < MAX_CLIENTS; ++i)
        {
            states[i].gripChangeTime = 0;
            states[i].lastMoveTime = 0;
            states[i].lastStillTime = 0;
        }
    }

    void SetIkTimesForgotten(bool on)
    {
        ikTimesForgotten = on;
    }

    void ResetLive()
    {
        AnimationPose::Reset();
        corpseTrees.clear();
        listedAnimationCount.reset();
        corpseAnimations = {};
        corpseMotions = {};
        deathStarts = {};
        corpseWeapons = {};
        for (std::size_t i = 0; i < MAX_GENTITIES; ++i)
            ReleaseLeftGun(static_cast<std::int32_t>(i));
        leftGunTag.reset();
        hiddenEntities.reset();
        playerWeapons = {};
        ForgetIkTimes();
    }

    void Install()
    {
        HookManager::CreateHook(GetGameAddresses().CG_AddPacketEntity(), (std::uintptr_t)CG_AddPacketEntity_Hook,
                                (std::uintptr_t*)&CG_AddPacketEntity_Trampoline);
        HookManager::CreateHook(GetGameAddresses().CG_DoControllers(), (std::uintptr_t)CG_DoControllers_Hook,
                                (std::uintptr_t*)&CG_DoControllers_Trampoline);
        HookManager::CreateHook(GetGameAddresses().DObjCalcAnim(), (std::uintptr_t)DObjCalcAnim_Hook,
                                (std::uintptr_t*)&DObjCalcAnim_Trampoline);
        HookManager::CreateHook(GetGameAddresses().DObjCalcSkel(), (std::uintptr_t)DObjCalcSkel_Hook,
                                (std::uintptr_t*)&DObjCalcSkel_Trampoline);

        // the animations and the body are assets of the map, which goes away when another demo loads
        Events::RegisterListener(EventType::PreDemoLoad, [] {
            AnimationPose::Reset();
            corpseTrees.clear();
        });

        // another demo can come with another playeranim.script
        Events::RegisterListener(EventType::PostDemoLoad, ResetLive);
    }
}  // namespace IWXMVM::T5::Hooks::PlayerAnimation

