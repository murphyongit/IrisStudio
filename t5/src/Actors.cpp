#include "StdInclude.hpp"
#include "Actors.hpp"

#include <set>

#include "Addresses.hpp"
#include "AnimationPose.hpp"
#include "AnimationStore.hpp"
#include "ContentPacks.hpp"
#include "ContentPacksInternal.hpp"
#include "Events.hpp"
#include "Functions.hpp"
#include "Mod.hpp"
#include "Structures.hpp"
#include "Bones.hpp"
#include "Lights.hpp"
#include "CampaignMaps.hpp"
#include "Hooks/Camera.hpp"
#include "Hooks/Effects.hpp"
#include "WeaponRig.hpp"
#include "Components/Actors.hpp"
#include "Components/CameraManager.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/DropLogic.hpp"
#include "Components/GripLogic.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/Loadouts.hpp"
#include "Components/Playback.hpp"
#include "Components/Props.hpp"
#include "Components/ReloadLogic.hpp"
#include "Components/ReloadProfiles.hpp"
#include "Components/ShotLogic.hpp"
#include "Components/WeaponCatalog.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::T5::Actors
{
    using namespace Structures;
    namespace ShotLogic = Components::ShotLogic;

    // An actor is drawn the way the client draws its own script models: a DObj of the models with an animation tree
    // of its own, handed to the renderer with a pose (where it stands) once a frame, after the entities. The tree has
    // a root blending an entry for each of the actor's clips, the animations its legs may play (the idles under upper
    // body clips, and with locomotion its walks and runs) and those its face may play, whose weights and times are set
    // every frame from what core works out for the timeline's tick (one clip, or two while one blends in, the legs
    // under upper body clips or of its locomotion, and the face: see ActorLogic::GetLayers).
    //
    // DObjs are found by handle. The client's own entities (those of its scripts) use 0x400 to 0x5FF from the bottom,
    // so actors take handles from the top, and the renderer gets the handle as the entity number. An actor is drawn
    // with the pose of the client entity of its handle, which is where the game looks for it, e.g. for the effects
    // bolted to its bones.
    inline constexpr std::int32_t LAST_HANDLE = 0x5FF;
    inline constexpr std::size_t MAX_ACTORS = 64;
    // the handles actors may take, from the top: their own, and those of their magazines, props and the like
    inline constexpr std::int32_t MAX_HANDLES = static_cast<std::int32_t>(MAX_ACTORS) * 6;
    // of an actor's reloads, the magazines left on the ground that are drawn (the latest)
    inline constexpr std::size_t MAX_DROPPED_MAGAZINES = 3;
    inline constexpr std::uint32_t ROOT_INDEX = 0;
    inline constexpr std::uint32_t FIRST_CLIP_INDEX = 1;
    inline constexpr std::int32_t ENTITYNUM_NONE = 0x3FF;
    inline constexpr std::int32_t ENTITYNUM_WORLD = 0x3FE;
    // where a cpose_t keeps what CG_GetWeaponMaterialOverride works out for the renderer (a centity_t's)
    inline constexpr std::size_t POSE_SHADER_DATA = 0xFC;
    inline constexpr std::int32_t FIRST_CLIENT_ENTITY = 0x400;
    inline constexpr std::size_t CLIENT_ENTITY_SIZE = 0x32C;
    inline constexpr std::size_t CLIENT_ENTITY_POSE = 4;
    // the flag of an entity's flags (after its pose) the game sets while the entity is in use, e.g. by the client's
    // scripts; effects are only bolted to entities that have it
    inline constexpr std::uint32_t CENTITY_FLAG_HAS_MODEL = 2;

    // How an actor's head turns to look at something. Each model of a character has its own copy of the head's bone
    // (the body's hangs from a helper bone, the head model's from j_neck), and each model's mesh follows its own, so
    // all of them are turned, each relative to the bone it hangs from. Which way the face looks and which way is up,
    // in the head's own axes, come from the rest pose of the first model that has the whole chain down to it, which
    // faces along x.
    struct HeadRig
    {
        struct Copy
        {
            std::uint8_t bone = 0;        // its index in the DObj
            std::uint16_t parentTag = 0;  // the bone it hangs from, a script string
        };
        std::vector<Copy> copies;
        glm::vec3 offset{};  // where the head is from the first copy's parent, in its axes
        glm::vec3 forward{1, 0, 0};
        glm::vec3 up{0, 0, 1};
    };
    inline constexpr std::size_t MAX_HEAD_COPIES = 4;

    // An MP body's spine under animations made for SP characters (AI and Zombies ones) has torso_stabilizer lined up
    // (see PoseLogic::SpineKind). That is done by animating it: the actor's tree gets an animation made up here of the
    // stabilizer alone, lined up (see AnimationPose::SpineRig), weighed as much as the clips for SP bodies weigh, and
    // one of it at rest for the clips that don't move it either (MP upper body ones). The tree blends those with the
    // stabilizer of the clips for MP bodies, so blends between clips of either kind come out right. (CG_DoControllers
    // can't do it: a bone it sets is set instead of animated, and what the animations make of it isn't worked out by
    // then.) The decoder of animations lines it up the same way (AnimationPose::GetPose).
    using AnimationPose::SpineRig;
    using SpineKind = PoseLogic::SpineKind;

    // An animation of torso_stabilizer alone, held as a rotation and an offset; XAnimParts as the zones have them,
    // with one bone of a constant full quaternion and a constant translation
    struct SpineAnimation
    {
        XAnimParts parts{};
        std::uint16_t name = 0;
        std::uint8_t bone = 0;  // of the translations, which bone of names
        std::array<std::int16_t, 4> rotation{};
        std::array<float, 3> offset{};
        XAnimNotifyInfo end{};  // for a player's corpse (see GetLinedSpine)
    };

    // A magazine that fell, placed in the world (see ReloadLogic::PlaceFall) for the actor as it was then: kept for as
    // long as what it is placed from stays the same, so the ground isn't looked for every frame
    struct PlacedFall
    {
        std::uint32_t generation = 0;  // of core's actors
        std::uint32_t clip = 0;        // the reload's clip's id and its tick out
        std::uint32_t out = 0;
        std::shared_ptr<const Types::ReloadProfile> profile;
        glm::mat4 placement{1.0f};  // the actor's as it started to fall
        float speed = 0.0f;         // the clip's
        std::optional<ActorLogic::MagazineFall> fall;
    };

    // what the game holds for one of core's actors
    struct Built
    {
        bool active = false;
        std::uint32_t id = 0;
        std::vector<std::string> models;
        bool spineRig = false;               // its DObj's first model is its body's spine rig (see GetSpineRig)
        std::vector<std::uint32_t> clipIds;  // the tree's entries after the root, one for each clip
        std::vector<std::string> clipAnimations;
        std::vector<const XAnimParts*> clipParts;
        // The entries after the clips': the legs' idles it may stand in under its clips of upper body animations,
        // for each stance as its gun stands, and with locomotion its walks and runs (see ActorLogic::GetLegsLibrary),
        // so the tree isn't made again as they change. None for an actor without such clips. The library as core
        // wants it (the game may lack some), to make the tree again when that changes.
        std::vector<std::string> legsWanted;
        std::vector<std::string> legsAnimations;
        std::vector<const XAnimParts*> legsParts;
        // The entries after the legs': the animations its face may play (see ActorLogic::GetFaceLibrary), for models
        // with a face (see HasFace); the library as core wants it, to make the tree again when that changes
        std::vector<std::string> faceWanted;
        std::vector<std::string> faceAnimations;
        std::vector<const XAnimParts*> faceParts;
        // what keeps the animations of the store its tree has loaded (see AnimationStore)
        std::vector<AnimationStore::Reference> stored;
        Types::GunClass gunClass = Types::GunClass::Rifle;
        // what its legs stand and walk as (a person with that gun, a dog: see ActorLogic::Body)
        ActorLogic::Body body;
        // what the clips' and the legs' animations do with an MP body's torso_stabilizer
        std::map<std::string, SpineKind, std::less<>> spines;
        std::string weapon;  // the variant in its hand, empty for none
        const WeaponVariantDef* variant = nullptr;
        std::uint32_t weaponModel = 0;  // the index of the variant's world model in the DObj
        // a dual wield gun's left one, on tag_weapon_left (see ActorLogic::GripsLeftGun): its variant (aspdwlh_mp for
        // aspdw_mp) and the index of its world model in the DObj
        const WeaponVariantDef* leftVariant = nullptr;
        std::uint32_t leftGunModel = 0;
        // what fires by a loop (the Death Machine): its alias' hash while it plays, where it sounds, and the tick the
        // burst ends
        std::uint32_t fireLoop = 0;
        std::array<float, 3> fireLoopAt{};
        std::uint32_t fireLoopEnd = 0;
        // the gun on its back, on tag_stowed_back: the variant (empty name for none), and the index of its model in the
        // DObj, with what a body without that bone hangs it from before it (see GetStowedRig)
        std::string stowed;
        const WeaponVariantDef* stowedVariant = nullptr;
        std::uint32_t stowedModel = 0;
        std::int32_t handle = 0;
        DObj* obj = nullptr;
        XAnim_s* anims = nullptr;
        XAnimTree_s* tree = nullptr;
        std::vector<float> weights;                 // the goal weight each entry after the root was last given
        std::optional<std::uint32_t> animatedTick;  // the tick its tree was last animated for
        bool posed = false;  // shown and placed in the frame now (see PoseActors)

        // Reloads, as core's reload profiles have them (see ReloadLogic::GetMagazines). While the magazine is off the
        // gun, the actor is drawn from a second DObj of the same models and tree, made with the gun's magazine bone
        // hidden: a bone hidden after a DObj was first drawn is drawn anyway. The magazines in the hand and on the
        // ground are DObjs of their own of a copy of the gun model that only draws the magazine (see
        // GetMagazineModel). Each is built when first needed.
        std::uint16_t magazineTag = 0;  // the gun's magazine bone (tag_clip or a variant's bigger one); 0 for none
        std::optional<Types::WeaponRig> rig;  // where that is in the gun model, as the profiles have it
        std::int32_t unloadedHandle = 0;
        DObj* unloadedObj = nullptr;
        bool magazineOffGun = false;  // the actor was last drawn without it
        struct Magazine
        {
            std::int32_t handle = 0;
            DObj* obj = nullptr;
        };
        Magazine held;                                        // in a hand
        std::array<Magazine, MAX_DROPPED_MAGAZINES> dropped;  // on the ground, the latest first
        std::vector<PlacedFall> falls;                        // of those drawn last
        // what was drawn last, for the harness
        ActorLogic::HeldMagazine heldDrawn = ActorLogic::HeldMagazine::None;
        glm::mat4 heldTransform{1.0f};
        std::vector<glm::mat4> droppedDrawn;
        std::string reloadDrawn;

        // A killstreak's: the game's effects it runs while shown (its rotors, lights, exhaust: see
        // KillstreakLogic::GetEffects), each with the handle of the effect playing now (0 for none)
        struct RunningEffect
        {
            std::string effect;
            std::string tag;
            std::int32_t handle = 0;
            Components::KillstreakLogic::EffectRow row = Components::KillstreakLogic::EffectRow::Lights;
        };
        std::vector<RunningEffect> running;
        // and its engine smoke while Damaged
        RunningEffect smoke;
        // the looping effects its events played (a dying killstreak's trails), stopped once it is hidden
        std::vector<std::int32_t> eventLoops;
        // which killstreak it is, and the bones its body turns (VehicleLogic::GetJoints, in that order) in its DObj
        std::optional<Types::Killstreak> killstreak;
        std::vector<std::optional<std::uint8_t>> vehicleBones;
        // A bone a killstreak's guns turn: its index in the DObj, its parent (a script string), its rotation at rest
        // relative to that (which it is turned from) and where it rests from it
        struct GunBone
        {
            std::uint8_t bone = 0;
            std::uint16_t parent = 0;
            glm::mat3 rest{1.0f};
            glm::vec3 offset{0.0f};
        };
        // of each of its guns (KillstreakLogic::Info::guns), the bone it turns about, the one tipping its barrel and
        // the one held while it aims
        struct GunBones
        {
            std::optional<GunBone> yaw;
            std::optional<GunBone> pitch;
            std::optional<GunBone> hold;
        };
        std::vector<GunBones> gunBones;
        std::optional<GunBone> spinBone;

        std::optional<HeadRig> head;  // none for a model without a head that hangs from another bone
        // the tree's entries after the face's that line up the spine (see SpineRig), with what each clip does with it;
        // none when the body doesn't need it (no clip or legs made for SP bodies)
        std::vector<SpineKind> clipSpines;
        std::unique_ptr<SpineAnimation> linedSpine;
        std::unique_ptr<SpineAnimation> restingSpine;
        // and after those, the stabilizer aimed ahead under the upper body clips for each of the legs made for SP
        // bodies (PoseLogic::AimSpine), with the index of the legs' entry
        std::vector<std::pair<std::size_t, std::unique_ptr<SpineAnimation>>> aimedSpines;

        // the models on its bones, each a DObj of its own (as Types::ActorProp)
        struct Prop
        {
            std::string model;
            std::int32_t handle = 0;
            DObj* obj = nullptr;  // null for a model that isn't loaded
        };
        std::vector<Prop> props;
    };
    std::array<Built, MAX_ACTORS> built;

    std::optional<Built::GunBone> FindGunBone(DObj& obj, std::string_view name);

    // The rotations CG_DoControllers gives bones of each actor's DObjs (the actor's own and the one without the gun's
    // magazine) in place of what they are animated to: a killstreak's body, steering and wheels (see PoseVehicle). Its
    // head's copies, while it looks at something (see TurnHead), are turned once the animations have posed them
    // instead (DObjCalcAnim, see BlendHeads), from where the clip has them: CG_DoControllers comes before the
    // animations, so a head set there turned from its rest. That runs on the threads that
    // work out skeletons, so the main thread sets the angles before the bone, and takes the bone away first.
    struct JointControl
    {
        std::atomic<const DObj*> obj = nullptr;
        std::atomic<std::int32_t> bone = -1;
        std::array<std::atomic<float>, 3> angles{};
        std::array<std::atomic<float>, 3> trans{};  // from its rest position
        // a head's: the rotation it turns to (x, y, z, w, relative to its parent) and how much of the clip's it takes
        // over (ActorLogic::HeadTurn); -1 for a joint set by CG_DoControllers
        std::array<std::atomic<float>, 4> rotation{};
        std::atomic<float> weight = -1.0f;
    };
    // of each DObj: the head's copies first, then a killstreak's joints, then a dual wield gun's left one (see
    // GripLeftGun)
    inline constexpr std::size_t FIRST_VEHICLE_JOINT = MAX_HEAD_COPIES;
    inline constexpr std::size_t LEFT_GUN_JOINT = MAX_HEAD_COPIES + 12;
    inline constexpr std::size_t MAX_JOINT_CONTROLS = LEFT_GUN_JOINT + 1;
    std::array<JointControl, MAX_ACTORS * 2 * MAX_JOINT_CONTROLS> jointControls;
    // how many of each DObj's are set, so the DObjs of everything else are passed by quickly
    std::array<std::atomic<std::int32_t>, MAX_ACTORS * 2> jointsControlled{};

    // What the threads that work out skeletons need for the left hand's grip of each actor's DObjs (the actor's own and
    // the one without the gun's magazine): see GripLogic. The main thread sets the rest before obj, and takes obj away
    // first.
    enum class GripPart : std::uint8_t
    {
        None,
        Upper,
        Forearm,
        Twist,
        Hand,
    };
    struct GripRig
    {
        std::atomic<const DObj*> obj = nullptr;
        std::array<std::uint32_t, 5> bones{};  // all of the DObj's, as partBits has them
        std::uint8_t gun = 0;
        std::uint8_t shoulder = 0;
        std::uint8_t elbow = 0;
        std::uint8_t wrist = 0;
        GripLogic::GunGrip standing;
        GripLogic::GunGrip prone;
        GripLogic::GripKind kind = GripLogic::GripKind::Gun;
        std::atomic<bool> lying = false;
        std::atomic<float> share = 1.0f;  // see GripLogic::GetGripShare
        std::atomic<float> campaign = 0.0f;  // see GripLogic::GetCampaignShare
        // bodies with other arms than a soldier's: how they rest, and what the bones of each arm hang from
        bool otherArms = false;
        GripLogic::ArmRest leftArm;
        GripLogic::ArmRest rightArm;
        std::array<std::uint8_t, 4> leftParents{};
        std::array<std::uint8_t, 4> rightParents{};
        // the arm's bones, of all the models that have them, and the part each is of
        std::array<std::uint8_t, MAX_DOBJ_BONES> moved{};
        std::array<GripPart, MAX_DOBJ_BONES> parts{};
        std::size_t movedCount = 0;
    };
    std::array<GripRig, MAX_ACTORS * 2> gripRigs;
    // how many are set, so the DObjs of everything else are passed by quickly
    std::atomic<std::int32_t> gripsSet = 0;
    std::atomic<bool> gripsOn = true;
    std::atomic<bool> gripPlaces = true;

    void ClearGrip(std::size_t slot, std::size_t objIndex)
    {
        auto& rig = gripRigs[slot * 2 + objIndex];
        if (rig.obj.exchange(nullptr))
            --gripsSet;
    }

    // sets a joint of one of a slot's DObjs (0 its own, 1 the one without the magazine), or takes it away (no bone)
    void SetJointControl(std::size_t slot, std::size_t objIndex, std::size_t joint, const DObj* obj,
                         std::optional<std::uint8_t> bone, const glm::vec3& angles, const glm::vec3& trans = {})
    {
        auto& control = jointControls[(slot * 2 + objIndex) * MAX_JOINT_CONTROLS + joint];
        auto& controlled = jointsControlled[slot * 2 + objIndex];
        const bool was = control.bone >= 0;
        control.bone = -1;
        if (!obj || !bone)
        {
            if (was)
                --controlled;
            return;
        }
        control.obj = obj;
        for (int i = 0; i < 3; ++i)
        {
            control.angles[i] = angles[i];
            control.trans[i] = trans[i];
        }
        control.weight = -1.0f;
        control.bone = *bone;
        if (!was)
            ++controlled;
    }

    // sets a copy of a slot's head turned after the animations (see BlendHeads), or takes it away (no turn: the clip's
    // own head)
    void SetHeadTurn(std::size_t slot, std::size_t objIndex, std::size_t copy, const DObj* obj, std::uint8_t bone,
                     const std::optional<ActorLogic::HeadTurn>& turn)
    {
        if (!turn || !obj)
        {
            SetJointControl(slot, objIndex, copy, nullptr, std::nullopt, {});
            return;
        }
        auto& control = jointControls[(slot * 2 + objIndex) * MAX_JOINT_CONTROLS + copy];
        auto& controlled = jointsControlled[slot * 2 + objIndex];
        const bool was = control.bone >= 0;
        control.bone = -1;
        control.obj = obj;
        const auto& rotation = turn->rotation;
        control.rotation[0] = rotation.x;
        control.rotation[1] = rotation.y;
        control.rotation[2] = rotation.z;
        control.rotation[3] = rotation.w;
        control.weight = turn->weight;
        control.bone = bone;
        if (!was)
            ++controlled;
    }

    void ClearJointControls(std::size_t slot, std::size_t from = 0, std::size_t to = MAX_JOINT_CONTROLS)
    {
        for (std::size_t obj = 0; obj < 2; ++obj)
            for (std::size_t joint = from; joint < to; ++joint)
                SetJointControl(slot, obj, joint, nullptr, std::nullopt, {});
    }

    void ClearHeadTurns(std::size_t slot)
    {
        ClearJointControls(slot, 0, MAX_HEAD_COPIES);
    }

    // core's actors as of its generation, and those that couldn't be built with the packs of a pack generation
    std::vector<Types::Actor> scene;
    std::optional<std::uint32_t> sceneGeneration;
    // the animations their clips name (see AnimationStore::RunFrame)
    std::vector<std::string> sceneAnimations;
    // the weapon variants core's actors hold, worked out again with the scene or another catalog of the loadouts
    std::map<std::uint32_t, std::string> sceneWeapons;
    std::map<std::uint32_t, std::string> sceneStowed;
    std::shared_ptr<const Types::LoadoutCatalog> weaponsCatalog;
    std::map<std::uint32_t, std::uint32_t> failed;  // actor id -> pack generation
    // the actors drawn without legs under their upper body clips, as before there were any, for the harness to check
    // those clips' decoded poses alone (see CheckDecodedPose)
    std::set<std::uint32_t> legsLeftOut;

    // the pose of the client entity of a handle, null while the client has none
    cpose_t* GetClientEntityPose(std::int32_t handle)
    {
        const auto entities = *reinterpret_cast<std::uint8_t**>(GetGameAddresses().clientEntities());
        if (!entities)
            return nullptr;
        return reinterpret_cast<cpose_t*>(entities + (handle - FIRST_CLIENT_ENTITY) * CLIENT_ENTITY_SIZE +
                                          CLIENT_ENTITY_POSE);
    }

    cpose_t& GetPose(const Built& actor)
    {
        return *GetClientEntityPose(actor.handle);
    }

    void* __cdecl Alloc(int size)
    {
        return std::calloc(1, static_cast<std::size_t>(size));
    }

    void __cdecl Free(void* pointer, int, int)
    {
        std::free(pointer);
    }

    typedef void*(__cdecl* Alloc_t)(int size);
    typedef void(__cdecl* Free_t)(void* pointer, int size, int context);

    std::uint32_t CountBones(std::span<const DObjModel_s> models)
    {
        std::uint32_t bones = 0;
        for (const auto& model : models)
            bones += model.model->numBones;
        return bones;
    }

    // Every DObj of the actors is made here. The game drops out of the demo when the models have more bones together
    // than a DObj can (see MAX_DOBJ_BONES), so none is made of them: null, as for a DObj that couldn't be made.
    DObj* CreateDObj(std::span<DObjModel_s> models, XAnimTree_s* tree, std::int32_t handle)
    {
        typedef DObj*(__cdecl * Com_ClientDObjCreate_t)(DObjModel_s * models, std::uint32_t numModels,
                                                        XAnimTree_s* tree, std::int32_t handle,
                                                        std::int32_t localClientNum);
        if (models.empty() || CountBones(models) > MAX_DOBJ_BONES)
            return nullptr;
        return reinterpret_cast<Com_ClientDObjCreate_t>(GetGameAddresses().Com_ClientDObjCreate())(
            models.data(), static_cast<std::uint32_t>(models.size()), tree, handle, 0);
    }

    // where XAnimCreate keeps an entry's animation in an XAnim_s, and the names of the animations it keeps for the
    // game's debugging, if it does (XAnimCreateAnims makes them when a flag of the game is set, 0x39D0B34)
    inline constexpr std::size_t XANIM_ENTRIES = 0x18;
    inline constexpr std::size_t XANIM_ENTRY_SIZE = 0x10;
    inline constexpr std::size_t XANIM_ENTRY_PARTS = 0xC;
    inline constexpr std::size_t XANIM_DEBUG_NAMES = 0x10;

    const XAnimParts*& GetEntryParts(XAnim_s* anims, std::uint32_t index)
    {
        return *reinterpret_cast<const XAnimParts**>(reinterpret_cast<std::uint8_t*>(anims) + XANIM_ENTRIES +
                                                     index * XANIM_ENTRY_SIZE + XANIM_ENTRY_PARTS);
    }

    // Makes an entry the animation of its parts, as XAnimCreate does with the parts it finds by name (0x523600 in
    // t5mp.exe: the entry's first u16 1, numAnims 0, the parts, and the name among the debugging names if there are
    // any). By the parts, as an actor's animations may be the store's (see AnimationStore), which the game hasn't
    // registered, or made up here: for a name the game hasn't registered, XAnimCreate's look-up makes a stand-in,
    // which takes a slot of the pool.
    void SetEntryAnimation(XAnim_s* anims, std::uint32_t index, const XAnimParts* parts)
    {
        const auto entry = reinterpret_cast<std::uint8_t*>(anims) + XANIM_ENTRIES + index * XANIM_ENTRY_SIZE;
        const std::uint16_t animation = 1, children = 0;
        std::memcpy(entry, &animation, sizeof(animation));
        std::memcpy(entry + 2, &children, sizeof(children));
        GetEntryParts(anims, index) = parts;
        if (const auto names =
                *reinterpret_cast<const char***>(reinterpret_cast<std::uint8_t*>(anims) + XANIM_DEBUG_NAMES))
            names[index] = parts->name;
    }

    // The animations of an actor's tree: a root blending the animations (the clips', the legs', the face's and the made
    // up ones of the spine), each entry given its parts
    XAnim_s* CreateAnimations(std::span<const XAnimParts* const> animations)
    {
        typedef XAnim_s*(__cdecl * XAnimCreateAnims_t)(const char* debugName, std::uint32_t size, std::uint32_t,
                                                       Alloc_t alloc);
        typedef void(__cdecl * XAnimBlend_t)(XAnim_s * anims, std::uint32_t index, const char* name,
                                             std::uint32_t firstChild, std::uint32_t numChildren, std::uint32_t flags);
        const auto& addresses = GetGameAddresses();
        const auto count = static_cast<std::uint32_t>(animations.size());
        const auto anims =
            reinterpret_cast<XAnimCreateAnims_t>(addresses.XAnimCreateAnims())("actor", count + 1, 0, Alloc);
        reinterpret_cast<XAnimBlend_t>(addresses.XAnimBlend())(anims, ROOT_INDEX, "root", FIRST_CLIP_INDEX, count, 0);
        for (std::uint32_t i = 0; i < count; ++i)
            SetEntryAnimation(anims, FIRST_CLIP_INDEX + i, animations[i]);
        return anims;
    }

    void ReleaseProps(Built& actor)
    {
        typedef void(__cdecl * Com_SafeClientDObjFree_t)(std::int32_t handle, std::int32_t localClientNum);
        for (const auto& prop : actor.props)
            if (prop.obj && Functions::Com_GetClientDObj(prop.handle, 0) == prop.obj)
                reinterpret_cast<Com_SafeClientDObjFree_t>(GetGameAddresses().Com_SafeClientDObjFree())(prop.handle, 0);
        actor.props.clear();
    }

    void StopRunningEffects(Built& actor);

    void StopFireLoop(Built& actor)
    {
        if (!actor.fireLoop)
            return;
        const float at[3] = {actor.fireLoopAt[0], actor.fireLoopAt[1], actor.fireLoopAt[2]};
        Functions::SND_RemoveLoopEmitter(actor.fireLoop, at);
        actor.fireLoop = 0;
    }

    void Release(Built& actor)
    {
        StopFireLoop(actor);
        ClearJointControls(&actor - built.data());
        ClearGrip(&actor - built.data(), 0);
        ClearGrip(&actor - built.data(), 1);
        // (an effect on its handle would go on with whatever is built on it next)
        StopRunningEffects(actor);

        typedef void(__cdecl * Com_SafeClientDObjFree_t)(std::int32_t handle, std::int32_t localClientNum);
        typedef void(__cdecl * XAnimFreeTree_t)(XAnimTree_s * tree, Free_t free, std::int32_t context);
        const auto& addresses = GetGameAddresses();

        std::vector<Built::Magazine> others = {actor.held, {actor.unloadedHandle, actor.unloadedObj}};
        others.insert(others.end(), actor.dropped.begin(), actor.dropped.end());
        for (const auto& [handle, obj] : others)
            if (obj && Functions::Com_GetClientDObj(handle, 0) == obj)
                reinterpret_cast<Com_SafeClientDObjFree_t>(addresses.Com_SafeClientDObjFree())(handle, 0);
        ReleaseProps(actor);

        // The client frees every DObj with the map. Then the tree's animation state went with it, and only the
        // memory is left to free.
        if (Functions::Com_GetClientDObj(actor.handle, 0) == actor.obj)
        {
            reinterpret_cast<centity_t&>(GetPose(actor)).flags &= ~CENTITY_FLAG_HAS_MODEL;
            reinterpret_cast<Com_SafeClientDObjFree_t>(addresses.Com_SafeClientDObjFree())(actor.handle, 0);
            if (actor.tree)
                reinterpret_cast<XAnimFreeTree_t>(addresses.XAnimFreeTree())(actor.tree, Free, 0);
        }
        else
        {
            std::free(actor.tree);
        }
        std::free(actor.anims);
        actor = {};
    }

    void ReleaseAll()
    {
        for (auto& actor : built)
            if (actor.active)
                Release(actor);
    }

    std::optional<std::int32_t> FindFreeHandle()
    {
        for (std::int32_t handle = LAST_HANDLE; handle > LAST_HANDLE - MAX_HANDLES; --handle)
        {
            const bool taken = std::ranges::any_of(built, [&](const Built& actor) {
                return actor.active &&
                       (actor.handle == handle || actor.held.handle == handle || actor.unloadedHandle == handle ||
                        std::ranges::find(actor.dropped, handle, &Built::Magazine::handle) != actor.dropped.end() ||
                        std::ranges::find(actor.props, handle, &Built::Prop::handle) != actor.props.end());
            });
            // nor one of an entity of the client's scripts, which may have no model
            const auto pose = GetClientEntityPose(handle);
            const bool used = !pose || Functions::Com_GetClientDObj(handle, 0) ||
                              (reinterpret_cast<const centity_t*>(pose)->flags & CENTITY_FLAG_HAS_MODEL);
            if (!taken && !used)
                return handle;
        }
        return std::nullopt;
    }

    // the clips that are entries of an actor's tree: all but those of a killstreak's motions, which are no animations
    // (see KillstreakLogic)
    bool IsTreeClip(const Types::ActorClip& clip)
    {
        return !Components::KillstreakLogic::IsMotion(clip.animation);
    }

    std::vector<std::uint32_t> GetClipIds(const Types::Actor& actor)
    {
        std::vector<std::uint32_t> ids;
        for (const auto& clip : actor.clips)
            if (IsTreeClip(clip))
                ids.push_back(clip.id);
        return ids;
    }

    // The tags actors use, as script strings. SL_GetString adds a reference to the string each time, so each is
    // asked for once; the table is rebuilt when a map loads.
    std::unordered_map<std::string, std::uint16_t> scriptStrings;

    std::uint16_t GetScriptString(const std::string& name)
    {
        auto it = scriptStrings.find(name);
        if (it == scriptStrings.end())
            it = scriptStrings.emplace(name, Functions::SL_GetString(name.c_str())).first;
        return it->second;
    }

    std::string GetSceneWeapon(std::uint32_t id)
    {
        const auto it = sceneWeapons.find(id);
        return it != sceneWeapons.end() ? it->second : std::string{};
    }

    std::string GetSceneStowed(std::uint32_t id)
    {
        const auto it = sceneStowed.find(id);
        return it != sceneStowed.end() ? it->second : std::string{};
    }

    // Hides the bones of the variant's world model that it hides (the attachments it doesn't have), like the client
    // does for a player's gun
    // the bones of a variant's world model it hides (the attachments it doesn't have): in the DObj's first model with
    // them, or only in the model at an index
    void HideWeaponParts(DObj& obj, const WeaponVariantDef& variant, int modelIndex = -1)
    {
        if (!variant.hideTags)
            return;
        for (std::size_t i = 0; i < MAX_WEAPON_HIDE_TAGS && variant.hideTags[i]; ++i)
            if (const auto bone = Functions::DObjGetBoneIndex(&obj, variant.hideTags[i], modelIndex))
                obj.hidePartBits[*bone / 32] |= 0x80000000u >> (*bone % 32);
    }

    // the parts a slot's guns hide, in a DObj of its models
    void HideWeaponParts(DObj& obj, const Built& slot)
    {
        std::ranges::fill(obj.hidePartBits, 0u);
        if (slot.variant)
            HideWeaponParts(obj, *slot.variant);
        if (slot.leftVariant)
            HideWeaponParts(obj, *slot.leftVariant, static_cast<int>(slot.leftGunModel));
        if (slot.stowedVariant)
            HideWeaponParts(obj, *slot.stowedVariant, static_cast<int>(slot.stowedModel));
    }

    // the models of a slot's DObj, the guns on their hands, to make another DObj of
    std::vector<DObjModel_s> GetModels(const Built& slot)
    {
        std::vector<DObjModel_s> models;
        for (std::uint32_t i = 0; slot.obj && i < static_cast<std::uint32_t>(slot.obj->numModels); ++i)
        {
            const auto tag = slot.variant && i == slot.weaponModel        ? GetScriptString("tag_weapon_right")
                             : slot.leftVariant && i == slot.leftGunModel ? GetScriptString("tag_weapon_left")
                             : slot.stowedVariant && i == slot.stowedModel
                                 ? GetScriptString(std::string(Components::PlayerLookLogic::STOWED_TAG))
                                 : std::uint16_t{0};
            models.push_back({slot.obj->models[i], tag, false});
        }
        return models;
    }

    std::vector<std::string> GetClipAnimations(const Types::Actor& actor)
    {
        std::vector<std::string> animations;
        for (const auto& clip : actor.clips)
            if (IsTreeClip(clip))
                animations.push_back(clip.animation);
        return animations;
    }

    std::optional<HeadRig> FindHeadRig(DObj& obj);
    void SetGrip(Built& slot, std::size_t objIndex, DObj* obj);
    std::unique_ptr<SpineAnimation> MakeSpineAnimation(glm::quat rotation, glm::vec3 offset, const XAnimParts& like);

    Types::ActorCatalog::ModelKind Classify(std::string_view name);

    // The spine rig. Animations made for MP bodies (the players' pb_ and pt_ ones)
    // rotate j_spinelower relative to torso_stabilizer, which SP bodies and full bodies don't have: they hang
    // j_spinelower from j_mainroot, so the torso turned by about 90 degrees (see PoseLogic::SpineChain). A DObj's
    // models take the bones named like those of the models before them from those, so an actor of such a body playing
    // such clips gets a model of five bones and nothing to draw in front of its body: tag_origin, j_mainroot, pelvis,
    // the stabilizer and j_spinelower. The body's j_spinelower is then the rig's, on the stabilizer, and the game puts
    // the spine where it does on MP bodies. Clips made for SP bodies on the same actor have the rig's stabilizer lined
    // up, as on MP bodies (see SpineRig). Its rests are the body's for tag_origin, j_mainroot and where j_spinelower is
    // (so the body's mesh rests as it was made) and the map's MP bodies' for pelvis and the stabilizer
    // (AnimationPose::FitSpineChain). A rig is a copy of the MP body's model with bones of its own and no surfaces,
    // made for each body that needs one and kept until the map goes, like the magazines' models.
    inline constexpr std::uint8_t SPINE_RIG_BONES = 5;
    inline constexpr std::array<std::string_view, SPINE_RIG_BONES> SPINE_RIG_BONE_NAMES = {
        "tag_origin", "j_mainroot", "pelvis", "torso_stabilizer", "j_spinelower"};
    inline constexpr auto SPINE_RIG_NAME = "actor_spine_rig";

    struct SpineRigModel
    {
        XModel model{};
        const XModel* body = nullptr;  // that it was made for
        std::uint16_t boneNames[SPINE_RIG_BONES]{};
        std::uint8_t parentList[SPINE_RIG_BONES - 1]{};
        std::int16_t quats[SPINE_RIG_BONES - 1][4]{};
        float trans[SPINE_RIG_BONES - 1][3]{};
        std::uint8_t partClassification[SPINE_RIG_BONES]{};
        DObjAnimMat baseMat[SPINE_RIG_BONES]{};
        XBoneInfo boneInfo[SPINE_RIG_BONES]{};
    };
    std::vector<std::unique_ptr<SpineRigModel>> spineRigs;
    // the map's MP body the rigs are copies of (see BuildCatalog)
    const XModel* spineSource = nullptr;

    // The spine rig of a body, made the first time; null for a body that doesn't need one (it has a stabilizer of its
    // own, or not the spine of SP bodies) or before the map's MP bodies are known
    const XModel* GetSpineRig(const XModel& body)
    {
        const auto chain = spineSource ? AnimationPose::FitSpineChain(body) : std::nullopt;
        if (!chain)
            return nullptr;
        auto rig = std::make_unique<SpineRigModel>();
        rig->body = &body;

        // each bone's name, hit location, rest in the model and bounds come from the body, but for pelvis and the
        // stabilizer, which come from the MP body as the chain does
        const std::array<const XModel*, SPINE_RIG_BONES> from = {&body, &body, spineSource, spineSource, &body};
        std::array<std::int32_t, SPINE_RIG_BONES> bones{};
        for (std::uint8_t b = 0; b < SPINE_RIG_BONES; ++b)
        {
            bones[b] = AnimationPose::FindBone(*from[b], SPINE_RIG_BONE_NAMES[b]);
            if (bones[b] < 0 || (b == 0) != (bones[b] < from[b]->numRootBones))
                return nullptr;
            rig->boneNames[b] = from[b]->boneNames[bones[b]];
            if (from[b]->partClassification)
                rig->partClassification[b] = from[b]->partClassification[bones[b]];
            if (from[b]->baseMat)
                rig->baseMat[b] = from[b]->baseMat[bones[b]];
            if (from[b]->boneInfo)
                rig->boneInfo[b] = from[b]->boneInfo[bones[b]];
        }
        // the body's j_mainroot on its tag_origin
        if (bones[1] - body.parentList[bones[1] - body.numRootBones] != bones[0])
            return nullptr;
        // each on the one before, at rest as it is in the model it comes from, j_spinelower where the body has it
        for (std::uint8_t b = 1; b < SPINE_RIG_BONES; ++b)
        {
            rig->parentList[b - 1] = 1;
            const auto source = bones[b] - from[b]->numRootBones;
            std::ranges::copy(from[b]->quats[source], rig->quats[b - 1]);
            std::ranges::copy(from[b]->trans[source], rig->trans[b - 1]);
        }
        std::ranges::copy(AnimationPose::ToShorts(chain->spine.rotation), rig->quats[SPINE_RIG_BONES - 2]);
        const auto spineAt = chain->spine.translation;
        std::ranges::copy(std::array{spineAt.x, spineAt.y, spineAt.z}, rig->trans[SPINE_RIG_BONES - 2]);

        // one made for the same body before (the body at the same place may be another model now)
        const auto same = std::ranges::find_if(spineRigs, [&](const std::unique_ptr<SpineRigModel>& other) {
            return other->body == &body && std::ranges::equal(other->boneNames, rig->boneNames) &&
                   std::memcmp(other->quats, rig->quats, sizeof(rig->quats)) == 0 &&
                   std::memcmp(other->trans, rig->trans, sizeof(rig->trans)) == 0;
        });
        if (same != spineRigs.end())
            return &(*same)->model;

        auto& model = rig->model;
        model = *spineSource;
        model.name = SPINE_RIG_NAME;
        model.numBones = SPINE_RIG_BONES;
        model.numRootBones = 1;
        model.numsurfs = 0;
        model.boneNames = rig->boneNames;
        model.parentList = rig->parentList;
        model.quats = rig->quats;
        model.trans = rig->trans;
        model.partClassification = rig->partClassification;
        model.baseMat = rig->baseMat;
        model.boneInfo = rig->boneInfo;
        for (auto& lod : model.lodInfo)
        {
            // nothing to draw, on all of its bones
            lod.numsurfs = 0;
            lod.surfIndex = 0;
            std::ranges::fill(lod.partBits, 0u);
            for (std::uint32_t b = 0; b < SPINE_RIG_BONES; ++b)
                lod.partBits[0] |= 0x80000000u >> b;
        }
        model.collSurfs = nullptr;
        model.numCollSurfs = 0;
        model.physPreset = nullptr;
        model.numCollmaps = 0;
        model.collmaps = nullptr;
        model.physConstraints = nullptr;
        spineRigs.push_back(std::move(rig));
        return &spineRigs.back()->model;
    }

    // The game's bodies for players have tag_stowed_back; the campaign's and civilians' mostly don't, and the game puts a
    // model on a bone a DObj hasn't at the root. So a body without it gets a model of two bones and nothing to draw after
    // its own (a DObj's models take the bones named like those of the models before them from those, as the spine rig's
    // do): the bone the map's MP bodies hang the tag from, and the tag, where they have it from there. Made once a map.
    inline constexpr std::uint8_t STOWED_RIG_BONES = 2;
    inline constexpr auto STOWED_RIG_NAME = "actor_stowed_rig";
    struct StowedRigModel
    {
        XModel model{};
        std::uint16_t boneNames[STOWED_RIG_BONES]{};
        std::uint8_t parentList[STOWED_RIG_BONES - 1]{};
        std::int16_t quats[STOWED_RIG_BONES - 1][4]{};
        float trans[STOWED_RIG_BONES - 1][3]{};
        std::uint8_t partClassification[STOWED_RIG_BONES]{};
        DObjAnimMat baseMat[STOWED_RIG_BONES]{};
        XBoneInfo boneInfo[STOWED_RIG_BONES]{};
    };
    std::unique_ptr<StowedRigModel> stowedRig;
    std::atomic<bool> stowedRigForced = false;

    void SetStowedRigForced(bool on)
    {
        stowedRigForced = on;
    }

    // null before the map's MP bodies are known, or for one that has no such tag
    const XModel* GetStowedRig()
    {
        if (stowedRig)
            return &stowedRig->model;
        if (!spineSource)
            return nullptr;
        const auto tag = AnimationPose::FindBone(*spineSource, Components::PlayerLookLogic::STOWED_TAG);
        if (tag < spineSource->numRootBones)
            return nullptr;
        const auto index = tag - spineSource->numRootBones;
        const auto parent = tag - spineSource->parentList[index];
        if (parent < 0)
            return nullptr;

        auto rig = std::make_unique<StowedRigModel>();
        rig->boneNames[0] = spineSource->boneNames[parent];
        rig->boneNames[1] = spineSource->boneNames[tag];
        rig->parentList[0] = 1;
        std::ranges::copy(spineSource->quats[index], rig->quats[0]);
        std::ranges::copy(spineSource->trans[index], rig->trans[0]);
        for (std::uint8_t b = 0; b < STOWED_RIG_BONES; ++b)
        {
            const auto from = b == 0 ? parent : tag;
            if (spineSource->partClassification)
                rig->partClassification[b] = spineSource->partClassification[from];
            if (spineSource->baseMat)
                rig->baseMat[b] = spineSource->baseMat[from];
            if (spineSource->boneInfo)
                rig->boneInfo[b] = spineSource->boneInfo[from];
        }
        auto& model = rig->model;
        model = *spineSource;
        model.name = STOWED_RIG_NAME;
        model.numBones = STOWED_RIG_BONES;
        model.numRootBones = 1;
        model.numsurfs = 0;
        model.boneNames = rig->boneNames;
        model.parentList = rig->parentList;
        model.quats = rig->quats;
        model.trans = rig->trans;
        model.partClassification = rig->partClassification;
        model.baseMat = rig->baseMat;
        model.boneInfo = rig->boneInfo;
        for (auto& lod : model.lodInfo)
        {
            // nothing to draw, on all of its bones
            lod.numsurfs = 0;
            lod.surfIndex = 0;
            std::ranges::fill(lod.partBits, 0u);
            for (std::uint32_t b = 0; b < STOWED_RIG_BONES; ++b)
                lod.partBits[0] |= 0x80000000u >> b;
        }
        model.collSurfs = nullptr;
        model.numCollSurfs = 0;
        model.physPreset = nullptr;
        model.numCollmaps = 0;
        model.collmaps = nullptr;
        model.physConstraints = nullptr;
        stowedRig = std::move(rig);
        return &stowedRig->model;
    }

    // What an actor is built of: its models (the body first, after its spine rig if it needs one) and the gun's world
    // model in the right hand, as on players, a dual wield gun's left one in the left hand, and the gun on its back; as
    // many of them as a DObj can have the bones of (see ActorLogic::FitBones), and what is left out. None when a model
    // isn't loaded.
    struct FittedModels
    {
        std::vector<DObjModel_s> models;
        bool spineRig = false;  // the first of the models is the body's spine rig
        bool gun = false;       // the gun's is the last of the models, before the left gun's and the stowed one's
        bool leftGun = false;   // the left gun's is the last of the models, before the stowed one's
        bool stowed = false;    // the stowed gun's is the last of the models
        bool stowedRig = false;  // and right before it, what it hangs on (see GetStowedRig)
        Components::Actors::LeftOut leftOut;
    };

    // A killstreak's model as it is seen from outside (KillstreakLogic::Info::insideLods): a copy whose levels of detail
    // start after those the game draws only from inside it (the Gunship's pilot's cockpit), as the free camera draws
    // every model's first at any distance; the model itself for others. The copies go with the map's models.
    std::map<const XModel*, std::unique_ptr<XModel>> outsideModels;

    XModel* GetOutsideModel(XModel* model)
    {
        const auto killstreak = Components::KillstreakLogic::FindByModel(model->name);
        const std::size_t inside = killstreak ? Components::KillstreakLogic::Get(*killstreak).insideLods : 0;
        if (inside == 0)
            return model;
        auto& copy = outsideModels[model];
        if (!copy)
        {
            copy = std::make_unique<XModel>(*model);
            for (std::size_t lod = 0; lod < MAX_LODS; ++lod)
                copy->lodInfo[lod] = lod + inside < MAX_LODS ? model->lodInfo[lod + inside] : XModelLodInfo{};
        }
        return copy.get();
    }

    // A body's model with its gun tag resting where a soldier's does (GripLogic::SOLDIER_GUN_TAG); the model itself for
    // one that has it there. The copies go with the map's models; the original is kept to tell a model that came in
    // after its zone went from the one copied.
    struct HoldingModel
    {
        XModel original{};
        XModel copy{};
        std::vector<std::array<float, 3>> trans;
        std::vector<std::array<std::int16_t, 4>> quats;
    };
    std::map<const XModel*, std::unique_ptr<HoldingModel>> holdingModels;

    // A campaign or Zombies body on a player. The game's IK turns a player's torso back from where the legs run and puts
    // the left hand on the gun, but only on a DObj whose first model has all of its 23 bones (0x5BD8D0, 0x681C50); with a
    // spine rig in front of the body it never ran, and the torso turned with the legs, up to 54 degrees. What
    // the rig is for is one bone: animations made for MP bodies key j_spinelower relative to torso_stabilizer, and never
    // key pelvis. So a copy of the body takes the place of a tag nothing hangs from for the stabilizer, on j_mainroot, and
    // hangs j_spinelower from it, with the rests the rig has (see GetSpineRig). A body has its bones first in the DObj, the
    // bones of the head and the gun attach to it as ever, and it costs none. The body itself for one with no such tag or
    // none needed. The copies go with the map's models; the original tells a model that came in after its zone went.
    struct PlayerBody
    {
        XModel original{};
        XModel copy{};
        std::vector<std::uint16_t> boneNames;
        std::vector<std::uint8_t> parentList;
        std::vector<std::array<float, 3>> trans;
        std::vector<std::array<std::int16_t, 4>> quats;
        std::vector<std::uint8_t> partClassification;
        std::vector<DObjAnimMat> baseMat;
        std::vector<XBoneInfo> boneInfo;
    };
    std::map<const XModel*, std::unique_ptr<PlayerBody>> playerBodies;

    std::optional<std::size_t> FindSpareTag(const XModel& model, std::int32_t after, std::int32_t before)
    {
        std::vector<std::string_view> names;
        std::vector<std::int32_t> parents;
        for (std::int32_t bone = 0; bone < model.numBones; ++bone)
        {
            names.emplace_back(Functions::SL_ConvertToString(model.boneNames[bone]));
            parents.push_back(bone < model.numRootBones ? -1 : bone - model.parentList[bone - model.numRootBones]);
        }
        return PoseLogic::FindSpareTag(names, parents, static_cast<std::size_t>(after), static_cast<std::size_t>(before));
    }

    std::atomic<bool> playerBodiesOn = true;

    void SetPlayerBodiesOn(bool on)
    {
        playerBodiesOn = on;
    }

    XModel* GetPlayerBody(XModel* model)
    {
        const auto chain = spineSource && playerBodiesOn ? AnimationPose::FitSpineChain(*model) : std::nullopt;
        if (!chain)
            return model;
        auto& body = playerBodies[model];
        if (body && std::memcmp(&body->original, model, sizeof(XModel)) == 0)
            return &body->copy;
        body.reset();

        const auto root = AnimationPose::FindBone(*model, "j_mainroot");
        const auto spine = AnimationPose::FindBone(*model, "j_spinelower");
        const auto stabilizer = AnimationPose::FindBone(*spineSource, "torso_stabilizer");
        const auto spare = FindSpareTag(*model, root, spine);
        if (!spare || stabilizer < 0)
            return model;
        const auto tag = static_cast<std::int32_t>(*spare);

        auto made = std::make_unique<PlayerBody>();
        made->original = *model;
        made->copy = *model;
        const std::size_t bones = model->numBones;
        const std::size_t moved = bones - model->numRootBones;
        made->boneNames.assign(model->boneNames, model->boneNames + bones);
        made->parentList.assign(model->parentList, model->parentList + moved);
        made->trans.resize(moved);
        made->quats.resize(moved);
        std::memcpy(made->trans.data(), model->trans, moved * sizeof(float[3]));
        std::memcpy(made->quats.data(), model->quats, moved * sizeof(std::int16_t[4]));

        // the stabilizer at rest on j_mainroot, through pelvis, which stays out
        const auto stabilizerRotation = chain->pelvis.rotation * chain->stabilizer.rotation;
        const auto stabilizerAt = chain->pelvis.translation + chain->pelvis.rotation * chain->stabilizer.translation;
        const auto tagIndex = tag - model->numRootBones, spineIndex = spine - model->numRootBones;
        made->boneNames[tag] = spineSource->boneNames[stabilizer];
        made->parentList[tagIndex] = static_cast<std::uint8_t>(tag - root);
        made->quats[tagIndex] = AnimationPose::ToShorts(stabilizerRotation);
        made->trans[tagIndex] = {stabilizerAt.x, stabilizerAt.y, stabilizerAt.z};
        made->parentList[spineIndex] = static_cast<std::uint8_t>(spine - tag);
        made->quats[spineIndex] = AnimationPose::ToShorts(chain->spine.rotation);
        made->trans[spineIndex] = {chain->spine.translation.x, chain->spine.translation.y, chain->spine.translation.z};

        made->copy.boneNames = made->boneNames.data();
        made->copy.parentList = made->parentList.data();
        made->copy.trans = reinterpret_cast<float (*)[3]>(made->trans.data());
        made->copy.quats = reinterpret_cast<std::int16_t (*)[4]>(made->quats.data());
        if (model->partClassification)
        {
            made->partClassification.assign(model->partClassification, model->partClassification + bones);
            if (spineSource->partClassification)
                made->partClassification[tag] = spineSource->partClassification[stabilizer];
            made->copy.partClassification = made->partClassification.data();
        }
        if (model->baseMat)
        {
            made->baseMat.assign(model->baseMat, model->baseMat + bones);
            if (spineSource->baseMat)
                made->baseMat[tag] = spineSource->baseMat[stabilizer];
            made->copy.baseMat = made->baseMat.data();
        }
        if (model->boneInfo)
        {
            made->boneInfo.assign(model->boneInfo, model->boneInfo + bones);
            if (spineSource->boneInfo)
                made->boneInfo[tag] = spineSource->boneInfo[stabilizer];
            made->copy.boneInfo = made->boneInfo.data();
        }
        body = std::move(made);
        return &body->copy;
    }

    XModel* GetHoldingModel(XModel* model)
    {
        const auto tag = GetScriptString("tag_weapon_right");
        const auto end = model->boneNames + model->numBones;
        const auto bone = static_cast<std::uint32_t>(std::find(model->boneNames, end, tag) - model->boneNames);
        if (bone >= model->numBones || bone < model->numRootBones)
            return model;
        const auto index = bone - model->numRootBones;
        if (GripLogic::IsSoldierGunTag(glm::make_vec3(model->trans[index])))
            return model;

        auto& holding = holdingModels[model];
        if (!holding || std::memcmp(&holding->original, model, sizeof(XModel)) != 0)
        {
            holding = std::make_unique<HoldingModel>();
            holding->original = *model;
            holding->copy = *model;
            const auto count = model->numBones - model->numRootBones;
            holding->trans.resize(count);
            holding->quats.resize(count);
            std::memcpy(holding->trans.data(), model->trans, count * sizeof(float[3]));
            std::memcpy(holding->quats.data(), model->quats, count * sizeof(std::int16_t[4]));
            holding->trans[index] = {GripLogic::SOLDIER_GUN_TAG.x, GripLogic::SOLDIER_GUN_TAG.y,
                                     GripLogic::SOLDIER_GUN_TAG.z};
            holding->quats[index] = GripLogic::SOLDIER_GUN_TAG_ROTATION;
            holding->copy.trans = reinterpret_cast<float (*)[3]>(holding->trans.data());
            holding->copy.quats = reinterpret_cast<std::int16_t (*)[4]>(holding->quats.data());
        }
        return &holding->copy;
    }

    // with spineRig, the body gets its spine rig if it needs one (when animations made for MP bodies play on it)
    std::optional<FittedModels> FitModels(const std::vector<std::string>& names, XModel* gun, XModel* leftGun,
                                          bool spineRig, XModel* stowed = nullptr)
    {
        std::vector<DObjModel_s> models;
        for (const auto& name : names)
        {
            const auto model = static_cast<XModel*>(ContentPacks::FindAsset(ASSET_TYPE_XMODEL, name.c_str()));
            if (!model)
                return std::nullopt;
            // the models after the first take the bones of the same names from it
            models.push_back({GetOutsideModel(model), 0, false});
        }
        if (gun && !models.empty())
            models.push_back({gun, GetScriptString("tag_weapon_right"), false});
        const auto leftGunIndex = models.size();
        const bool hasLeftGun = gun && leftGun && !names.empty();
        if (hasLeftGun)
            models.push_back({leftGun, GetScriptString("tag_weapon_left"), false});
        // the gun on its back, on the rig if the body hasn't the bone
        const auto stowedTag = GetScriptString(std::string(Components::PlayerLookLogic::STOWED_TAG));
        const auto hasStowedTag = [&] {
            return std::ranges::any_of(models, [&](const DObjModel_s& model) {
                return std::ranges::find(model.model->boneNames, model.model->boneNames + model.model->numBones,
                                         stowedTag) != model.model->boneNames + model.model->numBones;
            });
        };
        const auto stowedRigModel =
            stowed && !names.empty() && (stowedRigForced || !hasStowedTag()) ? GetStowedRig() : nullptr;
        const auto stowedRigIndex = models.size();
        if (stowedRigModel)
            models.push_back({const_cast<XModel*>(stowedRigModel), 0, false});
        const auto stowedIndex = models.size();
        const bool hasStowed = stowed && !names.empty();
        if (hasStowed)
            models.push_back({stowed, stowedTag, false});
        // (after the others here, first in the DObj)
        const auto rig = spineRig && !models.empty() ? GetSpineRig(*models[0].model) : nullptr;
        const auto rigIndex = models.size();
        if (rig)
            models.push_back({const_cast<XModel*>(rig), 0, false});

        // what matters most goes first: the body and its rig, its head, the gun (which shots and reloads need), then
        // what is worn
        const auto catalog = Components::Actors::GetCatalog();
        const auto isHead = [&](std::size_t i) {
            if (i >= names.size())
                return false;
            const auto model = catalog ? catalog->FindModel(names[i]) : nullptr;
            return (model ? model->kind : Classify(names[i])) == Types::ActorCatalog::ModelKind::Head;
        };
        std::vector<std::size_t> order;
        if (!names.empty())
            order.push_back(0);
        if (rig)
            order.push_back(rigIndex);
        for (std::size_t i = 1; i < names.size(); ++i)
            if (isHead(i))
                order.push_back(i);
        if (gun && !names.empty())
            order.push_back(names.size());
        if (hasLeftGun)
            order.push_back(leftGunIndex);
        for (std::size_t i = 1; i < names.size(); ++i)
            if (!isHead(i))
                order.push_back(i);
        if (stowedRigModel)
            order.push_back(stowedRigIndex);
        if (hasStowed)
            order.push_back(stowedIndex);
        std::vector<std::uint32_t> bones;
        for (const auto i : order)
            bones.push_back(models[i].model->numBones);
        const auto fits = ActorLogic::FitBones(bones, MAX_DOBJ_BONES);
        std::vector<bool> kept(models.size(), false);
        for (std::size_t i = 0; i < order.size(); ++i)
            kept[order[i]] = fits[i];

        FittedModels fitted;
        fitted.leftOut.bones = CountBones(models);
        // the rig with the body only, in front of it
        if (rig && kept[rigIndex] && kept[0])
        {
            fitted.models.push_back(models[rigIndex]);
            fitted.spineRig = true;
        }
        for (std::size_t i = 0; i < rigIndex; ++i)
        {
            const bool isGun = gun && i == names.size();
            const bool isLeftGun = hasLeftGun && i == leftGunIndex;
            const bool isStowedRig = stowedRigModel && i == stowedRigIndex;
            const bool isStowed = hasStowed && i == stowedIndex;
            // (the left gun only with the right one, the rig and the gun on it only together)
            const bool withOthers = (!isLeftGun || fitted.gun) && (!isStowed || !stowedRigModel || kept[stowedRigIndex]) &&
                                    (!isStowedRig || kept[stowedIndex]);
            if (kept[i] && withOthers)
            {
                fitted.models.push_back(models[i]);
                fitted.gun |= isGun;
                fitted.leftGun |= isLeftGun;
                fitted.stowed |= isStowed;
                fitted.stowedRig |= isStowedRig;
            }
            else if (isStowed)
                fitted.leftOut.stowed = true;
            else if (isStowedRig)
                continue;
            else if (isGun || isLeftGun)
                fitted.leftOut.gun = true;
            else
                fitted.leftOut.models.push_back(names[i]);
        }
        // the gun where the animations have it from the hand, on bodies that rest its tag elsewhere
        if (fitted.gun)
            for (auto& model : fitted.models)
                if (model.model != gun && model.model != leftGun && model.model != stowed &&
                    model.model != stowedRigModel)
                    model.model = GetHoldingModel(model.model);
        return fitted;
    }

    // the class of a gun as far as the players' animations go: those of its player animation type, else its class's
    Types::GunClass GetGunClass(const WeaponVariantDef* variant)
    {
        const auto def = variant ? variant->weapDef : nullptr;
        if (!def)
            return Types::GunClass::Rifle;
        if (def->playerAnimType == PLAYER_ANIM_TYPE_BALLISTIC_KNIFE)
            return Types::GunClass::BallisticKnife;
        if (def->playerAnimType == PLAYER_ANIM_TYPE_DUAL_WIELD)
            return Types::GunClass::DualWield;
        if (def->playerAnimType == PLAYER_ANIM_TYPE_MINIGUN)
            return Types::GunClass::Minigun;
        if (def->weapClass == WEAPCLASS_PISTOL)
            return Types::GunClass::Pistol;
        if (def->weapClass == WEAPCLASS_ROCKETLAUNCHER)
            return Types::GunClass::RocketLauncher;
        return Types::GunClass::Rifle;
    }

    // An animation by the catalog's name, as the game has it registered or as the animation store loads it (with a
    // reference that keeps it loaded), null for one neither has: every animation an actor's tree takes (its clips', its
    // legs' and its face's) is found here
    AnimationStore::Found FindAnimation(const std::string& name)
    {
        return AnimationStore::Find(name);
    }

    // the most steps a root motion is measured in (an animation's frames, every other frame and so on for the longest)
    inline constexpr std::uint32_t MAX_ROOT_MOTION_STEPS = 1024;

    // How an animation of a tree moves the root through a play (see ActorLogic::RootMotion), as the game's delta of it
    // from its start has it at each of its frames
    std::shared_ptr<const ActorLogic::RootMotion> MeasureRootMotion(XAnim_s* anims, std::uint32_t index,
                                                                    const XAnimParts& parts)
    {
        auto motion = std::make_shared<ActorLogic::RootMotion>();
        const auto steps = std::clamp<std::uint32_t>(parts.numframes, 1, MAX_ROOT_MOTION_STEPS);
        float turned = 0.0f;
        for (std::uint32_t step = 0; step <= steps; ++step)
        {
            float rotation[2] = {}, translation[3] = {};
            Functions::XAnimGetRelDelta(anims, index, rotation, translation, 0.0f, static_cast<float>(step) / steps);
            // the turn is a quaternion about z (the sine and cosine of half of it); unwrapped from step to step
            const auto yaw = glm::degrees(2.0f * std::atan2(rotation[0], rotation[1]));
            turned += std::remainder(yaw - turned, 360.0f);
            motion->samples.emplace_back(translation[0], translation[1], turned);
        }
        return motion;
    }

    // whether models have a face that the face's animations move (a head's or a full body's jaw, lips and brows; a
    // dog has a jaw and lips too, but isn't asked)
    bool HasFace(std::span<const DObjModel_s> models)
    {
        const std::array tags{GetScriptString("j_jaw"), GetScriptString("j_lip_top_le"), GetScriptString("j_brow_le")};
        return std::ranges::any_of(models, [&](const DObjModel_s& model) {
            return std::ranges::any_of(tags, [&](std::uint16_t tag) {
                return std::ranges::find(model.model->boneNames, model.model->boneNames + model.model->numBones, tag) !=
                       model.model->boneNames + model.model->numBones;
            });
        });
    }

    // whether a weapon variant of the weapon catalog is in a zone that is yet to load
    bool IsWeaponZoneComing(const std::string& weapon)
    {
        const auto zones = Components::WeaponCatalog::GetZones(Components::WeaponCatalog::Get(), weapon);
        return !zones.empty() && Components::Actors::IsCharacterZoneAvailable(zones.front()) &&
               !ContentPacks::IsCharacterZoneLoaded(zones.front()) && !ContentPacks::HasZoneFailed(zones.front());
    }

    // builds what the game draws an actor with, without what doesn't fit (see FitModels); false when its models or
    // animations (or its weapon's zone) aren't loaded
    bool Build(Built& slot, const Types::Actor& actor)
    {
        // the gun goes in the right hand, as on players; a dog or a killstreak holds nothing (its gun stays the scene's,
        // if it has one, for when it is a person again)
        const bool dog = Components::Actors::GetActorKind(actor) != Types::ActorKind::Human;
        const auto weapon = GetSceneWeapon(actor.id);
        const auto variant = weapon.empty() || dog ? nullptr : WeaponRig::FindVariant(weapon);
        // a weapon of a zone of its own (the campaign's, Zombies') waits for its zone, as a character does, unless it
        // won't load
        if (!variant && !weapon.empty() && !dog && IsWeaponZoneComing(weapon))
            return false;
        const auto gun =
            variant && variant->weapDef && variant->weapDef->worldModel ? variant->weapDef->worldModel[0] : nullptr;
        // and a dual wield gun's left one in the left hand, as players hold it (BG_UpdatePlayerDObj)
        const auto leftVariant = variant ? WeaponRig::FindLeftHandVariant(*variant) : nullptr;
        const auto leftGun = leftVariant && leftVariant->weapDef && leftVariant->weapDef->worldModel
                                 ? leftVariant->weapDef->worldModel[0]
                                 : nullptr;
        // the gun on its back waits for its zone too
        const auto stowedWeapon = GetSceneStowed(actor.id);
        const auto stowedVariant = stowedWeapon.empty() || dog ? nullptr : WeaponRig::FindVariant(stowedWeapon);
        if (!stowedVariant && !stowedWeapon.empty() && !dog && IsWeaponZoneComing(stowedWeapon))
            return false;
        const auto stowedGun = stowedVariant ? WeaponRig::GetStowedModel(*stowedVariant) : nullptr;
        std::vector<const XAnimParts*> clipParts;  // (null for one that isn't loaded)
        std::vector<AnimationStore::Reference> stored;
        for (const auto& clip : actor.clips)
        {
            if (!IsTreeClip(clip))
                continue;
            auto found = FindAnimation(clip.animation);
            clipParts.push_back(found.parts);
            if (found.stored)
                stored.push_back(std::move(found.stored));
        }
        // the legs under clips of upper body animations and of its locomotion: the idle of each stance, and the
        // walks and runs, as its gun has them (a dog's: its breed's)
        const auto gunClass = GetGunClass(variant);
        Components::Actors::SetGunClass(actor.id, gunClass);
        const auto body = Components::Actors::GetBody(actor, gunClass);
        auto legsWanted = Components::Actors::GetLegsLibrary(actor, body);
        std::vector<std::string> legsAnimations;
        std::vector<const XAnimParts*> legsParts;
        for (const auto& legs : legsWanted)
            if (auto found = FindAnimation(legs); found.parts)
            {
                legsParts.push_back(found.parts);
                legsAnimations.push_back(legs);
                if (found.stored)
                    stored.push_back(std::move(found.stored));
            }
        // the spine rig, for clips (or legs) made for MP bodies (a dog's never are)
        const auto isMp = [](const XAnimParts* parts) {
            return parts && AnimationPose::ClassifyAnimation(*parts).family == Types::AnimationFamily::MpPlayer;
        };
        const bool mpAnimations = std::ranges::any_of(legsParts, isMp) || std::ranges::any_of(clipParts, isMp);
        const auto fitted = FitModels(actor.models, gun, leftGun, mpAnimations, stowedGun);
        Components::Actors::SetLeftOut(actor.id, fitted ? fitted->leftOut : Components::Actors::LeftOut{});
        if (!fitted || std::ranges::find(clipParts, nullptr) != clipParts.end())
            return false;
        auto models = fitted->models;
        const auto weaponModel = fitted->gun ? gun : nullptr;
        const auto handle = FindFreeHandle();
        if (models.empty() || !handle)
            return false;
        // the face's, for models with a face
        auto faceWanted = Components::Actors::GetFaceLibrary(actor);
        std::vector<std::string> faceAnimations;
        std::vector<const XAnimParts*> faceParts;
        if (!dog && HasFace(models))
            for (const auto& face : faceWanted)
                if (auto found = FindAnimation(face); found.parts)
                {
                    faceParts.push_back(found.parts);
                    faceAnimations.push_back(face);
                    if (found.stored)
                        stored.push_back(std::move(found.stored));
                }

        typedef XAnimTree_s*(__cdecl * XAnimCreateTree_t)(XAnim_s * anims, Alloc_t alloc);
        typedef void(__cdecl * XAnimFreeTree_t)(XAnimTree_s * tree, Free_t free, std::int32_t context);
        const auto& addresses = GetGameAddresses();

        slot.id = actor.id;
        slot.models = actor.models;
        slot.spineRig = fitted->spineRig;
        slot.clipIds = GetClipIds(actor);
        slot.running.clear();
        slot.killstreak = Components::KillstreakLogic::FindByModels(actor.models);
        if (const auto killstreak = slot.killstreak)
        {
            const auto& info = Components::KillstreakLogic::Get(*killstreak);
            for (const auto& effect :
                 Components::KillstreakLogic::GetEffects(info, Components::KillstreakLogic::GetLook(info, actor.models[0])))
                slot.running.push_back({std::string(effect.effect), std::string(effect.tag), 0, effect.row});
            slot.smoke = {.tag = std::string(Components::KillstreakLogic::DAMAGE_TAG)};
        }
        slot.clipAnimations = GetClipAnimations(actor);
        slot.clipParts = std::move(clipParts);
        slot.legsWanted = std::move(legsWanted);
        slot.legsAnimations = std::move(legsAnimations);
        slot.legsParts = std::move(legsParts);
        slot.faceWanted = std::move(faceWanted);
        slot.faceAnimations = std::move(faceAnimations);
        slot.faceParts = std::move(faceParts);
        slot.stored = std::move(stored);
        slot.gunClass = gunClass;
        slot.body = body;
        slot.spines.clear();
        for (std::size_t i = 0; i < slot.clipParts.size(); ++i)
            slot.spines[slot.clipAnimations[i]] = AnimationPose::GetSpineKind(*slot.clipParts[i]);
        for (std::size_t i = 0; i < slot.legsParts.size(); ++i)
            slot.spines[slot.legsAnimations[i]] = AnimationPose::GetSpineKind(*slot.legsParts[i]);
        slot.weights.assign(slot.clipParts.size() + slot.legsParts.size() + slot.faceParts.size(), 0.0f);
        slot.weapon = weapon;
        slot.variant = weaponModel ? variant : nullptr;
        slot.leftVariant = fitted->leftGun ? leftVariant : nullptr;
        slot.stowed = stowedWeapon;
        slot.stowedVariant = fitted->stowed ? stowedVariant : nullptr;
        slot.stowedModel = static_cast<std::uint32_t>(models.size() - 1);
        // (those come after the guns)
        const auto gunsEnd =
            static_cast<std::uint32_t>(models.size()) - (fitted->stowed ? 1u : 0u) - (fitted->stowedRig ? 1u : 0u);
        slot.leftGunModel = gunsEnd - 1;
        slot.weaponModel = gunsEnd - (fitted->leftGun ? 2 : 1);
        slot.magazineTag = weaponModel ? WeaponRig::FindMagazineTag(*weaponModel, *variant) : 0;
        slot.rig = slot.magazineTag ? WeaponRig::GetWeaponRig(weapon) : std::nullopt;
        if (!slot.rig || slot.rig->magazineBone.empty())
        {
            slot.magazineTag = 0;
            slot.rig.reset();
        }
        slot.handle = *handle;
        // the spine's entries, for an MP body (or a spine rig) with clips or legs made for SP bodies (the AI's movement
        // sets walk with SP animations under clips of the players' movement, which get the body its rig)
        std::optional<SpineRig> spine;
        for (std::size_t i = 0; i < models.size() && !spine; ++i)
            spine = AnimationPose::FindSpineRig(*models[i].model);
        slot.clipSpines.clear();
        for (const auto parts : slot.clipParts)
            slot.clipSpines.push_back(AnimationPose::GetSpineKind(*parts));
        const auto isLined = [](const XAnimParts* parts) {
            return AnimationPose::GetSpineKind(*parts) == SpineKind::Lined;
        };
        const auto linedClip = std::ranges::find_if(slot.clipParts, isLined);
        const auto linedLegs = std::ranges::find_if(slot.legsParts, isLined);
        const auto lined = linedClip != slot.clipParts.end()   ? *linedClip
                           : linedLegs != slot.legsParts.end() ? *linedLegs
                                                               : nullptr;
        if (spine && lined)
        {
            slot.linedSpine = MakeSpineAnimation(spine->lined.rotation, spine->lined.offset, *lined);
            slot.restingSpine = MakeSpineAnimation(spine->rest, glm::vec3(0.0f), *lined);
            // aimed ahead over each of the legs made for SP bodies, from where they turn j_mainroot on the body in the
            // middle of their play (an idle sways it a few degrees; at rest while that can't be decoded)
            slot.aimedSpines.clear();
            const auto bodyModel = models[slot.spineRig && models.size() > 1 ? 1 : 0].model;
            for (std::size_t i = 0; i < slot.legsParts.size(); ++i)
            {
                if (AnimationPose::GetSpineKind(*slot.legsParts[i]) == SpineKind::Moves)
                    continue;
                constexpr int SAMPLES = 8;
                std::vector<glm::quat> mainroots;
                for (int sample = 0; bodyModel && sample < SAMPLES; ++sample)
                {
                    const auto pose = AnimationPose::GetPose(slot.legsAnimations[i], bodyModel->name,
                                                             static_cast<float>(sample) / SAMPLES);
                    if (const auto bone = pose ? pose->FindBone("j_mainroot") : std::nullopt)
                        mainroots.push_back(pose->rotations[*bone]);
                }
                const auto aim = !mainroots.empty() ? PoseLogic::AimSpine(spine->pelvis, spine->rest, spine->spineAt,
                                                                          PoseLogic::AverageRotation(mainroots))
                                                    : PoseLogic::SpineLineUp{spine->rest, glm::vec3(0.0f)};
                slot.aimedSpines.emplace_back(i, MakeSpineAnimation(aim.rotation, aim.offset, *lined));
            }
        }
        else
        {
            slot.clipSpines.clear();
        }
        // (a killstreak whose clips are all motions gets a tree of no animations: the game only draws the bones its
        // controllers turn (see PoseVehicle) on a DObj with a tree)
        if (!slot.clipParts.empty() || slot.killstreak)
        {
            auto animations = slot.clipAnimations;
            animations.insert(animations.end(), slot.legsAnimations.begin(), slot.legsAnimations.end());
            const auto moving = animations.size();
            animations.insert(animations.end(), slot.faceAnimations.begin(), slot.faceAnimations.end());
            auto parts = slot.clipParts;
            parts.insert(parts.end(), slot.legsParts.begin(), slot.legsParts.end());
            parts.insert(parts.end(), slot.faceParts.begin(), slot.faceParts.end());
            if (slot.linedSpine)
            {
                parts.insert(parts.end(), {&slot.linedSpine->parts, &slot.restingSpine->parts});
                for (const auto& [legs, aimed] : slot.aimedSpines)
                    parts.push_back(&aimed->parts);
            }
            slot.anims = CreateAnimations(parts);
            slot.tree = reinterpret_cast<XAnimCreateTree_t>(addresses.XAnimCreateTree())(slot.anims, Alloc);

            // how far each animation moves in one play (x forward, y to the left), which paths are walked at and
            // the legs' movement animations played as fast as (the face's don't)
            for (std::size_t i = 0; i < moving; ++i)
            {
                if (Components::Actors::GetAnimationMotion(animations[i]))
                    continue;
                const auto parts =
                    i < slot.clipParts.size() ? slot.clipParts[i] : slot.legsParts[i - slot.clipParts.size()];
                float rotation[2] = {}, translation[3] = {};
                if (parts->delta)
                    Functions::XAnimGetRelDelta(slot.anims, FIRST_CLIP_INDEX + static_cast<std::uint32_t>(i), rotation,
                                                translation, 0.0f, 1.0f);
                Components::Actors::SetAnimationMotion(animations[i], {translation[0], translation[1]});
            }
            // and how the clips' move the root through a play, for clips that move the actor by it
            for (std::size_t i = 0; i < slot.clipParts.size(); ++i)
                if (slot.clipParts[i]->delta && !Components::Actors::GetAnimationRootMotion(animations[i]))
                    Components::Actors::SetAnimationRootMotion(
                        animations[i], MeasureRootMotion(slot.anims, FIRST_CLIP_INDEX + static_cast<std::uint32_t>(i),
                                                         *slot.clipParts[i]));
        }
        slot.obj = CreateDObj(models, slot.tree, slot.handle);
        if (!slot.obj)
        {
            if (slot.tree)
                reinterpret_cast<XAnimFreeTree_t>(addresses.XAnimFreeTree())(slot.tree, Free, 0);
            std::free(slot.anims);
            slot = {};
            return false;
        }
        HideWeaponParts(*slot.obj, slot);
        slot.head = FindHeadRig(*slot.obj);
        SetGrip(slot, 0, slot.obj);
        slot.vehicleBones.clear();
        slot.gunBones.clear();
        slot.spinBone.reset();
        if (slot.killstreak)
        {
            const auto& info = Components::KillstreakLogic::Get(*slot.killstreak);
            for (const auto& joint : Components::VehicleLogic::GetJoints(info, {}))
                slot.vehicleBones.push_back(Functions::DObjGetBoneIndex(slot.obj, GetScriptString(std::string(joint.bone))));
            for (const auto& gun : info.guns)
                slot.gunBones.push_back(
                    {FindGunBone(*slot.obj, gun.yaw), FindGunBone(*slot.obj, gun.pitch), FindGunBone(*slot.obj, gun.hold)});
            slot.spinBone = FindGunBone(*slot.obj, info.spin);
        }
        // what the entity held before (it is free, as its handle has no DObj)
        std::memset(&GetPose(slot), 0, CENTITY_SIZE);
        slot.active = true;
        return true;
    }

    // where an animation is at a time in seconds, from 0 to 1, as its clip plays it
    float GetAnimationTime(const XAnimParts& parts, const Types::ActorClip& clip, float time)
    {
        if (parts.numframes == 0 || parts.framerate <= 0.0f)
            return 0.0f;
        const auto duration = parts.numframes / parts.framerate;
        if (!clip.loop)
            return std::clamp(time / duration, 0.0f, 1.0f);
        const auto wrapped = std::fmod(time, duration);
        return (wrapped < 0.0f ? wrapped + duration : wrapped) / duration;
    }

    // where a looping animation is at a time in seconds, from 0 to 1
    float GetLoopingTime(const XAnimParts& parts, float time)
    {
        if (parts.numframes == 0 || parts.framerate <= 0.0f)
            return 0.0f;
        const auto duration = parts.numframes / parts.framerate;
        const auto wrapped = std::fmod(time, duration);
        return (wrapped < 0.0f ? wrapped + duration : wrapped) / duration;
    }

    // XAnimSetGoalWeight takes a weight under 0.001 (EQUAL_EPSILON) for 0, and the legs weigh a hundredth of the
    // clips (ActorLogic::LEGS_WEIGHT), so less than that as they blend away: the weights are given a hundred times as
    // they are, which is the same to the game, as it blends each bone by the ratio of the weights
    inline constexpr float WEIGHT_SCALE = 100.0f;

    // Weighs every entry of the tree by what the actor shows now (the clips', the legs' and the spine's, see
    // ActorLogic::WeighLayers) and sets the times of those shown. Entries keep their weight until it is set again, and
    // XAnimSetCompleteGoalWeight only weighs its entry and the root, not the entries next to it: so the entries not
    // shown are set to 0 (else, after the timeline went back from a later clip, that clip's last frame stayed blended
    // in), and those that weren't shown before and aren't now are left alone. The heaviest goes through the complete
    // variant, which keeps the root weighed. Entries play at rate 0, so only the times set here move them: at rate 1
    // the tree moved them on by part of a frame before they were drawn, and a tick didn't always show the same pose.
    ActorLogic::TreeWeights Animate(DObj* obj, XAnimTree_s* tree, const Built& slot, std::vector<float>& lastWeights,
                                    const Types::Actor& actor, std::uint32_t tick)
    {
        typedef void(__cdecl * XAnimSetGoalWeight_t)(DObj * obj, std::uint32_t index, float weight, float blendTime,
                                                     float rate, std::uint32_t notifyName, std::uint32_t notifyType,
                                                     std::int32_t restart, std::int32_t);
        typedef void(__cdecl * XAnimSetTime_t)(XAnimTree_s * tree, std::uint32_t index, float time, std::int32_t);
        const auto& addresses = GetGameAddresses();
        const auto XAnimSetCompleteGoalWeight =
            reinterpret_cast<XAnimSetGoalWeight_t>(addresses.XAnimSetCompleteGoalWeight());
        const auto XAnimSetGoalWeight = reinterpret_cast<XAnimSetGoalWeight_t>(addresses.XAnimSetGoalWeight());
        const auto XAnimSetTime = reinterpret_cast<XAnimSetTime_t>(addresses.XAnimSetTime());

        auto layers = Components::Actors::GetLayers(actor, tick, slot.body);
        if (legsLeftOut.contains(actor.id))
            layers.legs.clear();
        const auto shown = ActorLogic::WeighLayers(layers, [&](const std::string& animation) {
            const auto spine = slot.spines.find(animation);
            return spine != slot.spines.end() ? spine->second : SpineKind::AtRest;
        });

        // the clips' entries, then the legs', the face's and the spine's (see Build)
        const auto clips = slot.clipIds.size();
        const auto faces = clips + slot.legsAnimations.size();
        const auto animations = faces + slot.faceAnimations.size();
        std::vector<float> weights(animations, 0.0f), times(animations, 0.0f);
        for (const auto& entry : shown.animations)
        {
            std::size_t position = 0;
            if (entry.clip)
            {
                const auto clip = std::ranges::find(slot.clipIds, entry.clip->id);
                if (clip == slot.clipIds.end())
                    continue;
                position = static_cast<std::size_t>(clip - slot.clipIds.begin());
                times[position] = GetAnimationTime(*slot.clipParts[position], *entry.clip, entry.time);
            }
            else
            {
                const auto legs = std::ranges::find(slot.legsAnimations, entry.animation);
                if (legs == slot.legsAnimations.end())
                    continue;
                position = clips + static_cast<std::size_t>(legs - slot.legsAnimations.begin());
                times[position] = GetLoopingTime(*slot.legsParts[position - clips], entry.time);
            }
            weights[position] += entry.weight * WEIGHT_SCALE;
        }
        for (const auto& entry : shown.faces)
        {
            const auto face = std::ranges::find(slot.faceAnimations, entry.animation);
            if (face == slot.faceAnimations.end())
                continue;
            const auto position = faces + static_cast<std::size_t>(face - slot.faceAnimations.begin());
            const auto& parts = *slot.faceParts[position - faces];
            times[position] =
                entry.loop ? GetLoopingTime(parts, entry.time) : GetAnimationTime(parts, {.loop = false}, entry.time);
            weights[position] += entry.weight * WEIGHT_SCALE;
        }
        if (!slot.clipSpines.empty())
        {
            weights.push_back(shown.lined * WEIGHT_SCALE);
            weights.push_back(shown.resting * WEIGHT_SCALE);
            for (const auto& [legs, aimed] : slot.aimedSpines)
            {
                float weight = 0.0f;
                for (const auto& [animation, share] : shown.aimed)
                    if (animation == slot.legsAnimations[legs])
                        weight += share;
                weights.push_back(weight * WEIGHT_SCALE);
            }
        }
        lastWeights.resize(weights.size(), 0.0f);

        for (std::size_t position = 0; position < weights.size(); ++position)
        {
            if (weights[position] == 0.0f && lastWeights[position] != 0.0f)
                XAnimSetGoalWeight(obj, FIRST_CLIP_INDEX + static_cast<std::uint32_t>(position), 0.0f, 0.0f, 0.0f, 0, 0,
                                   0, -1);
        }
        const auto heaviest = static_cast<std::size_t>(
            std::ranges::max_element(weights.begin(), weights.begin() + animations) - weights.begin());
        for (std::size_t position = 0; position < weights.size(); ++position)
        {
            if (weights[position] <= 0.0f)
                continue;
            const auto index = FIRST_CLIP_INDEX + static_cast<std::uint32_t>(position);
            (position == heaviest ? XAnimSetCompleteGoalWeight : XAnimSetGoalWeight)(obj, index, weights[position],
                                                                                     0.0f, 0.0f, 0, 0, 0, -1);
            if (position < animations)
                XAnimSetTime(tree, index, times[position], -1);
        }
        lastWeights = std::move(weights);
        return shown;
    }

    glm::mat4 ToMatrix(const float (&axis)[3][3], const float (&origin)[3])
    {
        glm::mat4 matrix(1.0f);
        for (int i = 0; i < 3; ++i)
            matrix[i] = glm::vec4(axis[i][0], axis[i][1], axis[i][2], 0.0f);
        matrix[3] = glm::vec4(origin[0], origin[1], origin[2], 1.0f);
        return matrix;
    }

    // Puts a pose where a matrix is: its origin, and its angles from its axes (forward, left, up), as the game turns
    // angles into axes
    void SetPose(cpose_t& pose, const glm::mat4& matrix)
    {
        const glm::vec3 forward(matrix[0]), left(matrix[1]), up(matrix[2]);
        pose.origin[0] = matrix[3].x;
        pose.origin[1] = matrix[3].y;
        pose.origin[2] = matrix[3].z;
        pose.angles[0] = glm::degrees(std::atan2(-forward.z, std::hypot(forward.x, forward.y)));
        pose.angles[1] = glm::degrees(std::atan2(forward.y, forward.x));
        pose.angles[2] = glm::degrees(std::atan2(left.z, up.z));
    }

    // The magazine of a gun's world model, as a model of its own: a copy of the model that only draws the surfaces on
    // the magazine bone. Hiding the other bones would hide the magazine too, as hiding a bone hides the bones on it.
    // The copies go with the map's models.
    std::map<std::pair<const XModel*, std::uint16_t>, std::unique_ptr<XModel>> magazineModels;

    const XModel* GetMagazineModel(const XModel& gun, std::uint16_t tag)
    {
        const auto key = std::pair(&gun, tag);
        if (const auto it = magazineModels.find(key); it != magazineModels.end())
            return it->second.get();

        const auto bone = static_cast<std::uint32_t>(std::ranges::find(gun.boneNames, gun.boneNames + gun.numBones, tag) -
                                                     gun.boneNames);
        auto copy = std::make_unique<XModel>(gun);
        bool any = false;
        for (auto& lod : copy->lodInfo)
        {
            // the first run of the level's surfaces that are on the magazine bone alone
            std::uint16_t first = 0, count = 0;
            for (std::uint16_t i = lod.surfIndex; bone < gun.numBones && i < lod.surfIndex + lod.numsurfs; ++i)
            {
                if (WeaponRig::IsOnlyOnBone(gun.surfs[i], bone) && (count == 0 || first + count == i))
                {
                    first = count == 0 ? i : first;
                    ++count;
                }
            }
            lod.surfIndex = first;
            lod.numsurfs = count;
            any |= count > 0;
        }
        const auto model = any ? copy.get() : nullptr;
        magazineModels[key] = any ? std::move(copy) : nullptr;
        return model;
    }

    // Builds a DObj of the magazine of an actor's gun (see GetMagazineModel); false when it can't (a gun without a
    // magazine of its own isn't tried again)
    bool BuildMagazine(Built& slot, Built::Magazine& magazine)
    {
        if (magazine.obj)
            return true;
        const auto model =
            slot.obj && slot.magazineTag && slot.weaponModel < static_cast<std::uint32_t>(slot.obj->numModels)
                ? GetMagazineModel(*slot.obj->models[slot.weaponModel], slot.magazineTag)
                : nullptr;
        if (!model)
        {
            slot.magazineTag = 0;
            return false;
        }
        const auto handle = FindFreeHandle();
        if (!handle)
            return false;
        DObjModel_s models[] = {{const_cast<XModel*>(model), 0, false}};
        const auto obj = CreateDObj(models, nullptr, *handle);
        if (!obj)
            return false;
        std::memset(GetClientEntityPose(*handle), 0, CENTITY_SIZE);
        magazine = {*handle, obj};
        return true;
    }

    // Sets the left hand's grip of one of an actor's DObjs (see GripRig) from the gun in its hand. A dual wield gun's left
    // one has that hand already. None for a body without arms, or a gun with no grip on a soldier's arms.
    void SetGrip(Built& slot, std::size_t objIndex, DObj* obj)
    {
        const auto index = static_cast<std::size_t>(&slot - built.data());
        ClearGrip(index, objIndex);
        const auto variant = slot.variant;
        if (!obj || !variant || !variant->weapDef || slot.leftVariant || slot.killstreak)
            return;

        const auto find = [&](const char* name) { return Functions::DObjGetBoneIndex(obj, GetScriptString(name)); };
        const auto spine = find("j_spine4"), gun = find("tag_weapon_right");
        const auto left = std::array{find("j_clavicle_le"), find("j_shoulder_le"), find("j_elbow_le"), find("j_wrist_le")};
        const auto right = std::array{find("j_clavicle_ri"), find("j_shoulder_ri"), find("j_elbow_ri"), find("j_wrist_ri")};
        const auto found = [](const auto& bone) { return bone.has_value(); };
        if (!spine || !gun || !std::ranges::all_of(left, found) || !std::ranges::all_of(right, found))
            return;

        auto& rig = gripRigs[index * 2 + objIndex];
        rig.standing = {glm::make_vec3(variant->ikLeftHandOffset), glm::make_vec3(variant->ikLeftHandRotation)};
        rig.prone = {glm::make_vec3(variant->ikLeftHandProneOffset), glm::make_vec3(variant->ikLeftHandProneRotation)};
        rig.kind = GripLogic::GetGripKind(variant->weapDef->weapType, variant->weapDef->weapClass,
                                          variant->weapDef->offhandClass != 0);
        rig.gun = *gun;
        rig.shoulder = *left[1];
        rig.elbow = *left[2];
        rig.wrist = *left[3];

        const auto rest = [&](const std::array<std::optional<std::uint8_t>, 4>& arm) {
            const auto at = [&](std::size_t i) {
                return AnimationPose::GetRestPosition(*obj, *arm[i]).value_or(glm::vec3(0.0f));
            };
            return GripLogic::ArmRest{at(0), at(1), at(2), at(3)};
        };
        rig.leftArm = rest(left);
        rig.rightArm = rest(right);
        rig.otherArms = !GripLogic::IsSoldierArm(rig.leftArm, GripLogic::SOLDIER_LEFT_ARM) ||
                        !GripLogic::IsSoldierArm(rig.rightArm, GripLogic::SOLDIER_RIGHT_ARM);
        rig.leftParents = {*spine, *left[0], *left[1], *left[2]};
        rig.rightParents = {*spine, *right[0], *right[1], *right[2]};
        // (a gun with no grip still has a place on animations made for the AI)
        const auto hasPlace = rig.kind == GripLogic::GripKind::Gun || rig.kind == GripLogic::GripKind::Pistol;
        if (rig.standing.IsZero() && rig.prone.IsZero() && !rig.otherArms && !hasPlace)
            return;

        // the parts of the arm, in every model that has its bones: each bone is of its parent's part, but the ones that
        // start a part
        const auto shoulderTag = GetScriptString("j_shoulder_le"), elbowTag = GetScriptString("j_elbow_le"),
                   wristTag = GetScriptString("j_wrist_le"), twistTag = GetScriptString("j_wristtwist_le");
        const auto attachedTo = reinterpret_cast<const std::uint8_t*>(obj->models + obj->numModels);
        std::vector<GripPart> parts;
        for (int m = 0; m < obj->numModels; ++m)
        {
            const auto& model = *obj->models[m];
            const auto first = parts.size();
            parts.resize(first + model.numBones, GripPart::None);
            for (std::uint32_t b = 0; b < model.numBones; ++b)
            {
                auto part = GripPart::None;
                if (b >= model.numRootBones)
                {
                    const auto parent = static_cast<std::int32_t>(b) - model.parentList[b - model.numRootBones];
                    if (parent >= 0 && parent < static_cast<std::int32_t>(b))
                        part = parts[first + parent];
                }
                else if (m > 0 && attachedTo[m] < first)
                    part = parts[attachedTo[m]];
                const auto name = model.boneNames[b];
                part = name == shoulderTag ? GripPart::Upper
                       : name == elbowTag  ? GripPart::Forearm
                       : name == wristTag  ? GripPart::Hand
                       : name == twistTag  ? GripPart::Twist
                                           : part;
                parts[first + b] = part == GripPart::Twist && name != twistTag ? GripPart::Forearm : part;
            }
        }
        rig.bones = {};
        rig.movedCount = 0;
        for (std::size_t b = 0; b < parts.size() && b < MAX_DOBJ_BONES; ++b)
        {
            rig.bones[b / 32] |= 0x80000000u >> (b % 32);
            if (parts[b] == GripPart::None)
                continue;
            rig.moved[rig.movedCount] = static_cast<std::uint8_t>(b);
            rig.parts[rig.movedCount++] = parts[b];
        }
        rig.lying = false;
        rig.share = 1.0f;
        rig.campaign = 0.0f;
        rig.obj = obj;
        ++gripsSet;
    }

    void AskGripBones(const DObj* obj, int* partBits)
    {
        if (gripsSet <= 0 || !gripsOn)
            return;
        for (const auto& rig : gripRigs)
            if (rig.obj == obj)
            {
                for (std::size_t word = 0; word < rig.bones.size(); ++word)
                    partBits[word] |= static_cast<int>(rig.bones[word]);
                return;
            }
    }

    void GripGuns(DObj* obj, const std::array<std::uint32_t, 5>& composed)
    {
        if (gripsSet <= 0 || !gripsOn)
            return;
        for (const auto& rig : gripRigs)
        {
            if (rig.obj != obj)
                continue;
            // once, as the skeleton is put together, and only with the bones it needs
            const auto bit = [](std::uint8_t bone) { return 0x80000000u >> (bone % 32); };
            const auto done = [&](std::uint8_t bone) { return (obj->calcSkelBits[bone / 32] & bit(bone)) != 0; };
            if ((composed[rig.wrist / 32] & bit(rig.wrist)) || !done(rig.wrist) || !done(rig.elbow) ||
                !done(rig.shoulder) || !done(rig.gun))
                return;

            const auto read = [&](std::uint8_t bone) {
                const auto& mat = obj->skel[bone];
                return GripLogic::BonePose{glm::make_vec3(mat.trans),
                                           glm::normalize(glm::quat(mat.quat[3], mat.quat[0], mat.quat[1], mat.quat[2]))};
            };
            auto shift = glm::vec3(0.0f);
            if (rig.otherArms)
            {
                const auto parents = [&](const std::array<std::uint8_t, 4>& bones) {
                    return GripLogic::ArmParents{read(bones[0]).rotation, read(bones[1]).rotation, read(bones[2]).rotation,
                                                 read(bones[3]).rotation};
                };
                shift = GripLogic::GetWristShift(rig.leftArm, parents(rig.leftParents), rig.rightArm,
                                                 parents(rig.rightParents));
            }
            const auto& grip = GripLogic::PickGrip(rig.standing, rig.prone, rig.lying);
            const auto move = GripLogic::Solve(read(rig.shoulder), read(rig.elbow), read(rig.wrist), read(rig.gun), grip,
                                               rig.kind, shift, rig.share, gripPlaces ? rig.campaign.load() : 0.0f);
            if (!move)
                return;

            const auto twist = glm::angleAxis(glm::radians(move->twist), glm::vec3(1, 0, 0));
            for (std::size_t i = 0; i < rig.movedCount; ++i)
            {
                const auto part = rig.parts[i];
                const auto& by = part == GripPart::Upper  ? move->upper
                                 : part == GripPart::Hand ? move->hand
                                                          : move->forearm;
                auto& mat = obj->skel[rig.moved[i]];
                const auto position = GripLogic::Move(by, glm::make_vec3(mat.trans));
                // (the length of a blended rotation stays, which transWeight goes by)
                auto rotation = by.turn * glm::quat(mat.quat[3], mat.quat[0], mat.quat[1], mat.quat[2]);
                if (part == GripPart::Twist)
                    rotation *= twist;
                mat.trans[0] = position.x;
                mat.trans[1] = position.y;
                mat.trans[2] = position.z;
                mat.quat[0] = rotation.x;
                mat.quat[1] = rotation.y;
                mat.quat[2] = rotation.z;
                mat.quat[3] = rotation.w;
            }
            return;
        }
    }

    void SetGripsOn(bool on)
    {
        gripsOn = on;
    }

    void SetGripPlaces(bool on)
    {
        gripPlaces = on;
    }

    // Builds the actor without its gun's magazine: a second DObj of its models on its tree, with the magazine bone
    // hidden from the start; false when it can't
    bool BuildUnloaded(Built& slot)
    {
        const auto handle = FindFreeHandle();
        if (!handle || !slot.obj || !slot.variant)
            return false;
        auto models = GetModels(slot);
        const auto obj = CreateDObj(models, slot.tree, *handle);
        if (!obj)
            return false;
        HideWeaponParts(*obj, slot);
        if (const auto bone = Functions::DObjGetBoneIndex(obj, slot.magazineTag))
            obj->hidePartBits[*bone / 32] |= 0x80000000u >> (*bone % 32);
        std::memset(GetClientEntityPose(*handle), 0, CENTITY_SIZE);
        slot.unloadedHandle = *handle;
        slot.unloadedObj = obj;
        SetGrip(slot, 1, obj);
        return true;
    }

    // The ground where a ray from the origin hits the world: what it hits, or where it ends, and from there down
    std::optional<glm::vec3> FindGround(glm::vec3 origin, glm::vec3 direction)
    {
        if (glm::length(direction) < 0.001f)
            return std::nullopt;
        direction = glm::normalize(direction);

        auto trace = [](glm::vec3 start, glm::vec3 end, float* normalZ = nullptr) {
            trace_t result{};
            const float from[3] = {start.x, start.y, start.z};
            const float to[3] = {end.x, end.y, end.z};
            Functions::CG_LocationalTrace(&result, from, to, ENTITYNUM_NONE, MASK_SHOT_NO_CHARACTERS);
            if (normalZ)
                *normalZ = result.normal[2];
            return start + (end - start) * result.fraction;
        };
        // a little back from a wall, so the actor stands in front of it; where the ground is hit, right there
        float normalZ = 0.0f;
        auto hit = trace(origin, origin + direction * 4000.0f, &normalZ);
        constexpr float MIN_GROUND_NORMAL_Z = 0.7f;
        if (normalZ < MIN_GROUND_NORMAL_Z)
            hit -= direction * 16.0f;
        return trace(hit + glm::vec3(0, 0, 8), hit - glm::vec3(0, 0, 4000));
    }

    // The first surface a ray hits and its normal, for an effect put on it; none for
    // nothing within reach
    std::optional<std::pair<glm::vec3, glm::vec3>> FindSurface(glm::vec3 origin, glm::vec3 direction)
    {
        if (glm::length(direction) < 0.001f)
            return std::nullopt;
        const auto end = origin + glm::normalize(direction) * 8000.0f;
        trace_t result{};
        const float from[3] = {origin.x, origin.y, origin.z};
        const float to[3] = {end.x, end.y, end.z};
        Functions::CG_LocationalTrace(&result, from, to, ENTITYNUM_NONE, MASK_SHOT_NO_CHARACTERS);
        if (result.fraction >= 1.0f)
            return std::nullopt;
        return std::pair(origin + (end - origin) * result.fraction, glm::make_vec3(result.normal));
    }

    void HandlePlacementRequests()
    {
        // (core moves the actor there, walks it there or places a point of its path; an effect goes on the surface)
        for (const auto& request : Components::Actors::TakePlacementRequests())
        {
            if (request.surface)
            {
                if (const auto surface = FindSurface(request.origin, request.direction))
                    Components::Actors::Place(request, surface->first, surface->second);
            }
            else if (const auto ground = FindGround(request.origin, request.direction))
                Components::Actors::Place(request, *ground);
        }
    }

    // What core lists as the characters: the models named c_* the zones have loaded, sorted into bodies, heads and
    // what is worn on them, the animations (the store's too), and the packs; made again when the zones' or the
    // store's generation changes
    std::optional<std::uint32_t> catalogGeneration;
    std::optional<std::uint32_t> catalogStoreGeneration;

    Types::ActorCatalog::ModelKind Classify(std::string_view name)
    {
        using Kind = Types::ActorCatalog::ModelKind;
        constexpr std::array GEAR{"headwear", "hat",    "helmet", "ushanka", "pilotka", "beret", "cap",  "scarf",
                                  "gasmask",  "goggle", "hood",   "bandana", "glasses", "gear",  "mask", "beanie"};
        if (std::ranges::any_of(GEAR, [&](const char* part) { return name.find(part) != std::string_view::npos; }))
            return Kind::Gear;
        if (name.find("head") != std::string_view::npos)
            return Kind::Head;
        if (name.ends_with("_fb") || name.find("_fb_") != std::string_view::npos ||
            name.find("fullbody") != std::string_view::npos)
            return Kind::FullBody;
        return Kind::Body;
    }

    bool IsCharacterModel(std::string_view name)
    {
        // not the pieces bodies fall apart into, view models or arms
        constexpr std::array SKIP{"_g_", "gib", "viewmodel", "viewhands", "_vm_", "_arms"};
        return name.starts_with("c_") &&
               !std::ranges::any_of(SKIP, [&](const char* part) { return name.find(part) != std::string_view::npos; });
    }

    void BuildCatalog(const ContentPacks::PackState& packs)
    {
        auto catalog = std::make_shared<Types::ActorCatalog>();

        std::string packName;
        for (const auto& pack : ContentPacks::GetAvailablePacks())
        {
            catalog->packs.push_back({pack.name, pack.displayName, pack.group});
            if (pack.name == packs.loaded)
                packName = pack.displayName;
        }
        catalog->maxBones = MAX_DOBJ_BONES;
        catalog->loadedPack = packs.loaded;
        catalog->loadingPack = packs.loading;
        catalog->packError = packs.error;
        catalog->packMemoryMegabytes = packs.memoryMegabytes;

        const std::set<std::string, std::less<>> packModels(packs.models.begin(), packs.models.end());
        // what the campaign's characters use (their dogs, which aren't c_ models, among them)
        const auto& campaignKinds = Components::Actors::GetCampaignModelKinds();
        // the models of the character zones, which come and go, unlike the map's
        std::set<std::string, std::less<>> zoneModels;
        for (const auto& zone : ContentPacks::GetCharacterZones())
            zoneModels.insert(zone.models.begin(), zone.models.end());
        // what the catalog of every animation says of those loaded
        const auto& animations = Components::Actors::GetAnimationCatalog();
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
        // the map's zone: the one of its world, for the prop browser's This map's
        std::optional<std::uint8_t> mapZone;
        for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE && !mapZone; ++hash)
            for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
                if (entries[index].header && entries[index].type == ContentPacks::ASSET_TYPE_GFXWORLD)
                    mapZone = entries[index].zoneIndex;
        for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
        {
            for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
            {
                // not every type starts with its name; zone 0 has the stand-ins left for the assets of unloaded zones
                // (see ContentPacks.cpp)
                const auto& entry = entries[index];
                if (!entry.header || entry.zoneIndex == 0 ||
                    (entry.type != ASSET_TYPE_XMODEL && entry.type != ASSET_TYPE_XANIMPARTS &&
                     entry.type != ASSET_TYPE_FX))
                    continue;
                const std::string_view name = *static_cast<const char* const*>(entry.header);
                if (entry.type == ASSET_TYPE_XMODEL)
                {
                    catalog->props.emplace_back(name);
                    if (mapZone && entry.zoneIndex == *mapZone)
                        catalog->mapProps.emplace_back(name);
                }
                if (entry.type == ASSET_TYPE_XMODEL &&
                    (IsCharacterModel(name) || campaignKinds.contains(std::string(name)) ||
                     std::ranges::find(Components::CharacterLogic::MAP_DOGS, name) !=
                         Components::CharacterLogic::MAP_DOGS.end() ||
                     Components::KillstreakLogic::FindByModel(name)))
                {
                    const bool fromPack = packModels.contains(name);
                    const auto& model = *static_cast<const XModel*>(entry.header);
                    catalog->models.push_back({std::string(name), Classify(name),
                                               fromPack ? packName : std::string("Multiplayer"), model.numBones,
                                               ContentPacks::CanHaveFacePaint(model)});
                    // the spine rigs' MP body: the first of the map's with the MP spine (see GetSpineRig)
                    if (!spineSource && !fromPack && !zoneModels.contains(name) &&
                        name.find("_mp_body") != std::string_view::npos)
                    {
                        if (const auto chain = AnimationPose::FindSpineChain(model))
                        {
                            spineSource = &model;
                            AnimationPose::SetSpineChain(chain);
                        }
                    }
                }
                else if (entry.type == ASSET_TYPE_XANIMPARTS)
                {
                    const auto& parts = *static_cast<const XAnimParts*>(entry.header);
                    const auto duration =
                        parts.framerate > 0.0f ? static_cast<float>(parts.numframes) / parts.framerate : 0.0f;
                    std::vector<Types::ActorNotetrack> notetracks;
                    for (std::size_t i = 0; parts.notify && i < parts.notifyCount; ++i)
                        if (const auto note = Functions::SL_ConvertToString(parts.notify[i].name); !note.empty())
                            notetracks.push_back({std::string(note), std::clamp(parts.notify[i].time, 0.0f, 1.0f)});
                    std::ranges::stable_sort(notetracks, {}, &Types::ActorNotetrack::time);
                    const auto rig = AnimationPose::ClassifyAnimation(parts);
                    catalog->animations.push_back({std::string(name), duration, parts.loop, std::move(notetracks),
                                                   rig.kind, rig.family, animations.Find(name)});
                }
                else if (entry.type == ASSET_TYPE_FX)
                {
                    catalog->effects.emplace_back(name);
                    // how it plays by itself: FxEffectDef's looping life (ms, 0x7FFFFFFF till it is stopped) and how
                    // many of its elements loop and play once, at +0xC, +0x10 and +0x14 as IW3 lays it out
                    const auto def = static_cast<const std::uint8_t*>(entry.header);
                    const auto field = [&](std::size_t offset) {
                        return static_cast<std::uint32_t>(
                            std::max(*reinterpret_cast<const std::int32_t*>(def + offset), 0));
                    };
                    catalog->effectInfo[std::string(name)] = {field(0xC), field(0x10), field(0x14)};
                }
            }
        }
        // the catalog's animations the game hasn't registered (and the copies that differ from the game's), which the
        // store loads from the animation pack as actors play them
        catalog->animationStore = AnimationStore::IsReady();
        catalog->animationStoreError = AnimationStore::GetError();
        if (catalog->animationStore)
        {
            // (added after, as the names are looked at)
            std::unordered_set<std::string_view> registered;
            for (const auto& animation : catalog->animations)
                registered.insert(animation.name);
            std::vector<Types::ActorCatalog::Animation> stored;
            for (const auto& entry : animations.entries)
                if (!registered.contains(entry.name) && AnimationStore::Has(entry.name))
                    stored.push_back({entry.name, entry.Duration(), entry.loop, entry.notetracks, entry.kind,
                                      entry.family, &entry, true});
            catalog->animations.insert(catalog->animations.end(), std::make_move_iterator(stored.begin()),
                                       std::make_move_iterator(stored.end()));
        }
        std::ranges::sort(catalog->models, {}, &Types::ActorCatalog::Model::name);

        // the characters: the map's own (made of its multiplayer models), then the campaign's and Zombies'; a model is
        // what the characters use it as (a bandage is worn, a Project Nova body is a body), else what its name says
        std::vector<std::string> modelNames;
        for (const auto& model : catalog->models)
            modelNames.push_back(model.name);
        catalog->characters = Components::CharacterLogic::MakeMapCharacters(modelNames);
        const auto mapKinds = Components::CharacterLogic::GetModelKinds(catalog->characters);
        const auto& campaign = Components::Actors::GetCampaignCharacters();
        catalog->characters.insert(catalog->characters.end(), campaign.characters.begin(), campaign.characters.end());
        catalog->levels = campaign.levels;
        for (const auto& zone : ContentPacks::GetCharacterZones())
            catalog->loadedZones.push_back(zone.name);
        catalog->zoneError = ContentPacks::GetCharacterZoneError();
        for (auto& model : catalog->models)
        {
            if (const auto kind = mapKinds.find(model.name); kind != mapKinds.end())
                model.kind = kind->second;
            else if (const auto campaignKind = campaignKinds.find(model.name); campaignKind != campaignKinds.end())
                model.kind = campaignKind->second;
        }

        std::ranges::sort(catalog->animations, {}, &Types::ActorCatalog::Animation::name);
        std::ranges::sort(catalog->effects);
        std::ranges::sort(catalog->props);
        std::ranges::sort(catalog->mapProps);
        Components::Actors::SetCatalog(std::move(catalog));
    }

    void* FindEffect(const std::string& name)
    {
        return name.empty() ? nullptr : ContentPacks::FindAsset(ASSET_TYPE_FX, name.c_str());
    }

    bool IsEffectPlaying(std::int32_t handle)
    {
        // FX_EffectFromHandle: null for a handle of an effect that is gone
        typedef void*(__cdecl * FX_EffectFromHandle_t)(void* system, std::int32_t handle);
        const auto system = *reinterpret_cast<void**>(GetGameAddresses().fx_system());
        return handle != 0 && system &&
               reinterpret_cast<FX_EffectFromHandle_t>(GetGameAddresses().FX_EffectFromHandle())(system, handle);
    }

    // the handles of the effects actors started, every one stopped when the demo rewinds
    // (a cap of 512 left the older ones of a long stretch of playback playing). Those that ended by themselves are
    // forgotten whenever the list has grown by MIN_EFFECT_HANDLES since (a handle carries a generation, so one of an
    // effect that is gone stops nothing).
    std::vector<std::int32_t> effectHandles;
    std::size_t effectHandlesForgetAt = 0;
    inline constexpr std::size_t MIN_EFFECT_HANDLES = 256;

    void RememberEffect(std::int32_t handle)
    {
        if (!handle)
            return;
        effectHandles.push_back(handle);
        if (effectHandles.size() < effectHandlesForgetAt)
            return;
        std::erase_if(effectHandles, [](std::int32_t remembered) { return !IsEffectPlaying(remembered); });
        effectHandlesForgetAt = effectHandles.size() + MIN_EFFECT_HANDLES;
    }

    void ForgetSceneEffects();

    void StopEffects()
    {
        for (const auto handle : effectHandles)
            Functions::FX_KillEffect(0, handle);
        effectHandles.clear();
        effectHandlesForgetAt = 0;
        // (the scene's effects among them: spawned again as the timeline has them)
        ForgetSceneEffects();
    }

    std::size_t CountLiveEffects()
    {
        return static_cast<std::size_t>(std::ranges::count_if(effectHandles, IsEffectPlaying));
    }

    std::vector<std::optional<glm::vec3>> GetBonePositions(std::uint32_t id, const std::vector<std::string>& bones)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        if (slot == built.end() || !slot->obj)
            return {};
        std::vector<std::optional<glm::vec3>> positions;
        for (const auto& bone : bones)
        {
            float axis[3][3] = {}, origin[3] = {};
            if (Functions::CG_DObjGetWorldTagMatrix(&GetPose(*slot), slot->obj, GetScriptString(bone), axis, origin))
                positions.emplace_back(glm::vec3(origin[0], origin[1], origin[2]));
            else
                positions.emplace_back(std::nullopt);
        }
        return positions;
    }

    std::vector<std::optional<glm::mat4>> GetBoneMatrices(std::uint32_t id, const std::vector<std::string>& bones)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        if (slot == built.end() || !slot->obj)
            return {};
        std::vector<std::optional<glm::mat4>> matrices;
        for (const auto& bone : bones)
        {
            float axis[3][3] = {}, origin[3] = {};
            if (Functions::CG_DObjGetWorldTagMatrix(&GetPose(*slot), slot->obj, GetScriptString(bone), axis, origin))
                matrices.emplace_back(ToMatrix(axis, origin));
            else
                matrices.emplace_back(std::nullopt);
        }
        return matrices;
    }

    glm::mat4 AnglesToMatrix(const glm::vec3& angles);

    std::optional<glm::mat4> GetDrawnPlacement(std::uint32_t id)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        if (slot == built.end() || !slot->obj)
            return std::nullopt;
        const auto& pose = GetPose(*slot);
        return glm::translate(glm::mat4(1.0f), glm::make_vec3(pose.origin)) *
               AnglesToMatrix(glm::make_vec3(pose.angles));
    }

    std::string DescribeAnimationBones(const std::string& animation)
    {
        const auto found = AnimationStore::FindLoaded(animation);
        const auto parts = found.parts;
        if (!parts)
            return "not loaded";
        std::string text = std::format("counts");
        for (const auto count : parts->boneCount)
            text += std::format(" {}", count);
        std::size_t index = 0;
        for (int type = 0; type < 5; ++type)
        {
            text += std::format(" | rot{}:", type);
            for (std::uint8_t i = 0; i < parts->boneCount[type] && parts->names; ++i, ++index)
                text += std::format(" {}", Functions::SL_ConvertToString(parts->names[index]));
        }
        return text;
    }

    float FindGroundBelow(glm::vec3 point);

    std::optional<MagazineState> GetMagazineState(std::uint32_t id)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        if (slot == built.end() || !slot->obj)
            return std::nullopt;
        MagazineState state{std::string(Functions::SL_ConvertToString(slot->magazineTag)), slot->magazineOffGun};
        state.reload = slot->reloadDrawn;
        if (!slot->rig)
            return state;
        const auto& rig = *slot->rig;
        if (slot->heldDrawn != ActorLogic::HeldMagazine::None)
        {
            state.held = slot->heldDrawn == ActorLogic::HeldMagazine::Old ? "old" : "new";
            state.heldMagazine = slot->heldTransform * rig.magazine;
            float axis[3][3] = {}, origin[3] = {};
            if (Functions::CG_DObjGetWorldTagMatrix(&GetPose(*slot), slot->obj, slot->magazineTag, axis, origin))
                state.heldToGun = glm::distance(glm::make_vec3(origin), glm::vec3((*state.heldMagazine)[3]));
        }
        const auto& drawn = GetPose(*slot);
        for (const auto& transform : slot->droppedDrawn)
        {
            const auto middle =
                glm::vec3(transform * glm::vec4(glm::vec3(rig.magazine[3]) + ReloadLogic::MAGAZINE_MIDDLE, 1.0f));
            state.dropped.push_back(middle.z - FindGroundBelow(middle));
            state.droppedFromActor.push_back(glm::distance(glm::vec2(middle), glm::vec2(glm::make_vec3(drawn.origin))));
            if (!state.droppedMagazine)
                state.droppedMagazine = transform * rig.magazine;
        }
        return state;
    }

    // plays an effect on a bone of the actor, which goes where the bone goes; returns its handle, 0 for none
    std::int32_t PlayBoltedEffect(const Built& slot, void* effect, const char* tag)
    {
        // returns the effect's handle, from FX_PlayBoltedEffect
        typedef std::int32_t(__cdecl * CG_PlayBoltedEffect_t)(std::int32_t localClientNum, void* effect,
                                                              std::int32_t handle, std::uint32_t tagName);
        if (!effect)
            return 0;
        // only bones it has; the game complains about the others
        const auto tagName = GetScriptString(tag);
        if (!Functions::DObjGetBoneIndex(slot.obj, tagName))
            return 0;
        const auto handle = reinterpret_cast<CG_PlayBoltedEffect_t>(GetGameAddresses().CG_PlayBoltedEffect())(
            0, effect, slot.handle, tagName);
        RememberEffect(handle);
        return handle;
    }

    // A running effect played while wanted: started again whenever it isn't playing (it is new, the demo rewound, which
    // stops the actors' effects, or it ended), on its bone; stopped once it isn't wanted
    void KeepRunning(Built& slot, Built::RunningEffect& running, bool wanted)
    {
        if (!wanted)
        {
            if (running.handle && IsEffectPlaying(running.handle))
                Functions::FX_KillEffect(0, running.handle);
            running.handle = 0;
        }
        else if (!IsEffectPlaying(running.handle))
            running.handle = PlayBoltedEffect(slot, FindEffect(running.effect), running.tag.c_str());
    }

    // A killstreak's running effects while it is shown, those of the rows of its effects it has on (see
    // KillstreakLogic::EffectRow), but its tail rotor once a spin out broke it (the damaged one is the death's), and
    // its engine smoke from the tick it is Damaged; all stopped once it isn't shown
    void RunEffects(Built& slot, bool shown, const Types::Actor* actor = nullptr, std::uint32_t tick = 0)
    {
        namespace KillstreakLogic = Components::KillstreakLogic;
        const bool broken = actor && KillstreakLogic::HasBrokenTailRotor(*actor, tick);
        for (auto& running : slot.running)
            KeepRunning(slot, running,
                        shown && actor && KillstreakLogic::IsEffectRowOn(*actor, running.row) &&
                            !(broken && running.row == KillstreakLogic::EffectRow::Rotors &&
                              running.tag == "tail_rotor_jnt"));
        // (light or heavy: the other stops first)
        const auto smoke =
            shown && actor && tick >= actor->damagedFrom && slot.killstreak &&
                    Components::KillstreakLogic::CanBeDamaged(Components::KillstreakLogic::Get(*slot.killstreak))
                ? Components::KillstreakLogic::GetDamageEffect(actor->damage)
                : std::string_view();
        if (slot.smoke.effect != smoke)
        {
            KeepRunning(slot, slot.smoke, false);
            slot.smoke.effect = smoke;
        }
        KeepRunning(slot, slot.smoke, !smoke.empty());
    }

    void StopRunningEffects(Built& actor)
    {
        RunEffects(actor, false);
    }

    std::vector<std::string> GetRunningEffects(std::uint32_t id)
    {
        std::vector<std::string> playing;
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        if (slot == built.end())
            return playing;
        for (const auto& running : slot->running)
            if (IsEffectPlaying(running.handle))
                playing.push_back(running.effect + "@" + running.tag);
        if (IsEffectPlaying(slot->smoke.handle))
            playing.push_back(slot->smoke.effect + "@" + slot->smoke.tag);
        return playing;
    }

    // plays an effect at a point of the world as the game's playfx plays its own there (its helicopters' dust,
    // impacts): no entity, bone or mark, facing as playfx does when told no way, straight up whatever the ground (its
    // angles -90 0 0, or turned as the axes given), let go of so it ends by itself
    // (spawned with FX_SpawnEffect and kept, a looping one played until it was killed: the rotor wash's dust filled the
    // game's 1024 effects); its handle kept to stop it on a rewind, 0 for none
    constexpr float PLAYFX_AXIS[3][3] = {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}};
    std::int32_t PlayEffectAt(void* effect, glm::vec3 point, const float (&axis)[3][3] = PLAYFX_AXIS,
                              std::optional<std::int32_t> msecBegin = std::nullopt)
    {
        const auto cg = Structures::GetClientGlobals();
        if (!effect || !cg)
            return 0;
        const float origin[3] = {point.x, point.y, point.z};
        const auto handle = Functions::FX_SpawnWorldEffect(effect, origin, axis, msecBegin.value_or(cg->time), false);
        RememberEffect(handle);
        return handle;
    }

    // the dust and treads killstreaks played, and the events' effects that stayed in the world, by effect, for the
    // harness
    std::map<std::string, std::size_t> extraEffects;
    // and which way each last faced
    std::map<std::string, glm::vec3> extraForwards;

    void CountExtraEffect(const std::string& effect, const float (&axis)[3][3])
    {
        ++extraEffects[effect];
        extraForwards[effect] = glm::make_vec3(axis[0]);
    }

    std::optional<glm::vec3> GetKillstreakEffectForward(const std::string& part)
    {
        for (const auto& [effect, forward] : extraForwards)
        {
            if (effect.find(part) != std::string::npos)
                return forward;
        }
        return std::nullopt;
    }

    std::map<std::string, std::size_t> GetKillstreakEffectCounts(bool reset)
    {
        auto counts = extraEffects;
        if (reset)
        {
            extraEffects.clear();
            extraForwards.clear();
        }
        return counts;
    }

    // what a trace straight down from a point hits: the ground's height and surface, none for nothing or an entity
    struct Ground
    {
        glm::vec3 point{0.0f};
        std::uint32_t surface = 0;
    };
    std::optional<Ground> TraceGround(glm::vec3 from, float down)
    {
        trace_t trace{};
        const float start[3] = {from.x, from.y, from.z};
        const float end[3] = {from.x, from.y, from.z - down};
        Functions::CG_LocationalTrace(&trace, start, end, ENTITYNUM_NONE, MASK_SHOT_NO_CHARACTERS);
        if (trace.fraction >= 1.0f || trace.hitId != ENTITYNUM_WORLD)
            return std::nullopt;
        return Ground{from - glm::vec3(0.0f, 0.0f, down * trace.fraction), (trace.surfaceFlags >> 20) & 0x1F};
    }

    // A killstreak's effects of the timeline's ticks between two (the first left out), as it plays on: a helicopter's
    // dust at the ground under it (its Rotor wash), the RC-XD's treads
    void PlayKillstreakTicks(Built& slot, const Types::Actor& actor, std::uint32_t from, std::uint32_t to)
    {
        namespace KillstreakLogic = Components::KillstreakLogic;
        namespace VehicleLogic = Components::VehicleLogic;
        if (!slot.killstreak)
            return;
        const auto& info = KillstreakLogic::Get(*slot.killstreak);
        if (info.dust && KillstreakLogic::IsEffectRowOn(actor, KillstreakLogic::EffectRow::RotorWash))
        {
            for (auto tick = (from / VehicleLogic::DUST_STEP + 1) * VehicleLogic::DUST_STEP; tick <= to;
                 tick += VehicleLogic::DUST_STEP)
            {
                // (from under its body, so it doesn't hit itself)
                const auto at = Components::Actors::EvaluatePlacement(actor, tick).position;
                const auto ground = TraceGround(at - glm::vec3(0.0f, 0.0f, 16.0f), 100000.0f);
                if (!ground || !VehicleLogic::IsDustTick(tick, at.z - ground->point.z))
                    continue;
                const std::string effect(KillstreakLogic::GetDustEffect(info, ground->surface));
                if (PlayEffectAt(FindEffect(effect), ground->point))
                    CountExtraEffect(effect, PLAYFX_AXIS);
            }
        }
        if (info.vehicleClass == Types::VehicleClass::Car &&
            KillstreakLogic::IsEffectRowOn(actor, KillstreakLogic::EffectRow::Treads))
        {
            for (auto tick = (from / VehicleLogic::TREAD_STEP + 1) * VehicleLogic::TREAD_STEP; tick <= to;
                 tick += VehicleLogic::TREAD_STEP)
            {
                const auto body = Components::Actors::GetVehicleBody(actor, tick);
                const auto ground = TraceGround(
                    Components::Actors::EvaluatePlacement(actor, tick).position + glm::vec3(0, 0, 8), 40.0f);
                const auto surface = ground ? KillstreakLogic::GetTreadSurface(ground->surface) : std::string_view();
                if (!body || surface.empty())
                    continue;
                for (const auto tread : VehicleLogic::GetTreads(info, *body))
                {
                    const auto effect = VehicleLogic::GetTreadEffect(surface, tread);
                    if (PlayBoltedEffect(slot, FindEffect(effect), "tag_origin"))
                        ++extraEffects[effect];
                }
            }
        }
    }

    // the shots fired, by the bone they came from, for the harness
    std::map<std::string, std::size_t> muzzleCounts;
    // what bullets left where they landed, for the harness
    std::map<std::string, std::size_t> hitCounts;
    std::optional<glm::vec3> lastHit;

    // A weapon as the game's impact table knows it: its index, or for a weapon of a zone of its own, which has none, the
    // index of one of the map's whose bullets are the same; kept by the variant until the map changes
    std::unordered_map<const WeaponVariantDef*, std::uint32_t> impactWeapons;

    std::uint32_t GetImpactWeapon(const WeaponVariantDef& variant)
    {
        if (const auto known = impactWeapons.find(&variant); known != impactWeapons.end())
            return known->second;
        const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        const auto count = Functions::GetWeaponVariantCount();
        std::uint32_t found = 0;
        for (std::size_t index = 1; index < count && !found; ++index)
            if (variants[index] == &variant)
                found = static_cast<std::uint32_t>(index);
        for (std::size_t index = 1; index < count && !found && variant.weapDef; ++index)
            if (variants[index] && variants[index]->weapDef &&
                variants[index]->weapDef->impactType == variant.weapDef->impactType)
                found = static_cast<std::uint32_t>(index);
        return impactWeapons[&variant] = found;
    }

    // Where a shot lands: what the game's bullets see (the world and the demo's players), or a body of another actor
    // before that
    struct ShotEnd
    {
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
        bool hit = false;              // on something, not in the air at the end of its range
        std::uint32_t surface = 0;     // the game's surface type
        std::int32_t entity = ENTITYNUM_WORLD;
        const Built* actor = nullptr;  // another actor's body
        std::uint32_t bone = TRACE_NO_BONE;  // of a body, in its DObj
    };

    // the first of the bodies of other actors a line meets, as far as it goes
    std::optional<std::pair<const Built*, ShotLogic::BodyHit>> FindActorHit(const Built& shooter, const glm::vec3& start,
                                                                            const glm::vec3& direction, float length)
    {
        std::optional<std::pair<const Built*, ShotLogic::BodyHit>> nearest;
        for (auto& slot : built)
        {
            // (a person's: a killstreak's or a dog's bones aren't a person's)
            if (!slot.active || !slot.posed || !slot.obj || &slot == &shooter ||
                slot.body.kind != Types::ActorKind::Human)
                continue;
            auto& pose = GetPose(slot);
            const glm::vec3 middle(pose.origin[0], pose.origin[1], pose.origin[2] + 36.0f);
            if (!ShotLogic::PassesNear(start, direction, length, middle, ShotLogic::BODY_REACH))
                continue;
            std::vector<ShotLogic::Capsule> capsules;
            const auto bone = [&](std::string_view name) -> std::optional<glm::vec3> {
                float axis[3][3] = {}, origin[3] = {};
                if (!Functions::CG_DObjGetWorldTagMatrix(&pose, slot.obj, GetScriptString(std::string(name)), axis, origin))
                    return std::nullopt;
                return glm::make_vec3(origin);
            };
            for (std::size_t part = 0; part < ShotLogic::BODY_PARTS.size(); ++part)
            {
                const auto& body = ShotLogic::BODY_PARTS[part];
                const auto from = bone(body.from), to = bone(body.to);
                if (from && to)
                    capsules.push_back({*from, *to, body.radius, part});
            }
            const auto hit = ShotLogic::FindBodyHit(start, direction, nearest ? nearest->second.distance : length, capsules);
            if (hit)
                nearest = std::pair(&slot, *hit);
        }
        return nearest;
    }

    ShotEnd TraceShot(const Built& shooter, const float (&muzzle)[3], const glm::vec3& direction)
    {
        trace_t trace{};
        const glm::vec3 start = glm::make_vec3(muzzle);
        const auto reach = start + direction * ShotLogic::SHOT_RANGE;
        const float end[3] = {reach.x, reach.y, reach.z};
        // (as a player's bullet asks it, for the bone of a body hit)
        Functions::CG_LocationalTrace(&trace, muzzle, end, ENTITYNUM_NONE, MASK_SHOT, true);

        ShotEnd shot{start + direction * ShotLogic::SHOT_RANGE * trace.fraction};
        if (trace.fraction < 1.0f)
        {
            shot.hit = true;
            shot.normal = glm::make_vec3(trace.normal);
            shot.surface = (trace.surfaceFlags >> 20) & 0x3F;
            shot.entity = trace.hitId;
            shot.bone = trace.partBone;
            // (a player has no surface: the game's bullets say flesh for what is alive, BulletTrace)
            const auto entities = Structures::GetEntities();
            if (trace.hitId != ENTITYNUM_WORLD && trace.surfaceFlags == 0 && entities && trace.hitId < MAX_GENTITIES &&
                ShotLogic::IsFleshEntity(static_cast<std::int32_t>(entities[trace.hitId].nextState.eType)))
                shot.surface = ShotLogic::SURFACE_FLESH;
        }
        if (const auto actor = FindActorHit(shooter, start, direction, ShotLogic::SHOT_RANGE * trace.fraction))
        {
            shot.point = actor->second.point;
            shot.normal = actor->second.normal;
            shot.hit = true;
            shot.surface = ShotLogic::SURFACE_FLESH;
            shot.entity = ENTITYNUM_NONE;
            shot.actor = actor->first;
            const auto part = ShotLogic::BODY_PARTS[actor->second.part];
            const auto bone = Functions::DObjGetBoneIndex(actor->first->obj, GetScriptString(std::string(part.to)));
            shot.bone = bone ? *bone : TRACE_NO_BONE;
        }
        return shot;
    }

    // What the game's bullets leave where they land (CG_BulletHitEvent, 0x7FD4A0): the effect of its impact table for the
    // weapon and the surface, marks on what it hit (they are the effect's), blood kept on the bone of a body, the sound.
    void PlayHit(const ShotEnd& shot, const WeaponVariantDef& weapon, std::uint32_t tick)
    {
        const auto cg = Structures::GetClientGlobals();
        if (!cg || !shot.hit || !ShotLogic::HasRoomForHit(Hooks::Effects::CountEffectsInUse()))
            return;
        void* effect = nullptr;
        std::uint32_t sound = 0;
        Functions::CG_GetImpactEffect(GetImpactWeapon(weapon), shot.surface, 0, effect, sound);
        const float point[3] = {shot.point.x, shot.point.y, shot.point.z};
        if (sound)
        {
            typedef std::int32_t(__cdecl * CG_PlaySoundAlias_t)(std::int32_t localClientNum, std::int32_t entnum,
                                                                const float* origin, std::int32_t, std::int32_t,
                                                                float volume, std::uint32_t aliasHash);
            reinterpret_cast<CG_PlaySoundAlias_t>(GetGameAddresses().CG_PlaySoundAlias())(0, ENTITYNUM_WORLD, point, 0, 0,
                                                                                          1.0f, sound);
        }
        const auto kind = shot.actor                       ? "actor"
                          : shot.entity == ENTITYNUM_WORLD ? "world"
                                                           : "entity";
        ++hitCounts[std::format("{} surface {} {}{}", kind, shot.surface,
                                effect ? *static_cast<const char* const*>(effect) : "no effect",
                                shot.bone != TRACE_NO_BONE ? " on a bone" : "")];
        lastHit = shot.point;
        if (!effect)
            return;
        const auto axes = ShotLogic::GetImpactAxes(shot.normal, ShotLogic::GetImpactTurn(tick));
        const float axis[3][3] = {{axes.forward.x, axes.forward.y, axes.forward.z},
                                  {axes.left.x, axes.left.y, axes.left.z},
                                  {axes.up.x, axes.up.y, axes.up.z}};
        // blood on a body stays on its bone, whoever's it is
        const auto handle = shot.actor ? shot.actor->handle : shot.entity;
        if (shot.surface == ShotLogic::SURFACE_FLESH && shot.bone != TRACE_NO_BONE)
            RememberEffect(Functions::FX_PlayEffectOnBone(effect, cg->time, handle, shot.bone, point, axis));
        else
            RememberEffect(Functions::FX_SpawnWorldEffect(effect, point, axis, cg->time, false,
                                                          static_cast<std::uint32_t>(shot.entity)));
    }

    std::map<std::string, std::size_t> GetHitCounts(bool reset)
    {
        auto counts = hitCounts;
        if (reset)
        {
            hitCounts.clear();
            lastHit.reset();
        }
        return counts;
    }

    std::optional<glm::vec3> GetLastHit()
    {
        return lastHit;
    }

    // A launcher's rocket in the air: where it goes, and what it lands on
    // what a missile homes on: an actor's body or a player (the SAM's)
    struct HomingTarget
    {
        Types::BoneCameraTarget target;
        std::string bone;
    };

    struct FlyingRocket
    {
        ShotLogic::RocketFlight flight;
        std::uint32_t actor = 0;
        const WeaponVariantDef* variant = nullptr;
        ShotEnd end;
        glm::mat3 rotation{1.0f};
        std::int32_t trail = 0;  // its effect, playing
        bool blast = true;       // it bursts where it lands (the Hits box of its Fire)
        std::optional<HomingTarget> homing;
    };
    std::vector<FlyingRocket> rockets;
    std::optional<std::uint32_t> rocketsTick;

    std::vector<std::pair<std::string, glm::vec3>> GetRockets()
    {
        std::vector<std::pair<std::string, glm::vec3>> flying;
        for (const auto& rocket : rockets)
            flying.emplace_back(rocket.variant->weapDef->projectileModel->name,
                                ShotLogic::GetRocketPoint(rocket.flight, rocketsTick.value_or(0)).value_or(glm::vec3(0.0f)));
        return flying;
    }

    // the bones of a dual wield gun's left one its shots come from
    constexpr auto LEFT_GUN_FLASH = "tag_flash1";
    constexpr auto LEFT_GUN_BRASS = "tag_brass1";

    constexpr auto DEFAULT_EFFECT_TAG = "tag_origin";

    // A shot of the gun in an actor's hand, as others see a player's: the muzzle flash and casing on the gun's tags,
    // the sound at the muzzle, and a tracer from it to what the barrel points at
    // A shot's sound and tracer from a muzzle bone, along it; and of a bullet (of that weapon) what it leaves where it lands
    void Shoot(Built& slot, const char* flash, const char* sound, bool tracers, const WeaponVariantDef* bullet,
               std::uint32_t tick, float chosenSpread)
    {
        typedef std::int32_t(__cdecl * CG_PlaySoundAlias_t)(std::int32_t localClientNum, std::int32_t entnum,
                                                            const float* origin, std::int32_t, std::int32_t,
                                                            float volume, std::uint32_t aliasHash);
        typedef std::uint32_t(__cdecl * SND_HashName_t)(const char* name);
        typedef void(__cdecl * CG_SpawnTracer_t)(std::int32_t localClientNum, const float* start, const float* end,
                                                 std::int32_t, float, float);
        const auto& addresses = GetGameAddresses();
        float axis[3][3] = {}, muzzle[3] = {};
        if (!Functions::CG_DObjGetWorldTagMatrix(&GetPose(slot), slot.obj, GetScriptString(flash), axis, muzzle))
            return;
        if (sound && *sound)
            reinterpret_cast<CG_PlaySoundAlias_t>(addresses.CG_PlaySoundAlias())(
                0, ENTITYNUM_WORLD, muzzle, 0, 0, 1.0f,
                reinterpret_cast<SND_HashName_t>(addresses.SND_HashName())(sound));
        if (!tracers && !bullet)
            return;
        // each pellet its own way; the tracer goes with the first
        const ShotLogic::Axes barrel{glm::make_vec3(axis[0]), glm::make_vec3(axis[1]), glm::make_vec3(axis[2])};
        const auto spread =
            bullet && bullet->weapDef
                ? ShotLogic::GetSpread(bullet->weapDef->shotCount, bullet->weapDef->adsSpread, chosenSpread)
                : ShotLogic::GetSpread(1, 0.0f, chosenSpread);
        for (std::uint32_t pellet = 0; pellet < spread.pellets; ++pellet)
        {
            const auto shot = TraceShot(slot, muzzle, ShotLogic::GetPelletDirection(barrel, tick, pellet, spread.degrees));
            if (tracers && pellet == 0)
            {
                const float hit[3] = {shot.point.x, shot.point.y, shot.point.z};
                reinterpret_cast<CG_SpawnTracer_t>(addresses.CG_SpawnTracer())(0, muzzle, hit, 0, 0.0f, 0.0f);
            }
            if (bullet)
                PlayHit(shot, *bullet, tick);
        }
    }

    // sends a launcher's rocket from its muzzle along the barrel, within its spread (PlayRockets flies it); with no blast
    // when the shot's hits are off
    void LaunchRocket(Built& slot, const Types::Actor& actor, const char* flash, const WeaponVariantDef& launcher,
                      std::uint32_t tick, bool blast, float chosenSpread)
    {
        float axis[3][3] = {}, muzzle[3] = {};
        if (!Functions::CG_DObjGetWorldTagMatrix(&GetPose(slot), slot.obj, GetScriptString(flash), axis, muzzle))
            return;
        const ShotLogic::Axes barrel{glm::make_vec3(axis[0]), glm::make_vec3(axis[1]), glm::make_vec3(axis[2])};
        const auto forward = ShotLogic::GetPelletDirection(
            barrel, tick, 0, ShotLogic::GetSpread(1, launcher.weapDef->adsSpread, chosenSpread).degrees);
        const auto end = TraceShot(slot, muzzle, forward);
        const glm::vec3 start = glm::make_vec3(muzzle);
        const auto left = glm::normalize(glm::cross(barrel.up, forward));
        rockets.push_back({{tick, start, forward, static_cast<float>(launcher.weapDef->iProjectileSpeed),
                            glm::distance(start, end.point)},
                           actor.id,
                           &launcher,
                           end,
                           glm::mat3(forward, left, glm::cross(forward, left)),
                           0,
                           blast});
    }

    // the rotation of something going along a direction, level
    glm::mat3 FacingAlong(glm::vec3 forward)
    {
        const auto side = std::abs(forward.z) > 0.999f
                              ? glm::vec3(0.0f, 1.0f, 0.0f)
                              : glm::normalize(glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), forward));
        return glm::mat3(forward, side, glm::cross(forward, side));
    }

    // A killstreak's rockets beside its gun (the Gunship's, _helicopter_player.gsc fire_rocket):
    // from its tags by turns (by the shot's place in its Fire), at what
    // its gun aims at, traced from the gun's muzzle along its barrel; with its weapon's flash on the tag
    void LaunchKillstreakRockets(Built& slot, const Types::Actor& actor,
                                 const Components::KillstreakLogic::Rockets& kit, const Types::ActorEvent& event,
                                 std::uint32_t tick, bool blast)
    {
        const auto variant = WeaponRig::FindVariant(kit.weapon);
        if (!variant || !variant->weapDef || !variant->weapDef->projectileModel)
            return;
        const auto shot =
            event.interval > 0 && tick > event.tick ? (tick - event.tick + event.interval / 2) / event.interval : 0;
        const std::string tag(kit.tags[shot % kit.tags.size()]);
        float tagAxis[3][3] = {}, start[3] = {}, muzzleAxis[3][3] = {}, muzzle[3] = {};
        if (!Functions::CG_DObjGetWorldTagMatrix(&GetPose(slot), slot.obj, GetScriptString(tag.c_str()), tagAxis,
                                                 start) ||
            !Functions::CG_DObjGetWorldTagMatrix(&GetPose(slot), slot.obj,
                                                 GetScriptString(std::string(kit.muzzle).c_str()), muzzleAxis, muzzle))
            return;
        PlayBoltedEffect(slot, variant->weapDef->worldFlashEffect, tag.c_str());
        // (the game traces along tag_barrel's angles, which tag_flash shares: the gun's aim)
        const auto end = TraceShot(slot, muzzle, glm::normalize(glm::make_vec3(muzzleAxis[0])));
        const glm::vec3 from = glm::make_vec3(start);
        const auto way = end.point - from;
        const auto distance = glm::length(way);
        if (distance < 1.0f)
            return;
        const auto forward = way / distance;
        rockets.push_back({{tick, from, forward, static_cast<float>(variant->weapDef->iProjectileSpeed), distance},
                           actor.id,
                           variant,
                           end,
                           FacingAlong(forward),
                           0,
                           blast});
    }

    // The SAM Turret's missile (its weapon tow_turret_mp): from its muzzle
    // along its barrel, homing on the actor or player its Look event aims it at as the game's homes on what it locked
    // onto; at a point or the camera, straight on to what the barrel points at
    void LaunchMissile(Built& slot, const Types::Actor& actor, const char* flash, std::string_view weapon,
                       std::uint32_t tick, bool blast)
    {
        const auto variant = WeaponRig::FindVariant(weapon);
        if (!variant || !variant->weapDef || !variant->weapDef->projectileModel)
            return;
        float axis[3][3] = {}, muzzle[3] = {};
        if (!Functions::CG_DObjGetWorldTagMatrix(&GetPose(slot), slot.obj, GetScriptString(flash), axis, muzzle))
            return;
        const glm::vec3 forward = glm::normalize(glm::make_vec3(axis[0]));
        const auto end = TraceShot(slot, muzzle, forward);
        const glm::vec3 start = glm::make_vec3(muzzle);
        FlyingRocket missile{{tick, start, forward, static_cast<float>(variant->weapDef->iProjectileSpeed),
                              glm::distance(start, end.point)},
                             actor.id,
                             variant,
                             end,
                             FacingAlong(forward),
                             0,
                             blast};
        // (what its Look aims it at then: the last before the shot)
        const Types::ActorEvent* look = nullptr;
        for (const auto& event : actor.events)
            if (event.type == Types::ActorEventType::Look && event.tick <= tick && (!look || event.tick >= look->tick))
                look = &event;
        if (look && look->lookTarget == Types::ActorLookTarget::Actor && look->lookActor != actor.id)
            missile.homing = HomingTarget{
                {Types::BoneCameraTargetKind::Actor, static_cast<std::int32_t>(look->lookActor)}, "tag_origin"};
        else if (look && look->lookTarget == Types::ActorLookTarget::Player)
            missile.homing = HomingTarget{{Types::BoneCameraTargetKind::Player, look->lookPlayer}, "j_spine4"};
        rockets.push_back(std::move(missile));
    }

    // where a homing missile's target is now, none when it isn't there (gone, or not drawn)
    std::optional<glm::vec3> GetHomingPoint(const HomingTarget& homing)
    {
        if (const auto bone = Bones::GetTargetBone(homing.target, homing.bone))
            return bone->position;
        return std::nullopt;
    }

    // A shot of the actor's gun: a dual wield gun's left one when `left` (its world model names its muzzle and casing
    // bones tag_flash1 and tag_brass1, as the game fires a player's left gun from them),
    // else the right one. Its bullet lands with the hits the game's do unless its Fire event has them off; a shot of a clip's
    // fire notetrack (no event) always lands, its spread the gun's.
    void Fire(Built& slot, const Types::Actor& actor, bool tracers, bool left, std::uint32_t tick,
              const Types::ActorEvent* event = nullptr)
    {
        const bool hits = !event || event->hits;
        const auto spread = event ? event->spread : -1.0f;
        // a killstreak's guns, with their weapons' flashes, without sound
        if (slot.killstreak)
        {
            // (or its rockets, when its Fire has them: the Gunship's)
            if (event && event->rockets)
                if (const auto* rockets = Components::KillstreakLogic::GetRockets(*slot.killstreak))
                {
                    LaunchKillstreakRockets(slot, actor, *rockets, *event, tick, hits);
                    return;
                }
            for (const auto& gun : Components::KillstreakLogic::Get(*slot.killstreak).guns)
            {
                const std::string flash(gun.flash);
                PlayBoltedEffect(slot, FindEffect(std::string(gun.flashEffect)), flash.c_str());
                // (a gun without tracers is the SAM's launcher: its missile)
                if (!gun.tracers)
                    LaunchMissile(slot, actor, flash.c_str(), Components::KillstreakLogic::Get(*slot.killstreak).weapon,
                                  tick, hits);
                // (those with tracers fire bullets; the rest are its rockets, which are no bullet)
                const auto weapon = hits && gun.tracers
                                        ? WeaponRig::FindVariant(Components::KillstreakLogic::Get(*slot.killstreak).weapon)
                                        : nullptr;
                Shoot(slot, flash.c_str(), nullptr, tracers && gun.tracers,
                      weapon && WeaponRig::FiresBullets(*weapon) ? weapon : nullptr, tick, spread);
            }
            // the B-52's bombs are an effect, on the plane as the game has it (what else drops is DropLogic's)
            if (Components::KillstreakLogic::Get(*slot.killstreak).fireDrop ==
                Components::KillstreakLogic::FireDrop::BombRun)
            {
                const std::string effect(Components::KillstreakLogic::BOMB_RUN_EFFECT);
                if (PlayBoltedEffect(slot, FindEffect(effect), DEFAULT_EFFECT_TAG))
                    ++extraEffects[effect];
            }
            return;
        }
        if (!slot.variant)
            return;
        // (a left gun without its own muzzle fires from the right one)
        const bool fromLeft = left && slot.leftVariant && slot.leftVariant->weapDef &&
                              Functions::DObjGetBoneIndex(slot.obj, GetScriptString(LEFT_GUN_FLASH));
        const auto& shooter = *(fromLeft ? slot.leftVariant : slot.variant);
        const auto& weapon = *shooter.weapDef;
        const auto flash = fromLeft ? LEFT_GUN_FLASH : "tag_flash";
        PlayBoltedEffect(slot, weapon.worldFlashEffect, flash);
        PlayBoltedEffect(slot, weapon.worldShellEjectEffect, fromLeft ? LEFT_GUN_BRASS : "tag_brass");
        // (a projectile's shot is no bullet: no tracer)
        Shoot(slot, flash, WeaponRig::GetShotSound(shooter), tracers && !WeaponRig::FiresProjectiles(shooter),
              hits && WeaponRig::FiresBullets(shooter) ? &shooter : nullptr, tick, spread);
        if (WeaponRig::FiresRockets(shooter))
            LaunchRocket(slot, actor, flash, shooter, tick, hits, spread);
        ++muzzleCounts[flash];
    }

    bool IsFireLoopPlaying(std::uint32_t id)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        return slot != built.end() && slot->fireLoop;
    }

    std::vector<std::string> GetBuiltModelTags(std::uint32_t id)
    {
        std::vector<std::string> lines;
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        if (slot == built.end() || !slot->obj)
            return lines;
        for (const auto& model : GetModels(*slot))
            lines.push_back(std::format("{} on {}", model.model->name,
                                        model.boneName ? Functions::SL_ConvertToString(model.boneName) : "its bones"));
        return lines;
    }

    std::map<std::string, std::size_t> GetMuzzleCounts(bool reset)
    {
        auto counts = muzzleCounts;
        if (reset)
            muzzleCounts.clear();
        return counts;
    }

    constexpr auto DEFAULT_HIT_EFFECT = "impacts/fx_flesh_hit_body_nonfatal_mp";
    constexpr auto DEFAULT_HIT_TAG = "j_spine4";

    // An event's effect: on its bone, going where the bone goes; or, when the actor is hidden just after it (see
    // ActorLogic::EffectStays: the blast that ends a killstreak), where the bone is, in the world, so it plays out
    // after the actor is gone, as the game's playfx does. Its handle, 0 for one that stays or none.
    std::int32_t PlayEventEffect(Built& slot, const Types::Actor& actor, const Types::ActorEvent& event,
                                 const std::string& name, const char* tag)
    {
        const auto effect = FindEffect(name);
        if (!ActorLogic::EffectStays(actor, event))
            return PlayBoltedEffect(slot, effect, tag);
        float axis[3][3] = {}, origin[3] = {};
        if (!effect ||
            !Functions::CG_DObjGetWorldTagMatrix(&GetPose(slot), slot.obj, GetScriptString(tag), axis, origin))
            return 0;
        // (a killstreak's blast the game plays with playfx faces up, whichever way the killstreak pointed)
        const auto& facing = slot.killstreak && Components::KillstreakLogic::FacesUp(name) ? PLAYFX_AXIS : axis;
        if (PlayEffectAt(effect, glm::make_vec3(origin), facing))
            CountExtraEffect(name, facing);
        return 0;
    }

    // a sound of the game at a gun's muzzle, its id; nothing for none
    std::int32_t PlaySoundAtMuzzle(Built& slot, const char* sound)
    {
        typedef std::int32_t(__cdecl * CG_PlaySoundAlias_t)(std::int32_t localClientNum, std::int32_t entnum,
                                                            const float* origin, std::int32_t, std::int32_t,
                                                            float volume, std::uint32_t aliasHash);
        typedef std::uint32_t(__cdecl * SND_HashName_t)(const char* name);
        float axis[3][3] = {}, muzzle[3] = {};
        if (!sound || !*sound ||
            !Functions::CG_DObjGetWorldTagMatrix(&GetPose(slot), slot.obj, GetScriptString("tag_flash"), axis, muzzle))
            return 0;
        const auto& addresses = GetGameAddresses();
        return reinterpret_cast<CG_PlaySoundAlias_t>(addresses.CG_PlaySoundAlias())(
            0, ENTITYNUM_WORLD, muzzle, 0, 0, 1.0f, reinterpret_cast<SND_HashName_t>(addresses.SND_HashName())(sound));
    }

    // A burst of what fires by a loop (the Death Machine has no sound of a shot): its barrels spin up as it starts, and
    // the loop plays until its last shot
    void StartFireLoop(Built& slot, const Types::ActorEvent& event)
    {
        const auto def = slot.variant ? slot.variant->weapDef : nullptr;
        if (!def || !def->loopFireSound || !*def->loopFireSound)
            return;
        StopFireLoop(slot);
        PlaySoundAtMuzzle(slot, def->startSpinSound);
        typedef std::uint32_t(__cdecl * SND_HashName_t)(const char* name);
        float axis[3][3] = {}, muzzle[3] = {};
        if (!Functions::CG_DObjGetWorldTagMatrix(&GetPose(slot), slot.obj, GetScriptString("tag_flash"), axis, muzzle))
            return;
        slot.fireLoop = reinterpret_cast<SND_HashName_t>(GetGameAddresses().SND_HashName())(def->loopFireSound);
        slot.fireLoopAt = {muzzle[0], muzzle[1], muzzle[2]};
        slot.fireLoopEnd = event.End();
        Functions::SND_AddLoopEmitter(slot.fireLoop, muzzle);
    }

    // and it ends with the burst, as the timeline stops or jumps, or as the actor goes
    void UpdateFireLoop(Built& slot, std::uint32_t tick, bool playing)
    {
        if (!slot.fireLoop || (playing && tick < slot.fireLoopEnd))
            return;
        StopFireLoop(slot);
        if (playing && slot.variant && slot.variant->weapDef)
            PlaySoundAtMuzzle(slot, slot.variant->weapDef->stopSpinSound);
    }

    void PlayEvent(Built& slot, const Types::Actor& actor, const Types::ActorEvent& event, std::uint32_t shot)
    {
        const auto tag = event.type == Types::ActorEventType::Hit
                             ? (event.tag.empty() ? DEFAULT_HIT_TAG : event.tag.c_str())
                             : (event.tag.empty() ? DEFAULT_EFFECT_TAG : event.tag.c_str());
        std::int32_t handle = 0;
        switch (event.type)
        {
            case Types::ActorEventType::Fire:
                if (shot == 0)
                    StartFireLoop(slot, event);
                Fire(slot, actor, event.tracers, ActorLogic::IsLeftGunShot(ActorLogic::GetShotPlace(actor, event, shot)),
                     event.tick + shot * event.interval, &event);
                break;
            case Types::ActorEventType::Hit:
                handle = PlayEventEffect(slot, actor, event, event.effect.empty() ? DEFAULT_HIT_EFFECT : event.effect,
                                         tag);
                break;
            case Types::ActorEventType::Effect:
                handle = PlayEventEffect(slot, actor, event, event.effect, tag);
                break;
            default:
                break;
        }
        if (handle && Hooks::Effects::IsLooping(handle))
            slot.eventLoops.push_back(handle);
    }

    NotetrackCounts notetrackCounts;

    NotetrackCounts GetNotetrackCounts(bool reset)
    {
        const auto counts = notetrackCounts;
        if (reset)
            notetrackCounts = {};
        return counts;
    }

    // What a notetrack of a clip does as the actor plays past it: a shot, or a sound from the actor
    void PlayNotetrack(Built& slot, const Types::Actor& actor, const Types::ActorClip& clip, std::string_view name,
                       std::uint32_t shot, std::uint32_t tick)
    {
        typedef std::int32_t(__cdecl * CG_PlaySoundAlias_t)(std::int32_t localClientNum, std::int32_t entnum,
                                                            const float* origin, std::int32_t, std::int32_t,
                                                            float volume, std::uint32_t aliasHash);
        typedef std::uint32_t(__cdecl * SND_HashName_t)(const char* name);
        if (ActorLogic::IsShotNotetrack(name))
        {
            if (clip.notetrackShots && slot.variant)
            {
                Fire(slot, actor, true, ActorLogic::IsLeftGunShot(shot), tick + shot);
                ++notetrackCounts.shots;
            }
            return;
        }
        const auto sound = ActorLogic::GetNotetrackSound(name);
        if (sound.empty() || !clip.notetrackSounds)
            return;
        // from the chest
        float axis[3][3] = {}, origin[3] = {};
        if (!Functions::CG_DObjGetWorldTagMatrix(&GetPose(slot), slot.obj, GetScriptString("j_spine4"), axis, origin))
            std::memcpy(origin, GetPose(slot).origin, sizeof(origin));
        ++notetrackCounts.sounds;
        const auto& addresses = GetGameAddresses();
        const std::string alias(sound);
        reinterpret_cast<CG_PlaySoundAlias_t>(addresses.CG_PlaySoundAlias())(
            0, ENTITYNUM_WORLD, origin, 0, 0, 1.0f,
            reinterpret_cast<SND_HashName_t>(addresses.SND_HashName())(alias.c_str()));
    }

    // R_AddDObjToScene draws a DObj of one model and no tree as a rigid model unless it has this flag
    // The head's rig, from the rest pose of the models that have the bone below another; none without one
    std::optional<HeadRig> FindHeadRig(DObj& obj)
    {
        const auto headTag = GetScriptString("j_head");
        auto toQuat = [](const std::int16_t (&q)[4]) {
            return glm::normalize(glm::quat(q[3] / 32767.0f, q[0] / 32767.0f, q[1] / 32767.0f, q[2] / 32767.0f));
        };

        HeadRig rig;
        std::uint32_t first = 0;  // the index in the DObj of the model's first bone
        for (int m = 0; m < obj.numModels; ++m)
        {
            const auto& model = *obj.models[m];
            // the rotations of the model's bones in the model, as far as the head
            std::vector<glm::quat> rotations(model.numBones, glm::quat(1, 0, 0, 0));
            for (std::uint32_t b = model.numRootBones; b < model.numBones; ++b)
            {
                const auto i = b - model.numRootBones;
                const auto parent = static_cast<std::int32_t>(b) - model.parentList[i];
                const bool hasParent = parent >= 0 && parent < static_cast<std::int32_t>(b);
                const auto local = toQuat(model.quats[i]);
                rotations[b] = hasParent ? rotations[parent] * local : local;
                if (model.boneNames[b] != headTag || !hasParent || rig.copies.size() == MAX_HEAD_COPIES ||
                    first + b > 0xFF)
                    continue;

                if (rig.copies.empty())
                {
                    rig.offset = glm::make_vec3(model.trans[i]);
                    rig.forward = glm::inverse(rotations[b]) * glm::vec3(1, 0, 0);
                    rig.up = glm::inverse(rotations[b]) * glm::vec3(0, 0, 1);
                }
                rig.copies.push_back({static_cast<std::uint8_t>(first + b), model.boneNames[parent]});
            }
            first += model.numBones;
        }
        return rig.copies.empty() ? std::nullopt : std::optional(rig);
    }

    std::unique_ptr<SpineAnimation> MakeSpineAnimation(glm::quat rotation, glm::vec3 offset, const XAnimParts& like)
    {
        auto animation = std::make_unique<SpineAnimation>();
        animation->name = GetScriptString("torso_stabilizer");
        animation->rotation = AnimationPose::ToShorts(rotation);
        animation->offset = {offset.x, offset.y, offset.z};

        auto& parts = animation->parts;
        parts.name = "actor_spine";
        parts.dataByteCount = 1;
        parts.dataShortCount = 4;
        parts.dataIntCount = 3;
        parts.numframes = 1;
        parts.loop = true;
        parts.boneCount[PART_TYPE_FULL_QUAT_NO_SIZE] = 1;
        parts.boneCount[PART_TYPE_TRANS_NO_SIZE] = 1;
        parts.boneCount[PART_TYPE_ALL] = 1;
        parts.assetType = like.assetType;
        parts.framerate = 30.0f;
        parts.frequency = 30.0f;
        parts.names = &animation->name;
        parts.dataByte = &animation->bone;
        parts.dataShort = animation->rotation.data();
        parts.dataInt = animation->offset.data();
        return animation;
    }

    // those made for players' corpses, by the model that has the stabilizer
    std::map<const XModel*, std::unique_ptr<SpineAnimation>> linedSpines;

    const XModel* GetSpineRigOf(const XModel& body)
    {
        return GetSpineRig(body);
    }

    void SetAnimationEntry(XAnim_s* anims, std::uint32_t index, const XAnimParts* parts)
    {
        SetEntryAnimation(anims, index, parts);
    }

    const XAnimParts* GetLinedSpine(const DObj& obj, const XAnimParts& like)
    {
        for (int i = 0; i < obj.numModels; ++i)
        {
            if (const auto spine = AnimationPose::FindSpineRig(*obj.models[i]))
            {
                auto& lined = linedSpines[obj.models[i]];
                if (!lined)
                {
                    lined = MakeSpineAnimation(spine->lined.rotation, spine->lined.offset, like);
                    // the game goes through the notetracks of what a player's tree plays, every animation having one
                    lined->end = {GetScriptString("end"), 1.0f};
                    lined->parts.notify = &lined->end;
                    lined->parts.notifyCount = 1;
                }
                return &lined->parts;
            }
        }
        return nullptr;
    }

    // Where a Look event has the actor look, none when that isn't there now
    std::optional<glm::vec3> FindLookTarget(const Types::ActorEvent& event)
    {
        switch (event.lookTarget)
        {
            case Types::ActorLookTarget::Camera:
            {
                // the dolly where it is at the actors' tick, so the head holds with everything else while Freeze time
                // (or bullet time that holds actors) plays the dolly on; any other camera where it is now
                const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                if (camera->GetMode() == Components::Camera::Mode::Dolly)
                    if (const auto dolly = Hooks::Camera::GetDollyPosition(
                            Components::Playback::ActorToTimelineTick(Components::Playback::GetActorTick())))
                        return dolly;
                return camera->GetPosition();
            }
            case Types::ActorLookTarget::Point:
                return event.lookPoint;
            case Types::ActorLookTarget::Actor:
            {
                const auto other = std::ranges::find_if(
                    built, [&](const Built& slot) { return slot.active && slot.id == event.lookActor; });
                float axis[3][3]{}, origin[3]{};
                // its head, or a killstreak's body
                for (const auto tag : {"j_head", "tag_body", "tag_origin"})
                    if (other != built.end() &&
                        Functions::CG_DObjGetWorldTagMatrix(&GetPose(*other), other->obj, GetScriptString(tag), axis,
                                                            origin))
                        return glm::make_vec3(origin);
                return std::nullopt;
            }
            case Types::ActorLookTarget::Player:
            {
                const auto bone = Bones::GetBoneData(event.lookPlayer, "j_head");
                return bone.id >= 0 ? std::optional(bone.position) : std::nullopt;
            }
            default:
                return std::nullopt;
        }
    }

    // the most a head turns from where the actor faces, and up or down, in degrees
    inline constexpr float MAX_HEAD_YAW = 80.0f;
    inline constexpr float MAX_HEAD_PITCH = 55.0f;

    // Turns the actor's head to what its Look events have it look at by then, as far as a head turns. The bones the
    // copies of the head hang from are looked up first: that works out the skeleton as far as them, without the head,
    // so the head gets this frame's rotation when it is drawn.
    void TurnHead(Built& slot, const Types::Actor& actor, std::uint32_t tick, DObj* obj, cpose_t& pose)
    {
        const auto index = &slot - built.data();
        const auto look = Components::Actors::GetLook(actor, tick);
        if ((!look.from && !look.to) || !slot.head)
        {
            ClearHeadTurns(index);
            return;
        }
        const auto& head = *slot.head;

        // the axes of the bones the copies hang from
        std::vector<glm::mat3> parents;
        glm::vec3 position{};
        for (const auto& copy : head.copies)
        {
            float axis[3][3]{}, origin[3]{};
            if (!Functions::CG_DObjGetWorldTagMatrix(&pose, obj, copy.parentTag, axis, origin))
            {
                ClearHeadTurns(index);
                return;
            }
            parents.push_back(glm::make_mat3(&axis[0][0]));
            if (parents.size() == 1)
                position = glm::make_vec3(origin) + parents[0] * head.offset;
        }

        // the rotation of the head in the world that looks at what an event looks at, if anything
        auto lookAt = [&](const Types::ActorEvent* event) -> std::optional<glm::mat3> {
            const auto target = event ? FindLookTarget(*event) : std::nullopt;
            if (!target || glm::distance(*target, position) < 1.0f)
                return std::nullopt;

            // the direction, within how far a head turns from where the actor faces
            const auto direction = glm::normalize(*target - position);
            const auto yaw = pose.angles[1] + std::clamp(std::remainder(glm::degrees(std::atan2(direction.y,
                                                                                               direction.x)) -
                                                                            pose.angles[1],
                                                                        360.0f),
                                                         -MAX_HEAD_YAW, MAX_HEAD_YAW);
            const auto pitch = std::clamp(glm::degrees(std::asin(std::clamp(direction.z, -1.0f, 1.0f))),
                                          -MAX_HEAD_PITCH, MAX_HEAD_PITCH);
            const glm::vec3 forward(std::cos(glm::radians(pitch)) * std::cos(glm::radians(yaw)),
                                    std::cos(glm::radians(pitch)) * std::sin(glm::radians(yaw)),
                                    std::sin(glm::radians(pitch)));

            // the head's axes turned so its face looks that way, upright
            const auto left = glm::normalize(glm::cross(glm::vec3(0, 0, 1), forward));
            const auto world = glm::mat3(forward, left, glm::cross(forward, left));
            const auto headLeft = glm::normalize(glm::cross(head.up, head.forward));
            const auto own = glm::mat3(head.forward, headLeft, glm::cross(head.forward, headLeft));
            return world * glm::transpose(own);
        };
        const auto from = lookAt(look.from);
        const auto to = lookAt(look.to);

        // nothing to look at (looking ahead, or a target that isn't there) is the clip's own head
        for (std::size_t c = 0; c < head.copies.size(); ++c)
        {
            const auto& copy = head.copies[c];
            auto local = [&](const std::optional<glm::mat3>& world) -> std::optional<glm::quat> {
                if (!world)
                    return std::nullopt;
                return glm::normalize(glm::quat_cast(glm::transpose(parents[c]) * *world));
            };
            const auto turn = ActorLogic::GetHeadTurn(local(from), local(to), look.turned);
            SetHeadTurn(index, 0, c, slot.obj, copy.bone, turn);
            SetHeadTurn(index, 1, c, slot.unloadedObj, copy.bone, turn);
        }
    }

    // Holds a dual wield gun's left one where the dual wield animations do while the actor shows others (see
    // ActorLogic::GripsLeftGun), in both of its DObjs, as a corpse's is held (PlayerAnimation's LeftGunGrip); else
    // leaves tag_weapon_left to the animations
    void GripLeftGun(Built& slot, bool grip)
    {
        const auto index = static_cast<std::size_t>(&slot - built.data());
        const auto hold = [&](std::size_t objIndex, DObj* obj) {
            const auto bone = grip && slot.leftVariant && obj
                                  ? Functions::DObjGetBoneIndex(obj, GetScriptString("tag_weapon_left"))
                                  : std::nullopt;
            const auto rest = bone ? AnimationPose::GetRestPosition(*obj, *bone) : std::nullopt;
            if (!rest)
            {
                SetJointControl(index, objIndex, LEFT_GUN_JOINT, nullptr, std::nullopt, {});
                return;
            }
            SetJointControl(index, objIndex, LEFT_GUN_JOINT, obj, bone, ActorLogic::LEFT_GUN_GRIP_ANGLES,
                            ActorLogic::LEFT_GUN_GRIP_POSITION - *rest);
        };
        hold(0, slot.obj);
        hold(1, slot.unloadedObj);
    }

    void BlendHeads(DObj* obj, const int* partBits, const std::array<std::uint32_t, 5>& worked)
    {
        for (std::size_t group = 0; group < jointsControlled.size(); ++group)
        {
            if (jointsControlled[group] <= 0)
                continue;
            for (std::size_t joint = 0; joint < MAX_HEAD_COPIES; ++joint)
            {
                const auto& control = jointControls[group * MAX_JOINT_CONTROLS + joint];
                const auto bone = control.bone.load();
                if (bone < 0 || control.obj != obj)
                    continue;
                const auto weight = control.weight.load();
                const glm::quat rotation(control.rotation[3], control.rotation[0], control.rotation[1],
                                         control.rotation[2]);
                // only as the animations pose it, once a frame (not one a controller set)
                const auto bit = 0x80000000u >> (bone % 32);
                if (weight < 0.0f || !(partBits[bone / 32] & bit) || (worked[bone / 32] & bit))
                    continue;

                auto& quat = obj->skel[bone].quat;
                const auto turned = ActorLogic::ApplyHeadTurn(glm::quat(quat[3], quat[0], quat[1], quat[2]),
                                                              {rotation, weight});
                quat[0] = turned.x;
                quat[1] = turned.y;
                quat[2] = turned.z;
                quat[3] = turned.w;
            }
        }
    }

    inline constexpr std::uint32_t RENDER_FLAG_SKINNED = 4;

    XModel* FindModel(const std::string& name)
    {
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
        for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
            for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
                if (const auto& entry = entries[index]; entry.header && entry.type == ASSET_TYPE_XMODEL &&
                                                        *static_cast<const char* const*>(entry.header) == name)
                    return static_cast<XModel*>(const_cast<void*>(entry.header));
        return nullptr;
    }

    // Builds the models on the actor's bones again when they changed; a model that isn't loaded is left out
    void BuildProps(Built& slot, const Types::Actor& actor)
    {
        const bool same = std::ranges::equal(slot.props, actor.props, [](const Built::Prop& built, const auto& prop) {
            return built.model == prop.model;
        });
        if (same)
            return;
        ReleaseProps(slot);

        for (const auto& prop : actor.props)
        {
            Built::Prop built{.model = prop.model};
            const auto model = FindModel(prop.model);
            const auto handle = model ? FindFreeHandle() : std::nullopt;
            if (handle)
            {
                DObjModel_s models[] = {{model, 0, false}};
                built.obj = CreateDObj(models, nullptr, *handle);
                built.handle = built.obj ? *handle : 0;
                if (built.obj)
                    std::memset(GetClientEntityPose(*handle), 0, CENTITY_SIZE);
            }
            slot.props.push_back(std::move(built));
        }
    }

    // the rotation of angles of the game (pitch, yaw, roll): yaw about z, then pitch about y, then roll about x
    glm::mat4 AnglesToMatrix(const glm::vec3& angles)
    {
        return glm::rotate(glm::mat4(1.0f), glm::radians(angles.y), glm::vec3(0, 0, 1)) *
               glm::rotate(glm::mat4(1.0f), glm::radians(angles.x), glm::vec3(0, 1, 0)) *
               glm::rotate(glm::mat4(1.0f), glm::radians(angles.z), glm::vec3(1, 0, 0));
    }

    // Where the bone an attached actor is on is now: its origin and the way it faces; none while it isn't there
    std::optional<Types::ActorPlacement> FindAttachment(const Types::Actor& actor)
    {
        const auto& attachment = actor.attachment;
        float axis[3][3]{}, origin[3]{};
        if (attachment.kind == Types::ActorAttachment::Kind::Actor)
        {
            const auto other = std::ranges::find_if(
                built, [&](const Built& slot) { return slot.active && slot.id == attachment.actor; });
            if (other == built.end() || other->id == actor.id ||
                !Functions::CG_DObjGetWorldTagMatrix(&GetPose(*other), other->obj, GetScriptString(attachment.tag),
                                                    axis, origin))
                return std::nullopt;
        }
        else
        {
            const auto bone = Bones::GetBoneData(attachment.player, attachment.tag);
            if (bone.id < 0)
                return std::nullopt;
            for (int i = 0; i < 3; ++i)
            {
                origin[i] = bone.position[i];
                for (int j = 0; j < 3; ++j)
                    axis[i][j] = bone.rotation[i][j];
            }
        }
        // upright, turned the way the bone faces
        const auto yaw = glm::degrees(std::atan2(axis[0][1], axis[0][0]));
        const auto turn = glm::rotate(glm::mat4(1.0f), glm::radians(yaw), glm::vec3(0, 0, 1));
        return Types::ActorPlacement{glm::make_vec3(origin) + glm::vec3(turn * glm::vec4(attachment.offset, 0.0f)),
                                     std::remainder(yaw + attachment.yaw, 360.0f)};
    }

    // Hands a DObj to the renderer with a pose, with the camo of a variant (if any) swapped into the materials of its
    // models at the indices of a mask (bit i for model i) as the renderer draws them
    // a gun of a DObj that has its own camo (the stowed one), beside the guns it is drawn with
    struct OtherGun
    {
        const WeaponVariantDef* variant = nullptr;
        std::int32_t camo = 0;
        std::uint32_t model = 0;
    };

    void AddToScene(DObj* obj, cpose_t& pose, std::int32_t handle, glm::vec3 lightingOrigin,
                    const WeaponVariantDef* variant, std::int32_t camo, std::uint32_t weaponModels,
                    std::uint32_t renderFlags = 0, std::int32_t facePaint = 0, float scale = 1.0f,
                    const OtherGun& other = {})
    {
        typedef void(__cdecl * R_AddDObjToScene_t)(
            DObj * obj, cpose_t * pose, std::int32_t entnum, std::uint32_t renderFlags, const float* lightingOrigin,
            float, float, float, float, std::int32_t, std::int32_t, void*, void*, float, float);
        typedef std::int32_t(__cdecl * CG_GetWeaponMaterialOverride_t)(
            std::int32_t localClientNum, const float* origin, const WeaponVariantDef* variant,
            std::uint32_t renderOptions, std::uint32_t modelMask, void* shaderData, std::int32_t, std::int32_t);
        const auto& addresses = GetGameAddresses();
        const auto shaderData = reinterpret_cast<std::uint8_t*>(&pose) + POSE_SHADER_DATA;
        std::int32_t materialOverride = -1;
        if (variant && camo > 0)
            materialOverride =
                reinterpret_cast<CG_GetWeaponMaterialOverride_t>(addresses.CG_GetWeaponMaterialOverride())(
                    0, pose.origin, variant, static_cast<std::uint32_t>(camo) & RENDER_OPTIONS_CAMO_BITS,
                    weaponModels, shaderData, 0, -1);
        if (other.variant && other.camo > 0)
            materialOverride =
                reinterpret_cast<CG_GetWeaponMaterialOverride_t>(addresses.CG_GetWeaponMaterialOverride())(
                    0, pose.origin, other.variant, static_cast<std::uint32_t>(other.camo) & RENDER_OPTIONS_CAMO_BITS,
                    1u << other.model, shaderData, 0, materialOverride);
        // (for any model of the DObj: only heads have the image it swaps)
        if (facePaint > 0)
            materialOverride =
                reinterpret_cast<CG_GetWeaponMaterialOverride_t>(addresses.CG_GetWeaponMaterialOverride())(
                    0, pose.origin, nullptr, static_cast<std::uint32_t>(facePaint) << RENDER_OPTIONS_FACE_PAINT_SHIFT,
                    0xFFFFFFFFu, shaderData, 0, materialOverride);
        const float lighting[3] = {lightingOrigin.x, lightingOrigin.y, lightingOrigin.z};
        reinterpret_cast<R_AddDObjToScene_t>(addresses.R_AddDObjToScene())(obj, &pose, handle, renderFlags, lighting,
                                                                           0.0f,
                                                                           0.0f, 0.0f, 0.0f, -1, materialOverride,
                                                                           shaderData, nullptr, 0.0f, scale);
    }

    // the game's angles of a rotation: yaw about z, then pitch about y (down), then roll about x
    glm::vec3 ToGameAngles(const glm::mat3& rotation)
    {
        return {glm::degrees(std::asin(std::clamp(-rotation[0].z, -1.0f, 1.0f))),
                glm::degrees(std::atan2(rotation[0].y, rotation[0].x)),
                glm::degrees(std::atan2(rotation[1].z, rotation[2].z))};
    }

    // A bone of a killstreak's model (its first) at rest: its rotation and offset relative to its parent, and where it is
    // in the model; none for a root bone or one it lacks
    struct RestBone
    {
        std::uint8_t bone = 0;     // in the DObj
        std::uint16_t parent = 0;  // its name, a script string
        glm::mat3 rotation{1.0f};
        glm::vec3 offset{0.0f};
    };

    std::optional<RestBone> FindRestBone(DObj& obj, std::string_view name)
    {
        if (name.empty() || obj.numModels < 1)
            return std::nullopt;
        const auto tag = GetScriptString(std::string(name));
        const auto index = Functions::DObjGetBoneIndex(&obj, tag);
        const auto& model = *obj.models[0];
        for (std::uint32_t b = model.numRootBones; index && b < model.numBones; ++b)
        {
            if (model.boneNames[b] != tag)
                continue;
            const auto& q = model.quats[b - model.numRootBones];
            const auto& t = model.trans[b - model.numRootBones];
            return RestBone{*index, model.boneNames[b - model.parentList[b - model.numRootBones]],
                            glm::mat3_cast(glm::normalize(glm::quat(q[3], q[0], q[1], q[2]))),
                            glm::vec3(t[0], t[1], t[2])};
        }
        return std::nullopt;
    }

    std::optional<Built::GunBone> FindGunBone(DObj& obj, std::string_view name)
    {
        const auto rest = FindRestBone(obj, name);
        return rest ? std::optional(Built::GunBone{rest->bone, rest->parent, rest->rotation, rest->offset})
                    : std::nullopt;
    }

    // the bones the harness turns, by actor (see SetJointOverride)
    std::map<std::uint32_t, std::vector<std::pair<std::string, glm::vec3>>> jointOverrides;

    // A car lies on the ground under its wheels: the ground under its front axle lower than under its back pitches it
    // forward, under its right side lower than its left rolls it to the right, and it stands at their mean height
    void LayOnGround(const Components::KillstreakLogic::Wheels& wheels, cpose_t& pose)
    {
        const auto yaw = glm::radians(pose.angles[1]);
        const glm::vec2 forward(std::cos(yaw), std::sin(yaw));
        const glm::vec2 left(-forward.y, forward.x);
        const glm::vec3 origin(pose.origin[0], pose.origin[1], pose.origin[2]);
        const auto ground = [&](float ahead, float aside) {
            const auto at = glm::vec2(origin) + forward * ahead + left * aside;
            trace_t trace{};
            const float from[3] = {at.x, at.y, origin.z + 32.0f};
            const float to[3] = {at.x, at.y, origin.z - 96.0f};
            Functions::CG_LocationalTrace(&trace, from, to, ENTITYNUM_NONE, MASK_SHOT_NO_CHARACTERS);
            return trace.fraction < 1.0f ? from[2] + (to[2] - from[2]) * trace.fraction : origin.z;
        };
        const auto frontLeft = ground(wheels.front, wheels.track);
        const auto frontRight = ground(wheels.front, -wheels.track);
        const auto backLeft = ground(-wheels.back, wheels.track);
        const auto backRight = ground(-wheels.back, -wheels.track);
        pose.angles[0] = glm::degrees(std::atan2((backLeft + backRight - frontLeft - frontRight) / 2.0f,
                                                 wheels.front + wheels.back));
        pose.angles[2] = glm::degrees(std::atan2((frontLeft + backLeft - frontRight - backRight) / 2.0f,
                                                 2.0f * wheels.track));
        pose.origin[2] = (frontLeft + frontRight + backLeft + backRight) / 4.0f;
    }

    // A killstreak's body as the game's vehicle code moves it (see VehicleLogic::GetBody): a helicopter or plane tilted
    // (about its origin, which is a helicopter's rotor) and drifting where it hovers, a car lying on the ground with its
    // body leaning over its wheels, its front wheels steering and all four rolling. And the bones the harness turns.
    void PoseVehicle(Built& slot, const Types::Actor& actor, std::uint32_t tick, cpose_t& pose)
    {
        namespace Vehicles = Components::VehicleLogic;
        // the rotation each bone gets relative to its parent, in place of its own, and its move from where it rests
        struct Control
        {
            std::uint8_t bone = 0;
            glm::mat3 rotation{1.0f};
            glm::vec3 trans{0.0f};
        };
        std::vector<Control> controls;
        // a bone turned from its rest
        const auto turn = [&](const Built::GunBone& bone, const glm::mat3& by) {
            controls.push_back({bone.bone, bone.rest * by});
        };

        if (slot.killstreak)
        {
            const auto& info = Components::KillstreakLogic::Get(*slot.killstreak);
            if (const auto body = Components::Actors::GetVehicleBody(actor, tick))
            {
                if (info.vehicleClass == Types::VehicleClass::Car)
                    LayOnGround(info.wheels, pose);
                else
                {
                    for (int i = 0; i < 3; ++i)
                        pose.origin[i] += body->drift[i];
                    pose.angles[0] = body->pitch;
                    pose.angles[2] = body->roll;
                }
                const auto joints = Vehicles::GetJoints(info, *body);
                for (std::size_t i = 0; i < joints.size() && i < slot.vehicleBones.size(); ++i)
                    if (slot.vehicleBones[i])
                        controls.push_back({*slot.vehicleBones[i], glm::mat3(AnglesToMatrix(joints[i].angles))});
            }
            // the care package falling from the chopper
            pose.origin[2] += Vehicles::GetDropHeight(actor, tick);

            // its guns: aimed by its Look events (from where they face at rest on their mounts as drawn now, which
            // works out the skeleton as far as the mounts, without the guns), else scanning, within their arcs; else
            // left to its animations
            const auto look = Components::Actors::GetLook(actor, tick);
            const bool aiming = look.from || look.to;
            for (std::size_t i = 0; i < info.guns.size() && i < slot.gunBones.size(); ++i)
            {
                const auto& gun = info.guns[i];
                const auto& [yaw, pitch, hold] = slot.gunBones[i];
                const auto scan = Vehicles::GetIdleAim(gun, actor, tick);
                float axis[3][3]{}, origin[3]{};
                if (!yaw || (!aiming && !scan) ||
                    !Functions::CG_DObjGetWorldTagMatrix(&pose, slot.obj, yaw->parent, axis, origin))
                    continue;
                const auto mount = glm::make_mat3(&axis[0][0]);
                const auto facing = mount * yaw->rest;
                const auto from = glm::make_vec3(origin) + mount * yaw->offset;
                const auto idle = scan.value_or(Vehicles::Aim{});
                const auto aimAt = [&](const Types::ActorEvent* event) {
                    const auto target = event ? FindLookTarget(*event) : std::nullopt;
                    return target ? Vehicles::AimAt(gun, glm::transpose(facing) * (*target - from)) : idle;
                };
                const auto aim = aiming ? Vehicles::Blend(aimAt(look.from), aimAt(look.to), look.turned) : idle;
                turn(*yaw, glm::mat3(AnglesToMatrix(glm::vec3(0.0f, aim.yaw, 0.0f))));
                if (pitch)
                    turn(*pitch, glm::mat3(AnglesToMatrix(glm::vec3(aim.pitch, 0.0f, 0.0f))));
                if (hold && aiming)
                    turn(*hold, glm::mat3(1.0f));
            }
            // its barrels spinning as it fires
            if (slot.spinBone && !info.guns.empty())
                turn(*slot.spinBone, glm::mat3(AnglesToMatrix(glm::vec3(
                                         0.0f, 0.0f, Vehicles::GetSpin(actor.events, tick, info.guns.front().fireTime)))));
        }
        if (const auto overrides = jointOverrides.find(actor.id); overrides != jointOverrides.end())
            for (const auto& [bone, angles] : overrides->second)
                if (const auto found = Functions::DObjGetBoneIndex(slot.obj, GetScriptString(bone)))
                    controls.push_back({*found, glm::mat3(AnglesToMatrix(angles))});

        const auto index = static_cast<std::size_t>(&slot - built.data());
        auto joint = FIRST_VEHICLE_JOINT;
        for (const auto& control : controls)
            if (joint < LEFT_GUN_JOINT)
                SetJointControl(index, 0, joint++, slot.obj, control.bone, ToGameAngles(control.rotation),
                                control.trans);
        ClearJointControls(index, joint, LEFT_GUN_JOINT);
    }

    // Where an actor stands at a tick: as core places it, on the ground below a path that follows it, or on the bone it
    // is attached to (where that is now); none while that isn't there
    std::optional<Types::ActorPlacement> PlaceActor(const Types::Actor& actor, std::uint32_t tick)
    {
        auto placement = Components::Actors::EvaluatePlacement(actor, tick);
        if (actor.movement == Types::ActorMovement::Attached)
            return FindAttachment(actor);
        // a path is walked on the ground below it, wherever it goes up or down between its points
        if (actor.movement == Types::ActorMovement::Path && actor.followGround)
        {
            trace_t trace{};
            const float from[3] = {placement.position.x, placement.position.y, placement.position.z + 32.0f};
            const float to[3] = {placement.position.x, placement.position.y, placement.position.z - 96.0f};
            Functions::CG_LocationalTrace(&trace, from, to, ENTITYNUM_NONE, MASK_SHOT_NO_CHARACTERS);
            if (trace.fraction < 1.0f)
                placement.position.z = from[2] + (to[2] - from[2]) * trace.fraction;
        }
        return placement;
    }

    // The height of the ground below a point: what a trace down from a little above it hits, or 512 units down
    float FindGroundBelow(glm::vec3 point)
    {
        trace_t trace{};
        const float from[3] = {point.x, point.y, point.z + 4.0f};
        const float to[3] = {point.x, point.y, point.z - 512.0f};
        Functions::CG_LocationalTrace(&trace, from, to, ENTITYNUM_NONE, MASK_SHOT_NO_CHARACTERS);
        return from[2] + (to[2] - from[2]) * trace.fraction;
    }

    // The magazines of an actor's reloads at a tick, as their profiles say (see ReloadLogic::GetMagazines). A profile
    // is worked out a few milliseconds after it is first asked for, and meanwhile its clip has no reloads: the magazine
    // stays on the gun.
    struct ReloadsNow
    {
        std::vector<ActorLogic::Reload> reloads;
        ReloadLogic::Magazines magazines;  // points into the reloads
    };

    ReloadsNow GetReloadsNow(const Built& slot, const Types::Actor& actor, std::uint32_t tick)
    {
        ReloadsNow now;
        if (!slot.magazineTag || !slot.variant || !slot.rig)
            return now;
        now.reloads = Components::ReloadProfiles::GetReloads(actor, tick);
        now.magazines = ReloadLogic::GetMagazines(now.reloads, Components::ReloadProfiles::GetLookup(actor), *slot.rig,
                                                  tick, MAX_DROPPED_MAGAZINES);
        return now;
    }

    // Where the game draws a bone of an actor at another tick than the one drawn: on a DObj of its models with a tree
    // of its own, placed and animated as the actor is then. A DObj works out its skeleton once a frame, so one is made
    // for the look and freed again. None when it can't be made. An attached actor is taken to be where it is attached
    // now.
    std::optional<glm::mat4> ProbeBone(const Built& slot, const Types::Actor& actor, std::uint32_t tick,
                                       std::uint16_t tag)
    {
        typedef XAnimTree_s*(__cdecl * XAnimCreateTree_t)(XAnim_s * anims, Alloc_t alloc);
        typedef void(__cdecl * XAnimFreeTree_t)(XAnimTree_s * tree, Free_t free, std::int32_t context);
        typedef void(__cdecl * Com_SafeClientDObjFree_t)(std::int32_t handle, std::int32_t localClientNum);
        const auto& addresses = GetGameAddresses();
        const auto placement = PlaceActor(actor, tick);
        const auto handle = FindFreeHandle();
        if (!placement || !handle || !slot.obj || !slot.anims)
            return std::nullopt;
        auto models = GetModels(slot);
        const auto tree = reinterpret_cast<XAnimCreateTree_t>(addresses.XAnimCreateTree())(slot.anims, Alloc);
        std::optional<glm::mat4> matrix;
        if (const auto obj = CreateDObj(models, tree, *handle))
        {
            auto& pose = *GetClientEntityPose(*handle);
            std::memset(&pose, 0, CENTITY_SIZE);
            pose.origin[0] = placement->position.x;
            pose.origin[1] = placement->position.y;
            pose.origin[2] = placement->position.z;
            pose.angles[1] = placement->yaw;
            std::vector<float> weights;
            Animate(obj, tree, slot, weights, actor, tick);
            float axis[3][3] = {}, origin[3] = {};
            if (Functions::CG_DObjGetWorldTagMatrix(&pose, obj, tag, axis, origin))
                matrix = ToMatrix(axis, origin);
            reinterpret_cast<Com_SafeClientDObjFree_t>(addresses.Com_SafeClientDObjFree())(*handle, 0);
            std::memset(&pose, 0, CENTITY_SIZE);
        }
        if (tree)
            reinterpret_cast<XAnimFreeTree_t>(addresses.XAnimFreeTree())(tree, Free, 0);
        return matrix;
    }

    // Where a magazine that fell is placed in the world: for the actor as it was drawn as the magazine started to fall
    // (an attached actor as it is attached now), on the ground below, from where the game drew the gun (or the hand)
    // then, which the legs under an upper body clip move from where the animation alone has it (see
    // ReloadLogic::PlaceFall). What was placed last frame is used again while it is placed from the same.
    std::optional<ActorLogic::MagazineFall> PlaceFallen(const Built& slot, const Types::Actor& actor,
                                                        const ReloadLogic::Magazines::Fallen& fallen,
                                                        std::vector<PlacedFall>& placed)
    {
        const auto at = PlaceActor(actor, fallen.start);
        if (!at)
            return std::nullopt;
        PlacedFall key{
            .generation = sceneGeneration.value_or(0),
            .clip = fallen.reload->clip->id,
            .out = fallen.reload->out,
            .profile = fallen.profile,
            .placement = glm::translate(glm::mat4(1.0f), at->position) * AnglesToMatrix(glm::vec3(0.0f, at->yaw, 0.0f)),
            .speed = fallen.reload->clip->speed};
        const auto before = std::ranges::find_if(slot.falls, [&](const PlacedFall& fall) {
            return fall.generation == key.generation && fall.clip == key.clip && fall.out == key.out &&
                   fall.profile == key.profile && fall.placement == key.placement && fall.speed == key.speed;
        });
        if (before != slot.falls.end())
        {
            key.fall = before->fall;
        }
        else
        {
            // the magazine bone on the gun as it falls from it, or held as the hand that pulled it out lets go
            const auto& profile = *key.profile;
            std::optional<glm::mat4> drawn;
            const auto relative = !profile.fall           ? std::nullopt
                                  : profile.fall->fromGun ? std::optional(slot.rig->magazine)
                                  : profile.oldGrip       ? std::optional(profile.oldGrip->magazine)
                                                          : std::nullopt;
            const auto bone = profile.fall && profile.fall->fromGun
                                  ? std::string("tag_weapon_right")
                                  : std::string(ReloadLogic::GetHandBone(profile.hand));
            if (relative)
                if (const auto from = ProbeBone(slot, actor, fallen.start, GetScriptString(bone)))
                    drawn = *from * *relative;
            key.fall = ReloadLogic::PlaceFall(profile, *slot.rig, key.placement, key.speed, FindGroundBelow, drawn);
        }
        placed.push_back(std::move(key));
        return placed.back().fall;
    }

    // Draws the magazine in the actor's hand during a reload, and the last ones that fell
    void DrawMagazines(Built& slot, const Types::Actor& actor, std::uint32_t tick, const ReloadsNow& now,
                       glm::vec3 lightingOrigin)
    {
        slot.heldDrawn = ActorLogic::HeldMagazine::None;
        slot.droppedDrawn.clear();
        slot.reloadDrawn.clear();
        std::vector<PlacedFall> placed;
        if (now.reloads.empty())
        {
            slot.falls.clear();
            return;
        }
        const auto& last = now.reloads.back();
        const auto& magazines = now.magazines;
        const auto hand = ReloadLogic::GetHandBone(magazines.hand);
        slot.reloadDrawn =
            std::format("out {} swap {} in {} end {}, hand {}", last.out,
                        last.swap ? std::to_string(*last.swap) : std::string("-"), last.in, last.end, hand);

        // on the hand's bone as it is drawn now
        float axis[3][3] = {}, origin[3] = {};
        if (magazines.state.held != ActorLogic::HeldMagazine::None && BuildMagazine(slot, slot.held) &&
            Functions::CG_DObjGetWorldTagMatrix(&GetPose(slot), slot.obj, GetScriptString(std::string(hand)), axis,
                                                origin))
        {
            const auto transform = ToMatrix(axis, origin) * magazines.held;
            auto& pose = *GetClientEntityPose(slot.held.handle);
            SetPose(pose, transform);
            // (skinned: a DObj of one model and no tree is otherwise drawn as a rigid model, with all the surfaces of
            // its level of detail)
            AddToScene(slot.held.obj, pose, slot.held.handle, lightingOrigin, slot.variant, actor.camo, 1u,
                       RENDER_FLAG_SKINNED);
            slot.heldDrawn = magazines.state.held;
            slot.heldTransform = transform;
        }

        // those that fell, the latest first
        std::size_t drawn = 0;
        for (const auto& fallen : magazines.fallen)
        {
            if (drawn == slot.dropped.size())
                break;
            auto& magazine = slot.dropped[drawn];
            if (!BuildMagazine(slot, magazine))
                break;
            const auto fall = PlaceFallen(slot, actor, fallen, placed);
            if (!fall)
                continue;
            const auto transform =
                ActorLogic::GetMagazineTransform(*fall, static_cast<float>(tick - fallen.start) / 1000.0f);
            auto& pose = *GetClientEntityPose(magazine.handle);
            SetPose(pose, transform);
            const auto middle = glm::vec3(transform * glm::vec4(fall->middle, 1.0f));
            AddToScene(magazine.obj, pose, magazine.handle, middle + glm::vec3(0.0f, 0.0f, 4.0f), slot.variant,
                       actor.camo, 1u, RENDER_FLAG_SKINNED);
            slot.droppedDrawn.push_back(transform);
            ++drawn;
        }
        slot.falls = std::move(placed);
    }

    // the timeline's tick when the events were last played: they go off as it plays past them, not when it jumps
    std::optional<std::uint32_t> eventTick;
    inline constexpr std::uint32_t MAX_EVENT_STEP = 1000;

    // Effects in the scene on their own. Each play of an effect that is on at the timeline's
    // tick is spawned where the effect is, facing its way; one the playhead lands inside begins as far back as it has
    // played, up to ActorLogic::MAX_EFFECT_CATCH_UP. A looping play is kept and ended at its end (its
    // particles let finish, or cut at once at the bar's end), one that plays once let go to end by itself, and all are
    // remembered for StopEffects (a rewind). What was spawned for an effect ends at once when it changes, is hidden or
    // deleted, and when the playhead jumps; what is on is spawned again then.
    struct SceneEffect
    {
        Types::Actor actor;  // as its plays were spawned
        std::map<std::uint32_t, std::pair<std::int32_t, ActorLogic::EffectPlay>> kept;  // by begin: handle and play
        std::set<std::uint32_t> begun;  // the begins of the plays spawned since the last jump
    };
    std::map<std::uint32_t, SceneEffect> sceneEffects;
    std::optional<std::uint32_t> sceneEffectsTick;
    std::map<std::string, std::size_t> sceneEffectSpawns;  // by effect, since the harness last read them

    void EndSceneEffect(SceneEffect& effect, bool cut)
    {
        for (const auto& [begin, kept] : effect.kept)
            Functions::FX_KillEffect(0, kept.first, cut);
        effect.kept.clear();
        effect.begun.clear();
    }

    // spawns an effect in the world and keeps it (unlike PlayEffectAt), to end it with FX_KillEffect; its handle, 0 for
    // none
    std::int32_t SpawnKeptEffect(void* effect, glm::vec3 point, const float (&axis)[3][3], std::int32_t msecBegin)
    {
        const float origin[3] = {point.x, point.y, point.z};
        const auto handle = Functions::FX_SpawnWorldEffect(effect, origin, axis, msecBegin, true);
        RememberEffect(handle);
        return handle;
    }

    void PlaySceneEffects(std::uint32_t tick)
    {
        const auto catalog = Components::Actors::GetCatalog();
        const auto cg = GetClientGlobals();
        if (!catalog || !cg)
            return;
        const bool jumped = !sceneEffectsTick || tick < *sceneEffectsTick || tick - *sceneEffectsTick > MAX_EVENT_STEP;
        sceneEffectsTick = tick;

        // gone, hidden or changed: what it spawned ends at once; after a jump all of it does
        for (auto it = sceneEffects.begin(); it != sceneEffects.end();)
        {
            const auto actor = std::ranges::find(scene, it->first, &Types::Actor::id);
            const auto& was = it->second.actor;
            if (actor == scene.end() || actor->hidden || actor->effect != was.effect ||
                actor->position != was.position || actor->yaw != was.yaw)
            {
                EndSceneEffect(it->second, true);
                it = sceneEffects.erase(it);
                continue;
            }
            if (jumped)
                EndSceneEffect(it->second, true);
            ++it;
        }

        for (const auto& actor : scene)
        {
            if (!actor.effect || actor.hidden)
                continue;
            const auto def = FindEffect(actor.effect->name);
            const auto info = catalog->effectInfo.find(actor.effect->name);
            if (!def || info == catalog->effectInfo.end())
                continue;
            auto& state = sceneEffects[actor.id];
            state.actor = actor;

            // its looping plays that are over end, their particles let finish (or cut at once at the bar's end)
            for (auto it = state.kept.begin(); it != state.kept.end();)
            {
                const auto& [handle, play] = it->second;
                if (tick < play.end && tick >= play.begin)
                {
                    ++it;
                    continue;
                }
                Functions::FX_KillEffect(0, handle, play.cut);
                it = state.kept.erase(it);
            }

            // facing its way: its x axis where it fires, as the game's angles turn it
            const auto turned = AnglesToMatrix(glm::vec3(actor.effect->pitch, actor.yaw, actor.effect->roll));
            float axis[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    axis[i][j] = turned[i][j];
            for (const auto& play : ActorLogic::GetEffectPlays(actor, info->second, tick))
            {
                if (!state.begun.insert(play.begin).second)
                    continue;
                const auto msecBegin =
                    cg->time - static_cast<std::int32_t>(std::min(tick - play.begin, ActorLogic::MAX_EFFECT_CATCH_UP));
                if (play.kept)
                {
                    if (const auto handle = SpawnKeptEffect(def, actor.position, axis, msecBegin))
                        state.kept[play.begin] = {handle, play};
                }
                else
                    PlayEffectAt(def, actor.position, axis, msecBegin);
                ++sceneEffectSpawns[actor.effect->name];
            }
        }
    }

    void ForgetDrops();

    void ForgetSceneEffects()
    {
        sceneEffects.clear();
        sceneEffectsTick.reset();
        ForgetDrops();
    }

    std::optional<SceneEffectState> GetSceneEffectState(std::uint32_t id)
    {
        const auto it = sceneEffects.find(id);
        if (it == sceneEffects.end())
            return std::nullopt;
        SceneEffectState state{.kept = it->second.kept.size(), .begun = it->second.begun.size()};
        for (const auto& [begin, kept] : it->second.kept)
            state.alive += IsEffectPlaying(kept.first) ? 1 : 0;
        return state;
    }

    std::map<std::string, std::size_t> GetSceneEffectSpawns(bool reset)
    {
        auto spawns = sceneEffectSpawns;
        if (reset)
            sceneEffectSpawns.clear();
        return spawns;
    }

    // Models in the world on their own: what a killstreak's Fire drops and the scene's props, each a DObj of its own
    // (as Built::Prop) on one of the handles left free. One not drawn in a frame goes, so they last as long as they
    // show, and the game's limits count only what is on screen.
    struct SceneModel
    {
        std::string model;
        std::int32_t handle = 0;
        DObj* obj = nullptr;
        bool drawn = false;  // this frame
        glm::vec3 position{0.0f};
    };
    using SceneModelKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;  // actor, event (0 for a prop), shot
    std::map<SceneModelKey, SceneModel> sceneModels;

    void ReleaseSceneModel(const SceneModel& model)
    {
        typedef void(__cdecl * Com_SafeClientDObjFree_t)(std::int32_t handle, std::int32_t localClientNum);
        if (model.obj && Functions::Com_GetClientDObj(model.handle, 0) == model.obj)
            reinterpret_cast<Com_SafeClientDObjFree_t>(GetGameAddresses().Com_SafeClientDObjFree())(model.handle, 0);
    }

    // (as rigid models, without RENDER_FLAG_SKINNED, the only way the game scales one)
    void DrawSceneModel(const SceneModelKey& key, const std::string& name, glm::vec3 point, const glm::mat3& rotation,
                        float scale, XModel* known = nullptr)
    {
        auto found = sceneModels.find(key);
        if (found != sceneModels.end() && found->second.model != name)
        {
            ReleaseSceneModel(found->second);
            sceneModels.erase(found);
            found = sceneModels.end();
        }
        if (found == sceneModels.end())
        {
            // (a model of another map stays out: the catalog says, without going through the game's assets each frame)
            const auto catalog = Components::Actors::GetCatalog();
            const auto model = known                                                         ? known
                               : catalog && std::ranges::binary_search(catalog->props, name) ? FindModel(name)
                                                                                             : nullptr;
            const auto handle = model ? FindFreeHandle() : std::nullopt;
            if (!handle)
                return;
            DObjModel_s models[] = {{model, 0, false}};
            const auto obj = CreateDObj(models, nullptr, *handle);
            if (!obj)
                return;
            std::memset(GetClientEntityPose(*handle), 0, CENTITY_SIZE);
            found = sceneModels.emplace(key, SceneModel{name, *handle, obj}).first;
        }
        auto& drawn = found->second;
        auto& pose = *GetClientEntityPose(drawn.handle);
        auto matrix = glm::mat4(rotation);
        matrix[3] = glm::vec4(point, 1.0f);
        SetPose(pose, matrix);
        AddToScene(drawn.obj, pose, drawn.handle, point + glm::vec3(0.0f, 0.0f, 32.0f), nullptr, 0, 0, 0, 0, scale);
        drawn.drawn = true;
        drawn.position = point;
    }

    void EndSceneModels()
    {
        std::erase_if(sceneModels, [](auto& entry) {
            auto& model = entry.second;
            const bool gone = !model.drawn;
            model.drawn = false;
            if (gone)
                ReleaseSceneModel(model);
            return gone;
        });
    }

    // What a killstreak's Fire events drop, kept only so the ground isn't looked for
    // every frame: core works it all out again as the actor changes, the plays begun and their handles go with a jump.
    struct FiredDrops
    {
        std::uint32_t generation = 0;  // of core's actors
        std::vector<Components::DropLogic::Drop> drops;
        std::set<std::pair<std::uint32_t, std::size_t>> begun;  // by shot and play
        std::vector<std::int32_t> handles;
        bool seen = false;
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, FiredDrops> firedDrops;  // by actor and event
    std::optional<std::uint32_t> dropsTick;

    void EndDropEffects(FiredDrops& fired)
    {
        for (const auto handle : fired.handles)
            Functions::FX_KillEffect(0, handle, true);
        fired.handles.clear();
        fired.begun.clear();
    }

    void ForgetDrops()
    {
        for (auto& [key, fired] : firedDrops)
        {
            fired.handles.clear();
            fired.begun.clear();
        }
        dropsTick.reset();
    }

    std::optional<Components::DropLogic::Hit> TraceDrop(glm::vec3 from, glm::vec3 to)
    {
        trace_t result{};
        const float start[3] = {from.x, from.y, from.z};
        const float end[3] = {to.x, to.y, to.z};
        Functions::CG_LocationalTrace(&result, start, end, ENTITYNUM_NONE, MASK_SHOT_NO_CHARACTERS);
        if (result.fraction >= 1.0f)
            return std::nullopt;
        return Components::DropLogic::Hit{from + (to - from) * result.fraction, glm::make_vec3(result.normal)};
    }

    bool HaveSameReleases(const std::vector<Components::DropLogic::Drop>& a,
                          const std::vector<Components::DropLogic::Drop>& b)
    {
        return std::ranges::equal(a, b, [](const auto& one, const auto& other) {
            return one.shot == other.shot && one.model == other.model && one.release.tick == other.release.tick &&
                   one.release.point == other.release.point && one.release.velocity == other.release.velocity;
        });
    }

    // What falls and what it plays, at a tick of the timeline. A model is where the tick puts it; an effect starts as
    // the playhead passes it, or as far back as it has played when the playhead lands inside it, as the scene's do.
    void PlayDrops(std::uint32_t tick)
    {
        namespace Killstreaks = Components::KillstreakLogic;
        namespace Drops = Components::DropLogic;
        const auto cg = GetClientGlobals();
        if (!cg)
            return;
        const bool jumped = !dropsTick || tick < *dropsTick || tick - *dropsTick > MAX_EVENT_STEP;
        dropsTick = tick;
        const auto generation = Components::Actors::GetGeneration();

        for (auto& [key, fired] : firedDrops)
            fired.seen = false;
        for (const auto& actor : scene)
        {
            // (the Mortar Team's shells too)
            const auto killstreak = Killstreaks::FindByModels(actor.models);
            if ((!killstreak && !actor.mortar) || actor.hidden)
                continue;
            const auto* info = killstreak ? &Killstreaks::Get(*killstreak) : nullptr;
            if (info && info->fireDrop != Killstreaks::FireDrop::NapalmBombs &&
                info->fireDrop != Killstreaks::FireDrop::Crate)
                continue;
            for (const auto& event : actor.events)
            {
                if (event.type != Types::ActorEventType::Fire)
                    continue;
                auto& fired = firedDrops[{actor.id, event.id}];
                fired.seen = true;
                if (fired.drops.empty() || fired.generation != generation)
                {
                    const auto place = [&](std::uint32_t at) {
                        auto placement = Components::Actors::EvaluatePlacement(actor, at);
                        if (const auto body = Components::Actors::GetVehicleBody(actor, at))
                            placement.position += body->drift;
                        return placement;
                    };
                    auto drops = info ? Drops::MakeDrops(*info, actor, event, place, TraceDrop)
                                      : Drops::MakeMortarDrops(actor, event, TraceDrop);
                    // (only what moved starts again)
                    if (!HaveSameReleases(fired.drops, drops))
                        EndDropEffects(fired);
                    fired.drops = std::move(drops);
                    fired.generation = generation;
                }
                if (jumped)
                    EndDropEffects(fired);
                for (const auto& drop : fired.drops)
                {
                    if (const auto shown = Drops::GetShown(drop, tick))
                        DrawSceneModel({actor.id, event.id, drop.shot}, std::string(drop.model), shown->point,
                                       shown->rotation, 1.0f);
                    for (std::size_t i = 0; i < drop.plays.size(); ++i)
                    {
                        const auto& play = drop.plays[i];
                        if (tick < play.tick || tick >= play.tick + play.life ||
                            !fired.begun.insert({drop.shot, i}).second)
                            continue;
                        const std::string effect(play.effect);
                        const auto turned = AnglesToMatrix(play.angles);
                        float axis[3][3];
                        for (int a = 0; a < 3; ++a)
                            for (int b = 0; b < 3; ++b)
                                axis[a][b] = turned[a][b];
                        const auto back = std::min(tick - play.tick, ActorLogic::MAX_EFFECT_CATCH_UP);
                        const auto msecBegin = cg->time - static_cast<std::int32_t>(back);
                        if (const auto handle = PlayEffectAt(FindEffect(effect), play.point, axis, msecBegin))
                        {
                            fired.handles.push_back(handle);
                            CountExtraEffect(effect, axis);
                        }
                    }
                }
            }
        }
        // (gone with its actor or event, or an actor hidden: what it played goes too)
        std::erase_if(firedDrops, [](auto& entry) {
            if (!entry.second.seen)
                EndDropEffects(entry.second);
            return !entry.second.seen;
        });
    }

    // the blast of a rocket where it landed: its weapon file's, or for one with none its row of the impact table by the
    // surface, as the game's rockets; none in the air at the end of its range
    void PlayBlast(const FlyingRocket& rocket)
    {
        const auto cg = GetClientGlobals();
        const auto& def = *rocket.variant->weapDef;
        if (!cg || !rocket.end.hit || !rocket.blast)
            return;
        void* effect = def.projExplosionEffect;
        std::uint32_t tableSound = 0;
        if (!effect)
            Functions::CG_GetImpactEffect(GetImpactWeapon(*rocket.variant), rocket.end.surface, 0, effect, tableSound);
        const float point[3] = {rocket.end.point.x, rocket.end.point.y, rocket.end.point.z};
        typedef std::int32_t(__cdecl * CG_PlaySoundAlias_t)(std::int32_t localClientNum, std::int32_t entnum,
                                                            const float* origin, std::int32_t, std::int32_t,
                                                            float volume, std::uint32_t aliasHash);
        typedef std::uint32_t(__cdecl * SND_HashName_t)(const char* name);
        const auto& addresses = GetGameAddresses();
        const auto sound = def.projExplosionSound && *def.projExplosionSound
                               ? reinterpret_cast<SND_HashName_t>(addresses.SND_HashName())(def.projExplosionSound)
                               : tableSound;
        if (sound)
            reinterpret_cast<CG_PlaySoundAlias_t>(addresses.CG_PlaySoundAlias())(0, ENTITYNUM_WORLD, point, 0, 0, 1.0f, sound);
        ++hitCounts[std::format("rocket surface {} {}", rocket.end.surface,
                                effect ? *static_cast<const char* const*>(effect) : "no effect")];
        lastHit = rocket.end.point;
        if (!effect)
            return;
        const auto axes = ShotLogic::GetImpactAxes(rocket.end.normal, ShotLogic::GetImpactTurn(rocket.flight.tick));
        const float axis[3][3] = {{axes.forward.x, axes.forward.y, axes.forward.z},
                                  {axes.left.x, axes.left.y, axes.left.z},
                                  {axes.up.x, axes.up.y, axes.up.z}};
        RememberEffect(Functions::FX_SpawnWorldEffect(effect, point, axis, cg->time, false,
                                                      static_cast<std::uint32_t>(rocket.end.entity)));
    }

    // the game's surface type of metal, which a missile bursts on as it reaches what it homed on
    constexpr std::uint32_t METAL_SURFACE = 13;

    // a rocket's model where it is, its trail on it
    void DrawRocket(FlyingRocket& rocket, glm::vec3 point)
    {
        typedef std::int32_t(__cdecl * CG_PlayBoltedEffect_t)(std::int32_t localClientNum, void* effect,
                                                              std::int32_t handle, std::uint32_t tagName);
        const auto model = rocket.variant->weapDef->projectileModel;
        const SceneModelKey key{rocket.actor, UINT32_MAX, rocket.flight.tick};
        DrawSceneModel(key, model->name, point, rocket.rotation, 1.0f, model);
        // (on the rocket as it flies: the game's call takes a bone it hasn't for the rocket itself)
        if (const auto drawn = sceneModels.find(key);
            drawn != sceneModels.end() && !IsEffectPlaying(rocket.trail) && rocket.variant->weapDef->projTrailEffect)
        {
            reinterpret_cast<centity_t*>(GetClientEntityPose(drawn->second.handle))->flags |= CENTITY_FLAG_HAS_MODEL;
            rocket.trail = reinterpret_cast<CG_PlayBoltedEffect_t>(GetGameAddresses().CG_PlayBoltedEffect())(
                0, rocket.variant->weapDef->projTrailEffect, drawn->second.handle, GetScriptString("tag_fx"));
            RememberEffect(rocket.trail);
        }
    }

    // The launchers' rockets of the shots gone: each drawn where its flight has it, its trail on it, its blast where it
    // lands; a missile homing on what it aims at. A jump or a rewind leaves those in the air behind, as the Fire events
    // launch them again.
    void PlayRockets(std::uint32_t tick)
    {
        const bool jumped = !rocketsTick || tick < *rocketsTick || tick - *rocketsTick > MAX_EVENT_STEP;
        rocketsTick = tick;
        if (jumped)
        {
            for (const auto& rocket : rockets)
                if (IsEffectPlaying(rocket.trail))
                    Functions::FX_KillEffect(0, rocket.trail);
            rockets.clear();
        }
        for (auto rocket = rockets.begin(); rocket != rockets.end();)
        {
            // a missile homing on what it aims at goes along the line to where that is now, and bursts there
            if (rocket->homing)
            {
                if (const auto target = GetHomingPoint(*rocket->homing))
                {
                    if (ShotLogic::HasHomed(rocket->flight, *target, tick))
                    {
                        if (IsEffectPlaying(rocket->trail))
                            Functions::FX_KillEffect(0, rocket->trail, false);
                        const auto way = *target - rocket->flight.start;
                        rocket->end = ShotEnd{
                            .point = *target,
                            .normal = glm::length(way) > 0.001f ? -glm::normalize(way) : glm::vec3(0.0f, 0.0f, 1.0f),
                            .hit = true,
                            .surface = METAL_SURFACE};
                        PlayBlast(*rocket);
                        rocket = rockets.erase(rocket);
                        continue;
                    }
                    if (const auto point = ShotLogic::GetHomingPoint(rocket->flight, *target, tick))
                    {
                        const auto way = *target - *point;
                        if (glm::length(way) > 1.0f)
                            rocket->rotation = FacingAlong(glm::normalize(way));
                        DrawRocket(*rocket, *point);
                    }
                    ++rocket;
                    continue;
                }
                // (its target gone, it flies on straight)
                rocket->homing.reset();
            }
            if (ShotLogic::HasLanded(rocket->flight, tick))
            {
                if (IsEffectPlaying(rocket->trail))
                    Functions::FX_KillEffect(0, rocket->trail, false);
                PlayBlast(*rocket);
                rocket = rockets.erase(rocket);
                continue;
            }
            if (const auto point = ShotLogic::GetRocketPoint(rocket->flight, tick))
                DrawRocket(*rocket, *point);
            ++rocket;
        }
    }

    std::vector<DropState> GetDropStates(std::uint32_t id)
    {
        std::vector<DropState> states;
        for (const auto& [key, fired] : firedDrops)
        {
            if (key.first != id)
                continue;
            for (const auto& drop : fired.drops)
            {
                DropState state{.event = key.second,
                                .shot = drop.shot,
                                .model = std::string(drop.model),
                                .releaseTick = drop.release.tick,
                                .release = drop.release.point,
                                .plays = drop.plays.size(),
                                .begun = static_cast<std::size_t>(std::ranges::count_if(
                                    fired.begun, [&](const auto& begun) { return begun.first == drop.shot; }))};
                if (drop.landing)
                {
                    state.landing = drop.landing->point;
                    state.seconds = drop.landing->seconds;
                }
                if (const auto found = sceneModels.find({id, key.second, drop.shot}); found != sceneModels.end())
                    state.drawnAt = found->second.position;
                states.push_back(std::move(state));
            }
        }
        return states;
    }

    // A prop in the scene's: the model that shows during its bar
    // the browser's preview where it stood last, traced again only as the camera's ray changes
    struct PropPreviewPlace
    {
        glm::vec3 origin{0.0f};
        glm::vec3 direction{0.0f};
        std::optional<glm::vec3> point;
    };
    std::optional<PropPreviewPlace> propPreviewPlace;
    // (a key of its own: no actor has id 0)
    constexpr SceneModelKey PROP_PREVIEW_KEY{0, 0, 0};

    // Props in the scene during their bars, where their keys have them at the timeline's tick, and the model
    // a row of the browser is hovered for, standing on what the camera looks at, facing it
    void DrawPlacedProps(std::uint32_t tick)
    {
        const auto keysTick = Components::Playback::GetTimelineTick();
        for (const auto& actor : scene)
        {
            if (!actor.placedProp || actor.hidden || tick < actor.placedProp->start || tick >= actor.placedProp->end)
                continue;
            const auto keyed = Components::Props::WithKeyedPlacement(actor, keysTick);
            const auto& prop = *keyed.placedProp;
            DrawSceneModel({actor.id, 0, 0}, prop.model, keyed.position,
                           glm::mat3(AnglesToMatrix(glm::vec3(prop.pitch, keyed.yaw, prop.roll))), prop.scale);
        }

        const auto preview = Components::Props::GetPreview();
        if (!preview)
        {
            propPreviewPlace.reset();
            return;
        }
        if (!propPreviewPlace || propPreviewPlace->origin != preview->origin ||
            propPreviewPlace->direction != preview->direction)
        {
            const auto surface = FindSurface(preview->origin, preview->direction);
            propPreviewPlace = PropPreviewPlace{preview->origin, preview->direction,
                                                surface ? std::optional(surface->first) : std::nullopt};
        }
        if (propPreviewPlace->point)
            DrawSceneModel(PROP_PREVIEW_KEY, preview->model, *propPreviewPlace->point,
                           glm::mat3(AnglesToMatrix(glm::vec3(0.0f, preview->yaw, 0.0f))), 1.0f);
    }

    std::optional<glm::vec3> GetPropPreviewDrawn()
    {
        const auto found = sceneModels.find(PROP_PREVIEW_KEY);
        return found != sceneModels.end() ? std::optional(found->second.position) : std::nullopt;
    }

    std::optional<glm::vec3> GetPlacedPropDrawn(std::uint32_t id)
    {
        const auto found = sceneModels.find({id, 0, 0});
        return found != sceneModels.end() ? std::optional(found->second.position) : std::nullopt;
    }

    std::size_t CountSceneModels()
    {
        return sceneModels.size();
    }

    // the mod unloads (see Unload): nothing is built anymore
    bool unloaded = false;

    // whether the actors can be built, posed and drawn now
    bool CanAddActors(std::int32_t localClientNum)
    {
        return localClientNum == 0 && GetClientGlobals() &&
               Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo && !ContentPacks::IsLoading() &&
               !unloaded;
    }

    // Brings what is built up to date with core's actors, the catalog of what they can be made of and their placement
    // requests, releasing what was built for actors that are gone or made of something else now. Again in a frame, it
    // finds nothing new.
    void UpdateScene()
    {
        // (what the actors' legs and faces may play depends on both)
        bool legsChanged = false;
        const auto packs = ContentPacks::GetPackState();
        const auto storeGeneration = AnimationStore::GetGeneration();
        if (catalogGeneration != packs.generation || catalogStoreGeneration != storeGeneration)
        {
            catalogGeneration = packs.generation;
            catalogStoreGeneration = storeGeneration;
            BuildCatalog(packs);
            failed.clear();
            legsChanged = true;
        }

        HandlePlacementRequests();

        if (const auto generation = Components::Actors::GetGeneration(); sceneGeneration != generation)
        {
            legsChanged = true;
            sceneGeneration = generation;
            scene = Components::Actors::GetActors();
            failed.clear();
            weaponsCatalog.reset();
            std::erase_if(legsLeftOut, [](std::uint32_t id) {
                return std::ranges::find(scene, id, &Types::Actor::id) == scene.end();
            });
            sceneAnimations.clear();
            for (const auto& actor : scene)
            {
                for (const auto& clip : actor.clips)
                    sceneAnimations.push_back(clip.animation);
                for (const auto& clip : actor.faceClips)
                    sceneAnimations.push_back(clip.animation);
            }
            std::ranges::sort(sceneAnimations);
            sceneAnimations.erase(std::unique(sceneAnimations.begin(), sceneAnimations.end()), sceneAnimations.end());
        }
        if (const auto loadouts = Components::Loadouts::GetCatalog(); weaponsCatalog != loadouts || !loadouts)
        {
            weaponsCatalog = loadouts;
            sceneWeapons.clear();
            sceneStowed.clear();
            for (const auto& actor : scene)
            {
                sceneWeapons[actor.id] = Components::Actors::ResolveWeapon(actor);
                sceneStowed[actor.id] = Components::Actors::ResolveStowedWeapon(actor);
            }
        }

        // what was built for actors that are gone or made of something else now
        for (auto& slot : built)
        {
            if (!slot.active)
                continue;
            const auto actor = std::ranges::find(scene, slot.id, &Types::Actor::id);
            if (actor == scene.end() || actor->models != slot.models ||
                GetClipAnimations(*actor) != slot.clipAnimations || GetClipIds(*actor) != slot.clipIds ||
                GetSceneWeapon(actor->id) != slot.weapon || GetSceneStowed(actor->id) != slot.stowed ||
                Functions::Com_GetClientDObj(slot.handle, 0) != slot.obj ||
                (legsChanged && (Components::Actors::GetBody(*actor, slot.gunClass) != slot.body ||
                                 Components::Actors::GetLegsLibrary(*actor, slot.body) != slot.legsWanted ||
                                 Components::Actors::GetFaceLibrary(*actor) != slot.faceWanted)))
                Release(slot);
        }
    }

    // the scene's actors in the order they are posed and drawn: attached actors after the others, so the actors they
    // are on are placed first
    std::vector<const Types::Actor*> GetDrawOrder()
    {
        std::vector<const Types::Actor*> order;
        for (const auto& actor : scene)
            if (actor.movement != Types::ActorMovement::Attached)
                order.push_back(&actor);
        for (const auto& actor : scene)
            if (actor.movement == Types::ActorMovement::Attached)
                order.push_back(&actor);
        return order;
    }

    Built* FindBuilt(std::uint32_t id)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        return slot != built.end() ? &*slot : nullptr;
    }

    // the DObj an actor is drawn from now and its pose: the one without the gun's magazine while a reload has it off
    std::pair<DObj*, cpose_t*> GetDrawn(Built& slot)
    {
        return slot.magazineOffGun ? std::pair(slot.unloadedObj, GetClientEntityPose(slot.unloadedHandle))
                                   : std::pair(slot.obj, &GetPose(slot));
    }

    // Animates and places the built actors for a tick and turns their heads, which is all their skeletons are worked
    // out from; Built::posed says which are shown (and their bones can be read, see GetDrawnBone).
    // the reloads each slot was posed with, which it is drawn with in the same frame (a profile worked out meanwhile
    // waits for the next)
    std::array<ReloadsNow, MAX_ACTORS> posedReloads;

    void PoseActors(std::uint32_t now)
    {
        for (const auto actorPointer : GetDrawOrder())
        {
            const auto& actor = *actorPointer;
            const auto slot = FindBuilt(actor.id);
            if (!slot)
                continue;
            slot->posed = false;
            // (one a look list previews going through its stretch again and again, whatever the playhead does)
            const auto tick = Components::Actors::GetPreviewTick(actor.id).value_or(now);
            if (!Components::Actors::IsVisible(actor, tick))
            {
                ClearJointControls(slot - built.data());
                // (a dying killstreak's trails stop with it, the smoke they left drifting off, as when the game
                // deletes it)
                for (const auto handle : std::exchange(slot->eventLoops, {}))
                    Functions::FX_KillEffect(0, handle, false);
                continue;
            }

            const auto shown =
                slot->tree ? Animate(slot->obj, slot->tree, *slot, slot->weights, actor, tick) : ActorLogic::TreeWeights{};
            slot->animatedTick = tick;

            const auto placement = PlaceActor(actor, tick);
            if (!placement)
            {
                ClearJointControls(slot - built.data());
                continue;
            }
            if (actor.movement == Types::ActorMovement::Attached)
                Components::Actors::SetDrawnPlacement(actor.id, *placement);

            auto& pose = GetPose(*slot);
            reinterpret_cast<centity_t&>(pose).flags |= CENTITY_FLAG_HAS_MODEL;
            pose.origin[0] = placement->position.x;
            pose.origin[1] = placement->position.y;
            pose.origin[2] = placement->position.z;
            pose.angles[0] = pose.angles[2] = 0.0f;
            pose.angles[1] = placement->yaw;
            PoseVehicle(*slot, actor, tick, pose);

            // drawn without the gun's magazine while a reload has it off the gun; the actor's own DObj still gets the
            // pose, for the effects bolted to it
            auto reload = GetReloadsNow(*slot, actor, tick);
            slot->magazineOffGun = reload.magazines.state.offGun && (slot->unloadedObj || BuildUnloaded(*slot));
            if (slot->magazineOffGun)
            {
                auto& unloadedPose = *GetClientEntityPose(slot->unloadedHandle);
                std::memcpy(unloadedPose.origin, pose.origin, sizeof(pose.origin));
                std::memcpy(unloadedPose.angles, pose.angles, sizeof(pose.angles));
            }
            posedReloads[slot - built.data()] = std::move(reload);
            GripLeftGun(*slot, ActorLogic::GripsLeftGun(shown));
            for (const auto objIndex : {0, 1})
            {
                auto& rig = gripRigs[(slot - built.data()) * 2 + objIndex];
                rig.lying = ActorLogic::IsProneShown(shown);
                rig.share = GripLogic::GetGripShare(shown);
                // (made for SP bodies, as the spine tells: see PoseLogic::SpineKind)
                rig.campaign = GripLogic::GetCampaignShare(shown, [&](std::string_view animation) {
                    const auto spine = slot->spines.find(animation);
                    return spine != slot->spines.end() && spine->second == SpineKind::Lined;
                });
            }
            const auto [drawnObj, drawnPose] = GetDrawn(*slot);
            TurnHead(*slot, actor, tick, drawnObj, *drawnPose);
            slot->posed = true;
        }
    }

    void AddActors(std::int32_t localClientNum)
    {
        if (!CanAddActors(localClientNum))
            return;
        const auto packs = ContentPacks::GetPackState();
        UpdateScene();

        // the store's animations the scene's clips name: loaded as it loads or changes, before the actors are built,
        // and kept while it names them; what nothing uses goes once the store takes too much
        AnimationStore::RunFrame(sceneAnimations, Components::CaptureManager::Get().IsCapturing());

        const auto tick = Components::Playback::GetActorTick();
        const bool playEvents = eventTick && tick > *eventTick && tick - *eventTick <= MAX_EVENT_STEP;
        const auto eventsFrom = eventTick.value_or(tick);
        eventTick = tick;

        const auto order = GetDrawOrder();
        for (const auto actorPointer : order)
        {
            const auto& actor = *actorPointer;
            // (an effect, a light or a prop in the scene has no models of its own: see PlaySceneEffects,
            // Lights::AddToScene, DrawPlacedProps)
            if (actor.effect || actor.light || actor.placedProp || actor.mortar || FindBuilt(actor.id))
                continue;
            // tried again once another pack is loaded
            if (const auto it = failed.find(actor.id); it != failed.end() && it->second == packs.generation)
                continue;
            const auto slot = std::ranges::find(built, false, &Built::active);
            if (slot == built.end())
                break;
            if (!Build(*slot, actor))
            {
                failed[actor.id] = packs.generation;
                continue;
            }
            failed.erase(actor.id);
        }

        PoseActors(tick);

        // the killstreaks' running effects, of those shown
        for (auto& slot : built)
        {
            if (!slot.active || !slot.killstreak)
                continue;
            const auto actor = std::ranges::find(scene, slot.id, &Types::Actor::id);
            RunEffects(slot, slot.posed, actor != scene.end() ? &*actor : nullptr, tick);
        }

        for (const auto actorPointer : order)
        {
            const auto& actor = *actorPointer;
            const auto slot = FindBuilt(actor.id);
            if (!slot || !slot->posed)
                continue;

            const auto& pose = GetPose(*slot);
            const glm::vec3 lightingOrigin(pose.origin[0], pose.origin[1], pose.origin[2] + 32.0f);
            const auto [drawnObj, drawnPoseRef] = GetDrawn(*slot);
            auto& drawnPose = *drawnPoseRef;
            // (both guns of a dual wield one in its camo)
            const auto guns = (1u << slot->weaponModel) | (slot->leftVariant ? 1u << slot->leftGunModel : 0u);
            AddToScene(drawnObj, drawnPose, slot->magazineOffGun ? slot->unloadedHandle : slot->handle, lightingOrigin,
                       slot->variant, actor.camo, guns, 0, actor.facePaint, 1.0f,
                       {slot->stowedVariant, actor.stowedCamo, slot->stowedModel});
            DrawMagazines(*slot, actor, tick, posedReloads[slot - built.data()], lightingOrigin);

            // the models on its bones
            BuildProps(*slot, actor);
            for (std::size_t i = 0; i < slot->props.size() && i < actor.props.size(); ++i)
            {
                const auto& prop = slot->props[i];
                float axis[3][3] = {}, origin[3] = {};
                if (!prop.obj || !Functions::CG_DObjGetWorldTagMatrix(&drawnPose, drawnObj,
                                                                     GetScriptString(actor.props[i].tag), axis, origin))
                    continue;
                auto& propPose = *GetClientEntityPose(prop.handle);
                SetPose(propPose, ToMatrix(axis, origin) * glm::translate(glm::mat4(1.0f), actor.props[i].offset) *
                                      AnglesToMatrix(actor.props[i].angles));
                AddToScene(prop.obj, propPose, prop.handle, lightingOrigin, nullptr, 0, 0, RENDER_FLAG_SKINNED);
            }

            if (playEvents)
            {
                for (const auto& fired : Components::Actors::GetFiredEvents(actor, eventsFrom, tick))
                    PlayEvent(*slot, actor, *fired.event, fired.shot);
                PlayKillstreakTicks(*slot, actor, eventsFrom, tick);
                for (const auto& passed : Components::Actors::GetPassedNotetracks(actor, eventsFrom, tick))
                    PlayNotetrack(*slot, actor, *passed.clip, passed.name, passed.shot, tick);
            }
        }

        for (auto& slot : built)
            if (slot.active)
                UpdateFireLoop(slot, tick, playEvents && slot.posed);
        PlaySceneEffects(tick);
        PlayDrops(tick);
        PlayRockets(tick);
        DrawPlacedProps(tick);
        EndSceneModels();
        Lights::AddToScene(scene, tick);
    }

    std::optional<Types::TargetBone> GetDrawnBone(std::uint32_t id, const std::string& bone)
    {
        const auto slot = FindBuilt(id);
        if (!slot || !slot->posed || unloaded)
            return std::nullopt;
        const auto [obj, pose] = GetDrawn(*slot);
        float axis[3][3] = {}, origin[3] = {};
        if (!obj || !Functions::CG_DObjGetWorldTagMatrix(pose, obj, GetScriptString(bone), axis, origin))
            return std::nullopt;
        return Types::TargetBone{glm::make_vec3(origin), glm::make_mat3(&axis[0][0]), glm::make_vec3(pose->origin),
                                 pose->angles[1]};
    }

    std::vector<std::string> GetBuiltBones(std::uint32_t id)
    {
        const auto slot = FindBuilt(id);
        std::vector<std::string> bones;
        for (std::uint32_t i = 0; slot && slot->obj && i < slot->obj->numModels; ++i)
        {
            for (auto& name : AnimationPose::GetBoneNames(*slot->obj->models[i]))
            {
                if (std::ranges::find(bones, name) == bones.end())
                    bones.push_back(std::move(name));
            }
        }
        return bones;
    }

    void SetJointOverride(std::uint32_t id, const std::string& bone, std::optional<glm::vec3> angles)
    {
        auto& overrides = jointOverrides[id];
        std::erase_if(overrides, [&](const auto& entry) { return entry.first == bone; });
        if (angles)
            overrides.emplace_back(bone, *angles);
        if (overrides.empty())
            jointOverrides.erase(id);
    }

    void ControlJoints(DObj* obj, int* partBits)
    {
        for (std::size_t group = 0; group < jointsControlled.size(); ++group)
        {
            if (jointsControlled[group] <= 0)
                continue;
            for (std::size_t joint = 0; joint < MAX_JOINT_CONTROLS; ++joint)
            {
                const auto& control = jointControls[group * MAX_JOINT_CONTROLS + joint];
                const auto bone = control.bone.load();
                // (heads are turned after the animations, see BlendHeads)
                if (bone < 0 || control.obj != obj || control.weight >= 0.0f)
                    continue;
                const float angles[3] = {control.angles[0], control.angles[1], control.angles[2]};
                const float trans[3] = {control.trans[0], control.trans[1], control.trans[2]};
                Functions::DObjSetLocalTag(obj, partBits, bone, trans, angles);
            }
        }
    }

    std::vector<PropState> GetPropStates(std::uint32_t id)
    {
        std::vector<PropState> states;
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        const auto actor = std::ranges::find(scene, id, &Types::Actor::id);
        if (slot == built.end() || actor == scene.end())
            return states;
        for (std::size_t i = 0; i < slot->props.size() && i < actor->props.size(); ++i)
        {
            const auto& prop = slot->props[i];
            PropState state{.model = prop.model, .drawn = prop.obj != nullptr};
            float axis[3][3] = {}, origin[3] = {};
            if (prop.obj && Functions::CG_DObjGetWorldTagMatrix(&GetPose(*slot), slot->obj,
                                                               GetScriptString(actor->props[i].tag), axis, origin))
            {
                const auto& pose = *GetClientEntityPose(prop.handle);
                state.position = glm::make_vec3(pose.origin);
                const auto expected =
                    glm::vec3(ToMatrix(axis, origin) * glm::vec4(actor->props[i].offset, 1.0f));
                state.distance = glm::distance(state.position, expected);
            }
            states.push_back(state);
        }
        return states;
    }

    std::optional<float> GetLookError(std::uint32_t id)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        const auto actor = std::ranges::find(scene, id, &Types::Actor::id);
        if (slot == built.end() || actor == scene.end() || !slot->head)
            return std::nullopt;
        const auto look = Components::Actors::GetLook(*actor, Components::Playback::GetActorTick());
        const auto target = look.to ? FindLookTarget(*look.to) : std::nullopt;
        if (!target)
            return std::nullopt;

        // the eyes' tag looks along its x axis, and is on the head model's own head bone, which its face follows
        float axis[3][3]{}, origin[3]{};
        glm::vec3 face;
        if (Functions::CG_DObjGetWorldTagMatrix(&GetPose(*slot), slot->obj, GetScriptString("tag_eye"), axis, origin))
            face = glm::make_vec3(axis[0]);
        else if (Functions::CG_DObjGetWorldTagMatrix(&GetPose(*slot), slot->obj, GetScriptString("j_head"), axis,
                                                     origin))
            face = glm::make_mat3(&axis[0][0]) * slot->head->forward;
        else
            return std::nullopt;
        const auto direction = glm::normalize(*target - glm::make_vec3(origin));
        return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(face), direction), -1.0f, 1.0f)));
    }

    std::vector<std::string> GetBuiltModels(std::uint32_t id)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        std::vector<std::string> models;
        for (std::uint32_t i = 0; slot != built.end() && slot->obj && i < slot->obj->numModels; ++i)
            models.emplace_back(slot->obj->models[i]->name);
        return models;
    }

    std::vector<std::string> GetFirstLodMaterials(std::uint32_t id)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        std::vector<std::string> materials;
        if (slot == built.end() || !slot->obj || slot->obj->numModels < 1)
            return materials;
        // (the body, after its spine rig if it has one: the first model with surfaces)
        for (std::uint32_t i = 0; i < slot->obj->numModels && materials.empty(); ++i)
        {
            const auto& model = *slot->obj->models[i];
            const auto& lod = model.lodInfo[0];
            for (std::uint32_t s = lod.surfIndex;
                 model.materialHandles && s < static_cast<std::uint32_t>(lod.surfIndex + lod.numsurfs); ++s)
                if (const auto material = model.materialHandles[s])
                    materials.emplace_back(material->name);
        }
        return materials;
    }

    std::vector<std::string> GetStoredAnimations(std::uint32_t id)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        std::vector<std::string> keys;
        for (std::size_t i = 0; slot != built.end() && i < slot->stored.size(); ++i)
            keys.emplace_back(slot->stored[i].Key());
        return keys;
    }

    std::optional<AnimationDelta> GetAnimationDelta(std::uint32_t id, std::uint32_t clipId, float from, float to)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        if (slot == built.end() || !slot->anims)
            return std::nullopt;
        const auto index = static_cast<std::size_t>(std::ranges::find(slot->clipIds, clipId) - slot->clipIds.begin());
        if (index >= slot->clipParts.size() || !slot->clipParts[index]->delta)
            return std::nullopt;
        AnimationDelta delta;
        float translation[3] = {};
        Functions::XAnimGetRelDelta(slot->anims, FIRST_CLIP_INDEX + static_cast<std::uint32_t>(index), delta.rotation,
                                    translation, from, to);
        delta.translation = {translation[0], translation[1], translation[2]};
        return delta;
    }

    std::optional<TreeState> GetTreeState(std::uint32_t id)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        if (slot == built.end())
            return std::nullopt;
        TreeState state;
        state.gunClass = slot->gunClass;
        state.body = slot->body;
        if (const auto variant = slot->weapon.empty() ? nullptr : WeaponRig::FindVariant(slot->weapon);
            variant && variant->weapDef)
        {
            state.playerAnimType = variant->weapDef->playerAnimType;
            state.weapClass = variant->weapDef->weapClass;
        }
        state.legs = slot->legsAnimations;
        state.faces = slot->faceAnimations;
        auto names = slot->clipAnimations;
        names.insert(names.end(), slot->legsAnimations.begin(), slot->legsAnimations.end());
        names.insert(names.end(), slot->faceAnimations.begin(), slot->faceAnimations.end());
        if (!slot->clipSpines.empty())
        {
            names.insert(names.end(), {"lined", "resting"});
            for (const auto& [legs, aimed] : slot->aimedSpines)
                names.push_back("aimed " + slot->legsAnimations[legs]);
        }
        for (std::size_t i = 0; i < slot->weights.size() && i < names.size(); ++i)
            state.weights.emplace_back(names[i], slot->weights[i] / WEIGHT_SCALE);
        const auto faces = slot->clipAnimations.size() + slot->legsAnimations.size();
        for (std::size_t i = 0; i < slot->faceAnimations.size() && faces + i < slot->weights.size(); ++i)
            state.faceWeights.emplace_back(slot->faceAnimations[i], slot->weights[faces + i] / WEIGHT_SCALE);
        state.legsLeftOut = legsLeftOut.contains(id);
        return state;
    }

    std::optional<SpineState> GetSpineState(std::uint32_t id)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        if (slot == built.end() || !slot->obj)
            return std::nullopt;
        SpineState state;
        state.rig = slot->spineRig;
        for (std::uint8_t i = 0; i < slot->obj->numModels && !state.stabilizer; ++i)
            state.stabilizer = AnimationPose::FindSpineRig(*slot->obj->models[i]).has_value();
        // the clips' entries, then the legs' (see Build)
        const auto clips = slot->clipParts.size();
        for (std::size_t i = 0; i < clips + slot->legsParts.size() && i < slot->weights.size(); ++i)
        {
            const auto& parts = i < clips ? *slot->clipParts[i] : *slot->legsParts[i - clips];
            const auto weight = slot->weights[i] / WEIGHT_SCALE;
            if (AnimationPose::ClassifyAnimation(parts).family == Types::AnimationFamily::MpPlayer)
                state.mpWeight += weight;
            if (AnimationPose::GetSpineKind(parts) == SpineKind::Lined)
                state.spWeight += weight;
        }
        // (the spine's entries after the face's)
        const auto spine = clips + slot->legsParts.size() + slot->faceParts.size();
        if (!slot->clipSpines.empty() && spine + 1 < slot->weights.size())
        {
            state.lined = slot->weights[spine] / WEIGHT_SCALE;
            state.resting = slot->weights[spine + 1] / WEIGHT_SCALE;
            state.aimed = 0.0f;
            for (std::size_t i = spine + 2; i < slot->weights.size(); ++i)
                *state.aimed += slot->weights[i] / WEIGHT_SCALE;
        }
        return state;
    }

    void SetLegsLeftOut(std::uint32_t id, bool leftOut)
    {
        if (leftOut)
            legsLeftOut.insert(id);
        else
            legsLeftOut.erase(id);
        // (the magazines that fell were placed from where the game drew the gun then)
        for (auto& slot : built)
            if (slot.active && slot.id == id)
                slot.falls.clear();
    }

    // how far along a bone's axes its transforms are compared: about a hand or a magazine
    inline constexpr float COMPARED_LENGTH = 10.0f;

    std::optional<DecodedPoseCheck> CheckDecodedPose(std::uint32_t id, float later)
    {
        const auto slot = std::ranges::find_if(built, [&](const Built& b) { return b.active && b.id == id; });
        const auto actor = std::ranges::find(scene, id, &Types::Actor::id);
        if (slot == built.end() || actor == scene.end() || !slot->obj)
            return std::nullopt;
        DecodedPoseCheck check;
        const auto samples = slot->animatedTick ? Components::Actors::Evaluate(*actor, *slot->animatedTick)
                                                : std::vector<Types::ActorAnimationSample>{};
        const auto entry =
            samples.size() == 1 ? std::ranges::find(slot->clipIds, samples[0].clip->id) : slot->clipIds.end();
        if (entry == slot->clipIds.end())
        {
            check.error = std::format("it shows {} clips", samples.size());
            return check;
        }
        // Legs under the clip move its root and stabilizer, and so all of its upper body: the clip alone is checked
        // only without them. (The decoder would need their tracks blended in.)
        if (!legsLeftOut.contains(id) &&
            !Components::Actors::GetLayers(*actor, *slot->animatedTick, slot->body).legs.empty())
        {
            check.error = "it has legs under the clip (leave them out with: legs <actor> off)";
            return check;
        }
        const auto& sample = samples[0];
        const auto& parts = *slot->clipParts[entry - slot->clipIds.begin()];
        check.animation = sample.clip->animation;
        check.weight = sample.weight;
        check.time = GetAnimationTime(parts, *sample.clip, sample.time);
        // (the body after its spine rig, which the decoder composes itself)
        const auto first = slot->obj->models[0];
        const auto body = slot->obj->models[slot->spineRig && slot->obj->numModels > 1 ? 1 : 0];
        const auto kind = AnimationPose::GetSpineKind(parts);
        check.spine = kind == SpineKind::Moves              ? "moved"
                      : kind == SpineKind::AtRest           ? "at rest"
                      : AnimationPose::FindSpineRig(*first) ? "lined up"
                                                            : "an SP body's";
        if (slot->spineRig)
            check.spine += " on the spine rig";

        const auto& game = Mod::GetGameInterface();
        const auto pose = game->GetAnimationPoseByName(check.animation, body->name, check.time + later);
        if (!pose)
        {
            check.error = "the animation isn't decoded";
            return check;
        }
        auto& drawn = GetPose(*slot);
        const auto placement = glm::translate(glm::mat4(1.0f), glm::make_vec3(drawn.origin)) *
                               AnglesToMatrix(glm::make_vec3(drawn.angles));
        const auto decodedAt = [&](const std::string& bone) -> std::optional<glm::mat4> {
            const auto index = pose->FindBone(bone);
            return index ? std::optional(placement * PoseLogic::GetTransform(*pose, *index)) : std::nullopt;
        };
        const auto compare = [&](const std::string& name, std::optional<glm::mat4> decoded) {
            float axis[3][3] = {}, origin[3] = {};
            const auto inGame =
                Functions::CG_DObjGetWorldTagMatrix(&drawn, slot->obj, GetScriptString(name), axis, origin)
                    ? std::optional(ToMatrix(axis, origin))
                    : std::nullopt;
            if (!decoded && !inGame)
                return;
            auto& bone = check.bones.emplace_back(DecodedPoseCheck::Bone{.name = name});
            if (!decoded || !inGame)
                return;
            bone.distance = PoseLogic::GetTransformDistance(*decoded, *inGame, COMPARED_LENGTH);
            bone.origins = glm::distance(glm::vec3((*decoded)[3]), glm::vec3((*inGame)[3]));
        };
        for (const auto bone : {"j_spine4", "j_wrist_le", "j_wrist_ri", "tag_inhand", "tag_weapon_right"})
            compare(bone, decodedAt(bone));

        // the gun's magazine, on the gun in the right hand
        if (const auto rig = slot->variant ? game->GetWeaponRig(slot->weapon) : std::nullopt;
            rig && !rig->magazineBone.empty())
        {
            const auto hand = decodedAt("tag_weapon_right");
            compare(rig->magazineBone, hand ? std::optional(*hand * rig->magazine) : std::nullopt);
            // and the rig alone: where the game has the magazine from the gun's root, on tag_weapon_right
            float axis[3][3] = {}, origin[3] = {}, magazineAxis[3][3] = {}, magazineOrigin[3] = {};
            if (Functions::CG_DObjGetWorldTagMatrix(&drawn, slot->obj, GetScriptString("tag_weapon_right"), axis,
                                                    origin) &&
                Functions::CG_DObjGetWorldTagMatrix(&drawn, slot->obj, GetScriptString(rig->magazineBone), magazineAxis,
                                                    magazineOrigin))
                check.rigToGame = PoseLogic::GetTransformDistance(
                    rig->magazine, glm::inverse(ToMatrix(axis, origin)) * ToMatrix(magazineAxis, magazineOrigin),
                    COMPARED_LENGTH);
        }
        return check;
    }

    BoneCheck CheckBones(std::size_t maxBuilt)
    {
        typedef DObj*(__cdecl * Com_ClientDObjCreate_t)(DObjModel_s * models, std::uint32_t numModels,
                                                        XAnimTree_s* tree, std::int32_t handle,
                                                        std::int32_t localClientNum);
        typedef void(__cdecl * Com_SafeClientDObjFree_t)(std::int32_t handle, std::int32_t localClientNum);
        BoneCheck check;
        const auto catalog = Components::Actors::GetCatalog();
        if (!catalog)
            return check;

        using Kind = Types::ActorCatalog::ModelKind;
        std::vector<std::string> bodies, heads{""}, worn{""};
        for (const auto& model : catalog->models)
        {
            if (model.kind == Kind::Body || model.kind == Kind::FullBody)
                bodies.push_back(model.name);
            else
                (model.kind == Kind::Head ? heads : worn).push_back(model.name);
        }
        // the gun of the most bones of all
        XModel* gun = nullptr;
        const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        for (std::size_t index = 1; index < Functions::GetWeaponVariantCount(); ++index)
        {
            const auto weapon = variants[index]->weapDef;
            const auto model = weapon && weapon->worldModel ? weapon->worldModel[0] : nullptr;
            if (model && (!gun || model->numBones > gun->numBones))
                gun = model;
        }
        check.gun = gun ? std::format("{} ({} bones)", gun->name, gun->numBones) : "none";
        std::vector<XModel*> guns{nullptr};
        if (gun)
            guns.push_back(gun);

        for (const auto& body : bodies)
        {
            for (const auto& head : heads)
            {
                for (const auto& wearing : worn)
                {
                    std::vector<std::string> names{body};
                    for (const auto& model : {head, wearing})
                        if (!model.empty())
                            names.push_back(model);
                    for (const auto withGun : guns)
                    {
                        // with the spine rig, for the bodies that get it with clips made for MP bodies
                        const auto fitted = FitModels(names, withGun, nullptr, true);
                        if (!fitted)
                            continue;
                        ++check.characters;
                        if (fitted->leftOut.bones <= MAX_DOBJ_BONES)
                            continue;
                        ++check.overLimit;
                        const auto bones = CountBones(fitted->models);
                        if (bones > MAX_DOBJ_BONES)
                            ++check.overBuilt;
                        if (fitted->models.empty() && catalog->FindModel(body)->bones <= MAX_DOBJ_BONES)
                            ++check.bodyLeftOut;
                        if (check.examples.size() < 12)
                        {
                            std::string text;
                            for (const auto& name : names)
                                text += name + "+";
                            text += withGun ? withGun->name : "no gun";
                            std::string leftOut;
                            for (const auto& name : fitted->leftOut.models)
                                leftOut += name + " ";
                            check.examples.push_back(std::format("{} = {} bones, built of {}, left out {}{}", text,
                                                                 fitted->leftOut.bones, bones, leftOut,
                                                                 fitted->leftOut.gun ? "the gun" : ""));
                        }
                        // the game's own count, which drops out of the demo when there are too many
                        const auto handle = check.built < maxBuilt && !fitted->models.empty() && bones <= MAX_DOBJ_BONES
                                                ? FindFreeHandle()
                                                : std::nullopt;
                        if (!handle)
                            continue;
                        auto models = fitted->models;
                        if (reinterpret_cast<Com_ClientDObjCreate_t>(GetGameAddresses().Com_ClientDObjCreate())(
                                models.data(), static_cast<std::uint32_t>(models.size()), nullptr, *handle, 0))
                        {
                            ++check.built;
                            reinterpret_cast<Com_SafeClientDObjFree_t>(GetGameAddresses().Com_SafeClientDObjFree())(
                                *handle, 0);
                        }
                    }
                }
            }
        }
        return check;
    }

    typedef std::int32_t(__cdecl* CG_AddPacketEntities_t)(std::int32_t localClientNum);
    CG_AddPacketEntities_t CG_AddPacketEntities_Trampoline;
    std::int32_t __cdecl CG_AddPacketEntities_Hook(std::int32_t localClientNum)
    {
        Lights::BeginFrame();
        const auto result = CG_AddPacketEntities_Trampoline(localClientNum);
        AddActors(localClientNum);
        Bones::UpdateRequestedActorBones();
        Hooks::Camera::OnEntitiesAdded();
        Hooks::Effects::OnClientFrame();
        CampaignMaps::PlayMapEffects();
        return result;
    }

    void Unload()
    {
        unloaded = true;
        Lights::Unload();
        StopEffects();
        // The game's other threads may be working out the skeletons of the actors' DObjs right now (freeing them here
        // crashed the game in CG_DoControllers), so they stay, drawn no more, until the game frees every DObj with the
        // map, and what they use that the mod made is left to the process
        for (auto& slot : built)
        {
            static_cast<void>(slot.linedSpine.release());
            static_cast<void>(slot.restingSpine.release());
            for (auto& [legs, aimed] : slot.aimedSpines)
                static_cast<void>(aimed.release());
        }
        for (auto& [gun, model] : magazineModels)
            static_cast<void>(model.release());
        for (auto& [original, model] : outsideModels)
            static_cast<void>(model.release());
        for (auto& [original, model] : holdingModels)
            static_cast<void>(model.release());
        for (auto& [original, model] : playerBodies)
            static_cast<void>(model.release());
        for (auto& rig : spineRigs)
            static_cast<void>(rig.release());
        AnimationStore::Unload();
    }

    void ResetForMap()
    {
        // the models and animations of actors go away with the map; core keeps its actors until the next demo's load
        ReleaseAll();
        for (const auto& [key, model] : sceneModels)
            ReleaseSceneModel(model);
        sceneModels.clear();
        firedDrops.clear();
        // the store's animations too (freed once the game can't be using them), and the pack is looked at again,
        // as the setup may have built it meanwhile
        AnimationStore::Clear();
        AnimationStore::Open();
        scene.clear();
        sceneGeneration.reset();
        sceneAnimations.clear();
        catalogGeneration.reset();
        catalogStoreGeneration.reset();
        failed.clear();
        legsLeftOut.clear();
        sceneWeapons.clear();
        sceneStowed.clear();
        rockets.clear();
        rocketsTick.reset();
        weaponsCatalog.reset();
        eventTick.reset();
        ForgetSceneEffects();
        scriptStrings.clear();
        impactWeapons.clear();
        // the effects go with the map, and the models the magazines, spine rigs and killstreaks' outsides were
        // copied from
        effectHandles.clear();
        effectHandlesForgetAt = 0;
        magazineModels.clear();
        outsideModels.clear();
        holdingModels.clear();
        playerBodies.clear();
        spineRigs.clear();
        stowedRig.reset();
        linedSpines.clear();
        spineSource = nullptr;
        AnimationPose::SetSpineChain(std::nullopt);
        Components::Actors::SetCatalog(nullptr);
    }

    void Install()
    {
        AnimationStore::Open();

        Events::RegisterListener(EventType::PreDemoLoad, ResetForMap);

        HookManager::CreateHook(GetGameAddresses().CG_AddPacketEntities(), (std::uintptr_t)CG_AddPacketEntities_Hook,
                                (std::uintptr_t*)&CG_AddPacketEntities_Trampoline);
    }
}  // namespace IWXMVM::T5::Actors
