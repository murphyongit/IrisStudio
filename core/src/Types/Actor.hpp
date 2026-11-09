#pragma once
#include "Types/BoneCamera.hpp"
#include "Types/Character.hpp"

namespace IWXMVM::Types
{
    // A named point in an animation, e.g. where a reload drops the magazine ("drop clip") or a shot goes off ("fire")
    struct ActorNotetrack
    {
        std::string name;
        float time = 0.0f;  // from 0 (the start) to 1 (the end)

        bool operator==(const ActorNotetrack&) const = default;
    };

    // What an animation moves, from the bones it has tracks for (see PoseLogic::ClassifyAnimation)
    enum class AnimationKind
    {
        FullBody,  // the body from its root (j_mainroot) or its legs on
        Torso,     // the spine and what is on it, but not the root nor the legs (MP's pt_ animations)
        Face,      // a face's bones alone
        Other,     // a dog's, first person arms', a prop's
    };

    // The rig an animation was made for, whose bones its tracks are relative to (see PoseLogic::ClassifyAnimation)
    enum class AnimationFamily
    {
        MpPlayer,  // MP bodies, which hang j_spinelower from torso_stabilizer: the players' pb_ and pt_ animations
        SpHuman,   // SP bodies (the AI's and Zombies'), which hang j_spinelower from j_mainroot
        Human,     // the bones both of those have alike: a face's, or a body's without j_spinelower
        Dog,
        Viewmodel,  // first person arms
        Other,      // props, vehicles and the like
    };

    struct AnimationEntry;

    // What an actor is, by its body (see Components::Actors::GetActorKind): what it plays, holds and does
    enum class ActorKind
    {
        Human,  // a person (the players', the campaign's and Zombies' characters): a gun in its right hand, legs under
                // its upper body clips and a face of its own
        Dog,  // a dog (multiplayer's german shepherd, the campaign's and Zombies' dogs): the dogs' animations, nothing
              // in its mouth, no face of a person's
        Vehicle,  // one of multiplayer's killstreaks the game moves in code (see Killstreak): its motions, no
                  // animations but its own
        Effect,  // an effect of the map placed on its own (see ActorEffect): no models, clips or events
        Light,   // a light placed on its own (see ActorLight): no models, clips or events
        Prop,    // a model placed on its own (see ActorPlacedProp): no clips or events
        Mortar,  // the Mortar Team (see ActorMortar): no models or clips, its Fire events' shells
    };

    // The killstreaks of multiplayer that actors can be, each its model of its own (see Components::KillstreakLogic)
    enum class Killstreak
    {
        RcXd,
        AttackHelicopter,    // the Cobra
        Gunship,             // the Hind
        ChopperGunner,       // the Huey
        CarePackageChopper,  // the CH-46 that brings the care package
        CarePackage,         // the crate
        SpyPlane,            // the U2 (of the Counter-Spy Plane too)
        Napalm,              // the F4
        RollingThunder,      // the B-52
        SentryGun,
        SamTurret,
    };

    // How a killstreak moves: on wheels, flying with rotors or wings, standing and aiming, or falling
    enum class VehicleClass
    {
        Car,
        Helicopter,
        Plane,
        Turret,
        Crate,
    };

    // How damaged a killstreak's helicopter shows: the game's engine smoke of its damage states (Damaged)
    enum class ActorDamage : std::uint8_t
    {
        None,
        Light,
        Heavy,
    };

    // What a game offers for building actors: the characters and character models and animations it has loaded, and
    // the content packs (single player levels, Zombies maps) it can load more of them from
    struct ActorCatalog
    {
        enum class ModelKind
        {
            Body,      // needs a head
            FullBody,  // has its head
            Head,
            Gear,  // hats, helmets and the like, worn on the head
        };

        struct Model
        {
            std::string name;
            ModelKind kind;
            std::string source;       // "Multiplayer", or the display name of the pack it came from
            std::uint32_t bones = 0;  // how many it has
            bool facePaint = false;   // it has a face the game can paint (the campaign's heads too, on a map that can)
        };

        struct Pack
        {
            std::string name;  // the zone, e.g. vorkuta
            std::string displayName;
            std::string group;  // Campaign or Zombies
        };

        struct Animation
        {
            std::string name;
            float duration;  // seconds
            bool loop;       // made to loop, such as idles and walks
            std::vector<ActorNotetrack> notetracks;  // sorted by time
            AnimationKind kind = AnimationKind::Other;
            AnimationFamily family = AnimationFamily::Other;
            // what the catalog of every animation the game has says of it (see Components::Actors::
            // GetAnimationCatalog), if it has it
            const AnimationEntry* entry = nullptr;
            // one the game hasn't loaded, which the animation store (T5's AnimationStore) loads from the animation
            // pack as an actor plays it (see Components::Actors::IsStoreLoaded)
            bool stored = false;
        };

        // How an effect plays by itself, from its definition: how many of its elements loop and for how long, and how
        // many play once (an explosion's sparks loop for a few hundred ms: it plays once)
        struct EffectInfo
        {
            std::uint32_t loopingLife = 0;  // ms; FOREVER for one that loops till it is stopped
            std::uint32_t loopingElements = 0;
            std::uint32_t oneShotElements = 0;
            static constexpr std::uint32_t FOREVER = 0x7FFFFFFF;

            bool operator==(const EffectInfo&) const = default;
        };

        std::vector<Model> models;
        std::vector<Animation> animations;  // sorted by name
        std::vector<std::string> effects;   // the effects the game has loaded, sorted
        std::map<std::string, EffectInfo, std::less<>> effectInfo;  // of those, as far as the game tells
        std::vector<std::string> props;     // every model the game has loaded, to put on actors, sorted
        std::vector<std::string> mapProps;  // of those, the map's own (its world's zone), sorted
        std::vector<Pack> packs;
        // the characters actors can be: the map's own, then those of the campaign and Zombies (whose models may not be
        // loaded), and the levels these come from, in campaign order
        std::vector<Character> characters;
        std::vector<Pack> levels;
        // the character zones loaded (see Components::Actors::GetWantedCharacterZones), and why one didn't load
        std::vector<std::string> loadedZones;
        std::string zoneError;
        // whether the animation store can load the animations of the catalog the game hasn't (from the animation pack,
        // which tools/t5-characters/setup.ps1 builds), and why it can't
        bool animationStore = false;
        std::string animationStoreError;
        // the most bones an actor's models and the gun in its hand may have together (see ActorLogic::FitBones), 0 for
        // no limit
        std::uint32_t maxBones = 0;
        std::string loadedPack;                // empty for none
        std::string loadingPack;               // what is being loaded, if anything
        std::string packError;                 // why the last pack didn't load
        std::int32_t packMemoryMegabytes = 0;  // what the game uses after loading the pack

        const Model* FindModel(std::string_view name) const
        {
            const auto it = std::ranges::find(models, name, &Model::name);
            return it != models.end() ? &*it : nullptr;
        }

        const Animation* FindAnimation(std::string_view name) const
        {
            const auto it = std::ranges::lower_bound(animations, name, {}, &Animation::name);
            return it != animations.end() && it->name == name ? &*it : nullptr;
        }

        const Character* FindCharacter(std::string_view id) const
        {
            const auto it = std::ranges::find(characters, id, &Character::id);
            return it != characters.end() ? &*it : nullptr;
        }

        // whether every model of the list is loaded
        bool HasModels(const std::vector<std::string>& names) const
        {
            return std::ranges::all_of(names, [&](const std::string& name) { return FindModel(name) != nullptr; });
        }

        // whether models have a face to paint; models not loaded yet can't say, so they may, and none chosen are a
        // demo's player's, which may
        bool CanHaveFacePaint(const std::vector<std::string>& names) const
        {
            return names.empty() || !HasModels(names) || std::ranges::any_of(names, [&](const std::string& name) {
                return FindModel(name)->facePaint;
            });
        }

        const Pack* FindPack(std::string_view name) const
        {
            const auto it = std::ranges::find(packs, name, &Pack::name);
            return it != packs.end() ? &*it : nullptr;
        }
    };

    // Which way a clip walks an actor along its path, relative to where the actor faces
    enum class ActorWalkDirection
    {
        Auto,  // as its animation moves
        Forward,
        Back,
        Left,
        Right,
    };

    // What put a clip in among an actor's clips, if it wasn't the user: the transitions
    // Auto transitions inserts, which a join done again replaces instead of adding more (see TransitionLogic)
    enum class ActorChain
    {
        None,   // the user's
        Join,   // a transition between two clips (a stop, a start, a turn, a change of stance)
        Start,  // a path's: setting off from standing (at its start, or after a wait at a point)
        Turn,   // a path's: a turn at a corner
        Stop,   // a path's: stopping at its end, or at a point it waits at (with the idle it waits with)
        Walk,   // a path's: the walk on after a turn, cut from the walk before it
    };

    // What a one-off clip (a death, a reload, a gesture) hangs on: a time of the
    // timeline (a death's Hit goes with it), the end of the actor's path (as it arrives), or a point of it (as it
    // reaches it); it keeps its offset from that as the path is planned again (see ActorLogic::TakeOutOneOffs)
    enum class ActorAnchor
    {
        Time,
        End,
        Point,
    };

    // An animation an actor plays for a stretch of the timeline. Before its actor's first clip the actor holds that
    // clip's first frame, and after a clip ends it holds its last frame until the next clip starts.
    struct ActorClip
    {
        std::uint32_t id = 0;  // of the clip within its actor
        std::string animation;
        std::uint32_t start = 0;      // the timeline tick it starts at
        std::uint32_t length = 3000;  // in ticks of the timeline
        float speed = 1.0f;
        float offset = 0.0f;          // seconds into the animation it starts at
        bool loop = true;             // plays again from the start when it ends, else holds its last frame
        std::uint32_t blendIn = 250;  // ticks over which it takes over from the clip before
        bool moves = true;            // walks the actor along its path at the animation's own speed
        ActorWalkDirection direction = ActorWalkDirection::Auto;
        float walkSpeed = 0.0f;  // units a second at 1x instead of the animation's own; 0 for its own
        // a killstreak's Spin out: the spin it spins up to (degrees a second) and the speed it flies on at (units a
        // second), 0 for the killstreak's own
        float spinRate = 0.0f;
        float crashSpeed = 0.0f;
        bool notetrackShots = true;   // fires the actor's gun where the animation shoots (its fire notetracks)
        bool notetrackSounds = true;  // plays the sounds the animation names (its sndnt# notetracks)
        // the left hand on the gun as its grip has it, else as the animation has it
        // (its Left hand row)
        bool grip = true;
        // where it doesn't walk the path, moves the actor as far as its animation moves the root (a death falls where
        // it falls, see ActorLogic::RootMotion), else the actor stays in place
        bool rootMotion = true;
        ActorChain chain = ActorChain::None;
        // how far it turns the actor by its end (degrees to the left), as the game turns its AI through a turn: none
        // for as far as its animation turns (see TransitionLogic::GetTurnMotion)
        std::optional<float> turn;
        // a path's Turn, or the Stop of a wait: the point of the path whose corner it turns at or waits at (the last
        // copy of it)
        std::optional<std::uint32_t> corner;
        // chosen by the user from a list of animations (played, dropped, Then..., the clip's animation): it plays its
        // own animation as picked, never standing for the actor's locomotion (see ActorLogic::FindLocomotion)
        bool picked = false;
        // a one-off's: what it hangs on (see ActorAnchor), the ticks it starts after that (before it, negative) and
        // the point of the path for Point
        ActorAnchor anchor = ActorAnchor::Time;
        std::int32_t anchorOffset = 0;
        std::uint32_t anchorPoint = 0;

        std::uint32_t End() const
        {
            return start + length;
        }

        bool operator==(const ActorClip&) const = default;
    };

    // Where an actor stands at a tick of the timeline, for actors that move by keyframes
    struct ActorKey
    {
        std::uint32_t tick = 0;
        glm::vec3 position{};
        float yaw = 0.0f;  // degrees

        bool operator==(const ActorKey&) const = default;
    };

    enum class ActorMovement
    {
        Still,      // stands at its position
        Keyframes,  // between its keys
        Path,       // along its path, as far as its moving clips have walked it
        Attached,   // with a bone of another actor or of a player (see ActorAttachment)
    };

    // What an attached actor moves with: a bone of another actor or of a player of the demo, kept upright, turned
    // as the bone faces
    struct ActorAttachment
    {
        enum class Kind
        {
            Actor,
            Player,
        };
        Kind kind = Kind::Actor;
        std::uint32_t actor = 0;  // its id
        std::int32_t player = 0;  // the client number
        std::string tag = "tag_origin";
        glm::vec3 offset{};  // from the bone, along the way it faces (x forward, y left, z up)
        float yaw = 0.0f;    // degrees from the way the bone faces

        bool operator==(const ActorAttachment&) const = default;
    };

    // A model held by or worn on a bone of an actor, e.g. a radio in a hand or a bag on the back
    struct ActorProp
    {
        std::string model;
        std::string tag = "tag_weapon_left";
        glm::vec3 offset{};  // from the bone, along its axes
        glm::vec3 angles{};  // pitch, yaw and roll from the bone's axes, degrees

        bool operator==(const ActorProp&) const = default;
    };

    enum class ActorEventType
    {
        Fire,    // shoots its weapon: muzzle flash, sound, tracer and casing
        Hit,     // bleeds from a bone
        Effect,  // plays an effect on a bone
        Show,    // is drawn from here on
        Hide,    // isn't drawn from here on
        Look,    // turns its head to look at something from here on
    };

    // What a Look event has an actor look at
    enum class ActorLookTarget
    {
        Ahead,   // nothing: its head moves as its animation moves it
        Camera,  // the camera the demo is seen from
        Point,
        Actor,   // the head of another actor
        Player,  // the head of a player of the demo
    };

    // Something an actor does at a tick of the timeline, as the timeline plays past it
    struct ActorEvent
    {
        std::uint32_t id = 0;  // of the event within its actor
        ActorEventType type = ActorEventType::Fire;
        std::uint32_t tick = 0;
        std::uint32_t shots = 1;       // Fire: how many, interval ticks apart
        std::uint32_t interval = 100;  // a fast rifle fires every 80 to 100 ms
        bool tracers = true;           // Fire: draws a tracer for each shot
        bool hits = true;              // Fire: its bullets leave the game's hits, its rockets their blast (see ShotLogic)
        float spread = -1.0f;          // Fire: degrees its shots go off the barrel, below 0 the gun's own
        bool rockets = false;          // Fire: a killstreak's rockets in place of its gun (the Gunship's)
        std::string effect;            // Hit, Effect: the game's effect, e.g. impacts/fx_flesh_hit
        std::string tag;               // Hit, Effect: the bone it plays on, e.g. j_spine4
        ActorLookTarget lookTarget = ActorLookTarget::Camera;  // Look
        glm::vec3 lookPoint{};         // Look at a Point
        std::uint32_t lookActor = 0;   // Look at an Actor: its id
        std::int32_t lookPlayer = 0;   // Look at a Player: the client number
        std::uint32_t lookTurn = 400;  // Look: ticks it takes to turn there from what it looked at before
        // Hit: the clip of the death it dies with (its id; 0 for none), which starts as it goes off: moving either
        // moves both (see ActorLogic::Normalize). Effect, Hide: the clip of the death it is part of (a killstreak's),
        // which it goes off deathAt ticks after the start of (after its end with
        // afterDeathEnd), moving with it; it goes with it
        std::uint32_t death = 0;
        std::uint32_t deathAt = 0;
        bool afterDeathEnd = false;

        // the ticks its shots (one for the others) go off at
        std::uint32_t End() const
        {
            return type == ActorEventType::Fire && shots > 1 ? tick + (shots - 1) * interval : tick;
        }

        bool operator==(const ActorEvent&) const = default;
    };

    // How an actor's legs stand under its clips of upper body animations, and walk with Locomotion Auto (see
    // ActorLogic::GetLayers)
    enum class ActorStance
    {
        Auto,  // as the clip shown says: crouching or prone by its name, or by where its anim_pose notetracks put it
        Stand,
        Crouch,
        Prone,
    };

    // Whether an actor walks, runs and stands as the game's players do (see ActorLogic::GetLayers)
    enum class ActorLocomotion
    {
        Off,   // as its clips' animations play, walking its path at their own speed
        Auto,  // its clips of the players' standing, walking and running animations stand for the animation of its
               // gun the game picks for its speed, stance and way it goes, and walk its path at the actor's speed
    };

    // The animations a person walks, runs, turns, starts and stops with, chosen once
    // per actor: the game's players' (by the gun's class) or one of the campaign AI's styles (rifle)
    enum class ActorMovementSet
    {
        Auto,       // as its body was made for: the AI's low ready on a campaign body, the players' on another
        Players,    // the players': no starts or stops, turns on the spot (pb_turn_90...)
        LowReady,   // the AI's: walks and runs with the gun low, its starts, stops and running turns
        Cqb,        // the AI's close quarters: walks aiming, its own starts, stops and turns
        RunAndGun,  // the AI's: runs aiming forward, else as low ready
    };

    // Whether an actor's face moves on its own as the game's players' faces do (see ActorLogic::GetFace)
    enum class ActorFace
    {
        Off,   // as its clips' animations have it (most of them leave the face as it is at rest)
        Auto,  // an idle of the face (alert while it aims or shoots) that grimaces as it shoots, winces as it is hit
               // and dies with a clip of a death
    };

    // The kind of gun an actor holds, as far as the game's player animations have variants for it
    // (mp/playeranim.script): the legs under upper body clips stand as with such a gun
    enum class GunClass
    {
        Rifle,  // and anything else held in two hands, or nothing
        Pistol,
        RocketLauncher,
        DualWield,
        BallisticKnife,
        Minigun,  // the Death Machine, of a stance and walk of its own
    };

    // How fast an actor goes along a stretch of its path: a person's walk, run and
    // sprint (crouched: its crouched walk and run), a dog's walk, run and sprint as its breed has them
    enum class ActorGait
    {
        Walk,
        Run,
        Sprint,
    };

    // Where a join's blend sits against its point, as a cut's transition sits in an
    // editor: centred on it, ending at it, or starting at it
    enum class ActorJoinAt
    {
        Centred,
        Before,
        After,
    };

    // A stretch of an actor's path, from one of its points to the next (the last point's only waits): the gait it goes
    // at, as fast as that gait's animation goes for its body and movement set (see ActorLogic::GetStretchSpeed), and
    // how long it stands at the stretch's first point before going on
    struct ActorStretch
    {
        ActorGait gait = ActorGait::Walk;
        bool crouched = false;
        // the animation its gait plays going forward: its look (see ActorLogic::GetLook), else the one for the actor's
        // body and set as they were when chosen (see Components::Actors::ResolveStretches): the stretch goes as fast
        // as it does
        std::string animation;
        std::uint32_t wait = 0;  // ticks
        // a look of its own, over the actor's of its gait: empty for that
        std::string look;
        // its own pace, on top of the actor's
        float pace = 1.0f;
        // its join with the stretch before (see ActorLogic::HasJoin): how long the legs
        // and the speed blend over (ticks; none for Auto, see ActorLogic::GetJoinBlend), where that sits against its
        // first point, and whether the incoming look comes in on the next foot
        std::optional<std::uint32_t> blend;
        ActorJoinAt blendAt = ActorJoinAt::Centred;
        bool matchFeet = true;
        // a join kept as scenes from before joins had it: the legs blend over the game's least to a moving
        // animation from the point, the speed changes there and the feet fall as they may, until the join is changed
        bool joinAsBefore = false;

        bool operator==(const ActorStretch&) const = default;
    };

    // A gait's look: the animation an actor's stretches of that gait play going forward,
    // in place of its body's and set's own, as fast as it goes
    struct ActorLook
    {
        ActorGait gait = ActorGait::Walk;
        bool crouched = false;
        std::string animation;

        bool operator==(const ActorLook&) const = default;
    };

    // where an actor stands and which way it faces
    struct ActorPlacement
    {
        glm::vec3 position{};
        float yaw = 0.0f;  // degrees
    };

    // How an effect in the scene plays over its bar: as the effect is made (one
    // that loops for its looping life or till it is stopped, one that doesn't once), once, or again as it ends till the
    // bar's end
    enum class EffectPlays
    {
        Auto,
        Once,
        Loop,
    };

    // How it ends at its bar's end: its particles let finish, or gone at once
    enum class EffectEnds
    {
        Fade,
        Cut,
    };

    // An effect of the map placed in the scene on its own, not on an actor: an actor of no
    // models whose position is where it plays, facing (with the actor's yaw) the way its x axis fires
    struct ActorEffect
    {
        std::string name;  // the game's effect, e.g. destructibles/fx_dest_fire_lg
        float pitch = -90.0f;  // degrees, the game's way: -90 fires straight up (out of the ground)
        float roll = 0.0f;
        std::uint32_t start = 0;  // its bar on the timeline, ticks
        std::uint32_t end = 0;
        EffectPlays plays = EffectPlays::Auto;
        EffectEnds ends = EffectEnds::Fade;
        std::uint32_t every = 2000;  // ms: how often one that plays once plays again while it loops

        bool operator==(const ActorEffect&) const = default;
    };

    // A light in the scene: one that lights all around it, or a cone
    enum class LightKind
    {
        Point,
        Spot,
    };

    // Which gun a light on a player's bone goes by in that player's first person view:
    // the gun on screen for the gun's bones and the body for the rest, the body always, or the gun on
    // screen (its gun and arms) for all the bones it has
    enum class LightFirstPerson
    {
        Auto,
        Body,
        ViewGun,
    };

    // What a light follows: a bone of an actor or a player, from an offset along the bone's axes; a spot's
    // aim turns with the bone. Steady smooths the bone's sway over that long, in seconds, as the bone camera's does.
    struct LightFollow
    {
        BoneCameraTarget target;
        std::string bone;
        glm::vec3 offset{};
        LightFirstPerson firstPerson = LightFirstPerson::Auto;
        float steady = 0.0f;

        bool operator==(const LightFollow&) const = default;
    };

    // A light placed in the scene on its own: an actor of no models whose position is where it is, on from its bar's
    // start to its end. Colour, intensity, radius and cone can have keys (Components::LightLogic).
    struct ActorLight
    {
        std::uint32_t id = 0;  // its keys are filed under this, which stays across saves as an actor's id doesn't
        LightKind kind = LightKind::Point;
        glm::vec3 color{1.0f, 0.85f, 0.7f};  // as a colour row has it, gamma, 0 to 1
        float intensity = 1.0f;
        float radius = 300.0f;  // units
        // a spot's: its aim with the actor's yaw (the game's way: positive is down), how wide its cone is across, in
        // degrees, how much of its edge fades, whether it casts shadows (one spot at a time does), and how strong its
        // highlights are
        float pitch = 45.0f;
        float cone = 60.0f;
        float softness = 0.25f;
        bool shadows = false;
        float highlights = 1.0f;
        float flickerAmount = 0.0f;  // how far it dips, 0 to 1
        float flickerRate = 8.0f;    // how often it changes, a second
        std::uint32_t start = 0;     // its bar on the timeline, ticks
        std::uint32_t end = 0;
        std::optional<LightFollow> follow;

        bool operator==(const ActorLight&) const = default;
    };

    // A model of the map placed in the scene on its own, not on an actor: an actor of
    // no models whose position is where it stands, turned (with the actor's yaw) as the game turns a script model. Its
    // position and turn can have keys (Components::PropLogic).
    struct ActorPlacedProp
    {
        std::uint32_t id = 0;  // its keys are filed under this, which stays across saves as an actor's id doesn't
        std::string model;     // the game's model, e.g. p_rus_chair_metal
        float pitch = 0.0f;  // degrees, the game's way: positive is nose down
        float roll = 0.0f;
        float scale = 1.0f;
        std::uint32_t start = 0;  // its bar on the timeline, ticks
        std::uint32_t end = 0;

        bool operator==(const ActorPlacedProp&) const = default;
    };

    // The Mortar Team: no model, the points on the ground its Fire events'
    // shells fall on, one after another as the game's three do; they come in along the actor's yaw
    struct ActorMortar
    {
        std::vector<glm::vec3> points;

        bool operator==(const ActorMortar&) const = default;
    };

    // A character placed in a demo that the game draws on its own, playing clips of animations
    struct Actor
    {
        std::uint32_t id = 0;
        std::string name;
        std::vector<std::string> models;  // the body first; the others (head, gear) share its bones
        std::string character;            // the id of the character it is (see Types::Character), if known
        std::vector<ActorClip> clips;     // sorted by start; none for its rest pose
        ActorStance stance = ActorStance::Auto;  // of its legs under clips of upper body animations, and walking
        ActorLocomotion locomotion = ActorLocomotion::Off;
        float pathSpeed = 94.0f;  // with Locomotion Auto, units a second its walking clips walk its path at
        ActorMovementSet movementSet = ActorMovementSet::Players;
        // clips added after others are joined by the game's transitions (stops, starts, turns, changes of stance), and
        // with Locomotion Auto its path starts, turns at its corners and stops with them (see TransitionLogic)
        bool autoTransitions = false;
        ActorFace face = ActorFace::Off;
        // the Face row's clips of animations of the face alone, sorted by start: while one plays it is the face,
        // over what Face Auto does
        std::vector<ActorClip> faceClips;
        glm::vec3 position{};
        float yaw = 0.0f;  // degrees
        bool hidden = false;
        ActorMovement movement = ActorMovement::Still;
        std::vector<ActorKey> keys;   // sorted by tick
        std::vector<glm::vec3> path;  // the points it walks through, from the first
        // with Locomotion Auto, how it goes along each stretch of its path, by the point it starts at (a point past the
        // last stretch goes as that one); none for a path walked at pathSpeed, as before gaits
        std::vector<ActorStretch> stretches;
        // the looks given to its gaits (see ActorLook), one per gait at most; the others are its body's and set's own
        std::vector<ActorLook> looks;
        float pace = 1.0f;         // the stretches' speeds scaled
        bool followGround = true;  // on a path, kept on the ground below it
        // a flying killstreak's: how high above the ground the points of its path are put (units); 0 for others
        float flyHeight = 0.0f;
        // a killstreak's effects: the rows of them turned off, by key (see
        // KillstreakLogic::GetEffectRowKey), and Damaged: its engine smoking, from a tick on
        std::vector<std::string> effectsOff;
        ActorDamage damage = ActorDamage::None;
        std::uint32_t damagedFrom = 0;
        std::string weapon;                // held in its right hand, a base weapon of the loadouts (ak47_mp)
        std::vector<std::string> attachments;  // sorted
        std::int32_t camo = 0;
        // the gun on its back, as players carry their other one: a base weapon of the loadouts, empty for none
        std::string stowedWeapon;
        std::vector<std::string> stowedAttachments;  // sorted
        std::int32_t stowedCamo = 0;
        std::int32_t facePaint = 0;      // see Components::FacePaintLogic
        std::vector<ActorEvent> events;  // sorted by tick
        ActorAttachment attachment;      // when it moves Attached
        std::vector<ActorProp> props;
        std::optional<ActorEffect> effect;  // an effect in the scene's (see ActorKind::Effect); none for others
        std::optional<ActorLight> light;    // a light in the scene's (see ActorKind::Light); none for others
        std::optional<ActorPlacedProp> placedProp;  // a prop in the scene's (see ActorKind::Prop); none for others
        std::optional<ActorMortar> mortar;          // the Mortar Team's; none for others

        bool operator==(const Actor&) const = default;
    };

    // What an actor shows at a tick: one animation, or two while a clip blends in over the one before
    struct ActorAnimationSample
    {
        const ActorClip* clip;
        float time;    // seconds into the animation, before it loops or holds its last frame
        float weight;  // the samples' weights add up to 1
    };
}  // namespace IWXMVM::Types
