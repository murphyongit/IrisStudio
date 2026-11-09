#pragma once
#include "Types/Actor.hpp"
#include "Components/ActorLogic.hpp"

#include <span>

namespace IWXMVM::Components::KillstreakLogic
{
    // Multiplayer's killstreaks as actors: what each is made of and how the game moves
    // and shows it, as its scripts and vehicle definitions have it, and the motions their clips are of. No game state;
    // tested in tests/actors-tests.

    // Which team's colours a killstreak's look is: the game draws the friendly team's (the "light" models, green lights)
    // and the enemy's (red), and a hacked care package booby-trapped
    enum class Team
    {
        Both,
        Friendly,
        Enemy,
    };

    // The rows of a killstreak's effects on its Look page (S3, effects only):
    // each of the game's effects it runs is of one, and the dust a helicopter raises (its rotor wash) and the RC-XD's
    // treads are rows of their own. Every row is on, as the game has them, until the actor turns it off
    // (Types::Actor::effectsOff): nothing runs that can't be seen and turned off.
    enum class EffectRow : std::uint8_t
    {
        Rotors,
        Lights,
        Exhaust,
        RotorWash,
        Treads,
        Afterburners,
        Contrails,
    };

    // One of the game's effects a killstreak runs on a bone while it is there: its rotors (the models have no blades,
    // the effects are them), lights, exhaust and afterburners
    struct Effect
    {
        std::string_view effect;
        std::string_view tag;
        Team team = Team::Both;  // shown only with that look
        EffectRow row = EffectRow::Lights;
    };

    // The dust a helicopter raises from the ground under it, by the surface there (its vehicle definition's treadFx):
    // the effect of each of the game's surface types (see GetSurfaceName)
    using DustEffects = std::array<std::string_view, 29>;

    // The units a second of a speed in miles an hour, as the game's vehicles go (its scripts convert with 17.6)
    constexpr float FromMph(float mph)
    {
        return mph * 17.6f;
    }

    // A car's wheels and body as its model and vehicle definition have them (rc_car_medium_mp's nitrous_* values), for
    // how it steers, rolls and leans (see VehicleLogic)
    struct Wheels
    {
        std::span<const std::string_view> steer;  // the front wheels' bones the steering turns
        std::span<const std::string_view> roll;   // the bones at the wheels' middles, which roll
        float radius = 0.0f;                      // from a wheel's middle to the ground
        float front = 0.0f;                       // how far the front axle is ahead of its origin
        float back = 0.0f;                        // and the back axle behind it
        float track = 0.0f;                       // half the width between its left and right wheels
        float maxSteer = 0.0f;                    // degrees
        float grip = 0.0f;                        // its tyres' sideways grip, in g: the most it turns at
        std::string_view body;                    // the bone its body hangs from over its wheels, which leans
    };

    // A gun it aims and fires: the bones that turn it (about its up, then its barrel
    // up and down under that), its weapon's arcs from where it faces at rest (on its mount as drawn: the Huey's door
    // gun swings out with its door), how fast it sweeps them scanning, and what a shot looks like from its weapon
    // definition (the game may not have the weapon registered: a demo without the killstreak). Killstreaks play no
    // sounds (they are loud, captures don't record them, editors add their own).
    struct Gun
    {
        std::string_view yaw;
        std::string_view pitch;
        // a bone between those two its animations move, held at rest while it aims (the Huey's, which its door's open
        // state keys)
        std::string_view hold;
        float left = 0.0f;  // degrees it turns to the left, right, up and down at most
        float right = 0.0f;
        float up = 0.0f;
        float down = 0.0f;
        float scan = 0.0f;  // degrees a second it sweeps between its arcs (the Scan motion)
        std::string_view flash;        // its muzzle
        std::string_view flashEffect;  // worldFlashEffect
        std::uint32_t fireTime = 100;  // milliseconds between shots
        bool tracers = true;           // false for a missile
    };

    // What a killstreak without a gun does when it fires: what the game has it drop
    enum class FireDrop : std::uint8_t
    {
        None,
        NapalmBombs,  // bombs that fall, burst on the ground and leave it burning
        BombRun,      // an effect the game plays on the plane again and again, which drops and bursts the bombs itself
        Crate,        // the care package, which falls from behind it and stays where it lands
        Detonate,     // it goes off: its Detonate death
    };

    struct Info
    {
        Types::Killstreak killstreak;
        std::string_view name;  // the game's name for it: "Attack Helicopter"
        Types::VehicleClass vehicleClass;
        // its models: the friendly team's look first, then the enemy's (and the care package's booby-trapped one)
        std::span<const std::string_view> models;
        // what it goes at, slowest first (the moving motion's gait is the preset's): the speeds its scripts give it
        std::span<const ActorLogic::SpeedPreset> presets;
        float acceleration = 0.0f;  // units a second squared, as the game's (0: it never speeds up or slows down)
        float flyHeight = 0.0f;     // how high above the ground it flies, as on Nuketown (0 for those on the ground)
        float maxPitch = 0.0f;      // degrees its body pitches and rolls at most (its scripts' setmaxpitchroll)
        float maxRoll = 0.0f;
        // seconds its tilt follows how it moves over (either side of a tick; the definitions' tiltSpeed: about a second
        // for the helicopters)
        float tiltTime = 0.0f;
        float hoverDrift = 0.0f;    // how far a helicopter drifts about where it hovers (SetHoverParams' radius)
        Wheels wheels;              // a car's
        // how many of its models' first levels of detail the game draws only from inside it: the Gunship's first is its
        // pilot's cockpit (a dived shell without its rotor or windows, within 195 units), its outside the next
        std::uint8_t insideLods = 0;
        float passLength = 0.0f;    // a plane's pass over where it is put, as long as the game's
        std::span<const Effect> effects;
        const DustEffects* dust = nullptr;           // a helicopter's rotor wash
        std::string_view weapon;                     // what it fires (a vehicle's or turret's weapon), if anything
        std::span<const Gun> guns;                   // what its Look events aim and Fire events fire
        FireDrop fireDrop = FireDrop::None;          // what its Fire events do without guns
        std::string_view spin;                       // a bone its barrels spin on as it fires (the Sentry Gun's)
        // the game's own animations for it (mp_vehicles.atr): the Huey's doors
        std::span<const std::string_view> animations;
        std::string_view seat;  // where a camera in it sits (the bone camera's "Look through its eyes")
        // its deaths (see DEATH_MOTIONS), the game's own first; none for those that never die
        std::span<const std::string_view> deaths;
        // a helicopter's spin out: the speed it goes on at (units a second) and how fast it spins (degrees a second)
        float crashSpeed = 0.0f;
        float spinRate = 0.0f;
    };

    std::span<const Info> GetAll();
    const Info& Get(Types::Killstreak killstreak);
    // the killstreak a model is one of (any of its looks), none for others
    std::optional<Types::Killstreak> FindByModel(std::string_view model);
    // the killstreak an actor's models are: its body's
    std::optional<Types::Killstreak> FindByModels(const std::vector<std::string>& models);
    // which of a killstreak's looks a model is (an index into Info::models), 0 for one it doesn't have
    std::size_t GetLook(const Info& info, std::string_view model);
    // the name of a look: "Friendly", "Enemy", "Booby-trapped"
    std::string_view GetLookName(std::size_t look);
    // the effects it runs with a look
    std::vector<Effect> GetEffects(const Info& info, std::size_t look);

    // whether a killstreak flies (helicopters and planes: its path's points are in the air, see Types::Actor::flyHeight)
    bool Flies(Types::VehicleClass vehicleClass);

    // Where a click in the view picks a killstreak: a sphere about its body, this far above its origin (below a
    // helicopter's, which is its rotor) and as big as it is
    struct PickSphere
    {
        float up = 0.0f;
        float radius = 0.0f;
    };
    PickSphere GetPickSphere(const Info& info);

    // the rows of its effects, in the Look page's order: its effects' rows, its rotor wash and its treads
    std::vector<EffectRow> GetEffectRows(const Info& info);
    // a row as saved ("rotorwash"), named ("Rotor wash") and described
    std::string_view GetEffectRowKey(EffectRow row);
    std::string_view GetEffectRowName(EffectRow row);
    std::string_view DescribeEffectRow(EffectRow row);
    bool IsEffectRowOn(const Types::Actor& actor, EffectRow row);
    void SetEffectRowOn(Types::Actor& actor, EffectRow row, bool on);

    // The game's surface types, by the index a trace's surface flags have (bits 20-24): "grass", "asphalt"; empty
    // for one past them
    std::string_view GetSurfaceName(std::uint32_t surface);
    // Whether the game plays an effect of a killstreak's end with playfx, facing up whichever way the killstreak
    // points: the helicopters' final blast and the turrets' (a plane's goes on its tag, turned with it, and the RC-XD's
    // sideways). An effect stays in the world that way when its actor is hidden just after it (ActorLogic::EffectStays)
    bool FacesUp(std::string_view effect);
    // the dust a helicopter raises from a surface (none for one without rotor wash)
    std::string_view GetDustEffect(const Info& info, std::uint32_t surface);
    // the RC-XD's tread effects of a surface (_rcbomb.csc): "dust" (earth, grass, sand, water), "concrete" (hard
    // ground), "snow" (snow and ice); empty for others (wood, carpet...), which raise none
    std::string_view GetTreadSurface(std::uint32_t surface);

    // Damaged: the engine smoke a helicopter's damage states show, on its left engine
    bool CanBeDamaged(const Info& info);
    std::string_view GetDamageEffect(Types::ActorDamage damage);
    inline constexpr std::string_view DAMAGE_TAG = "tag_engine_left";

    // Its motions: what the clips of a killstreak's actor are of, in place of animations (the game moves them in code):
    // reserved names the game has no animation by. Each class has a still one and
    // (but turrets and crates) a moving one, which walks the path at the actor's speed as a locomotion clip does.
    inline constexpr std::string_view MOTION_IDLE = "vehicle_idle";    // a car's engine running, a turret, a crate
    inline constexpr std::string_view MOTION_DRIVE = "vehicle_drive";  // a car along its path
    inline constexpr std::string_view MOTION_HOVER = "vehicle_hover";  // a helicopter in the air where it is
    inline constexpr std::string_view MOTION_FLY = "vehicle_fly";      // a helicopter or plane along its path
    inline constexpr std::string_view MOTION_SCAN = "vehicle_scan";    // a turret sweeping between its arcs
    inline constexpr std::string_view MOTION_DROP = "vehicle_drop";    // a crate falling to the ground
    inline constexpr std::array<std::string_view, 6> MOTIONS{MOTION_IDLE,  MOTION_DRIVE, MOTION_HOVER,
                                                             MOTION_FLY,   MOTION_SCAN,  MOTION_DROP};
    bool IsMotion(std::string_view animation);
    // a class's motions, the still one first
    std::span<const std::string_view> GetMotions(Types::VehicleClass vehicleClass);
    bool FitsMotion(Types::VehicleClass vehicleClass, std::string_view motion);
    // a class's motion of a gait: its still one for Idle (a plane flies on), else its moving one (a turret scans, a
    // crate stays)
    std::string_view GetMotion(Types::VehicleClass vehicleClass, ActorLogic::Gait gait);
    // the gait a motion stands for: Run for the moving ones, Idle for the others
    ActorLogic::Gait GetMotionGait(std::string_view motion);
    bool IsMoving(std::string_view motion);
    // a motion in words, for lists ("Fly") and tooltips ("Flies along its path at the actor's speed")
    std::string_view GetMotionLabel(std::string_view motion);
    std::string_view DescribeMotion(std::string_view motion);
    // how a motion is looked up as an animation (see Components::Actors::GetAnimationLookup): a loop of a second, the
    // moving ones going as fast as a helicopter's or car's cruise in it
    ActorLogic::AnimationInfo GetMotionInfo(std::string_view motion);
    inline constexpr float MOTION_DURATION = 1.0f;  // seconds

    // Deaths (D3, the game's deaths without a crash or a wreck): a
    // killstreak's death is a clip of one of these motions, made as every death is (ActorLogic::MakeDeath: once, one
    // play long, off its path), carrying on from where it is at the speed and heading it had (VehicleLogic::
    // GetDeathMotion), with the game's effects as events of their own linked to it and gone at its end
    inline constexpr std::string_view MOTION_EXPLODE = "vehicle_explode";      // blows up where it is
    inline constexpr std::string_view MOTION_SPINOUT = "vehicle_spinout";      // a helicopter spins, then blows up
    inline constexpr std::string_view MOTION_DETONATE = "vehicle_detonate";    // the RC-XD goes off
    inline constexpr std::string_view MOTION_DESTROYED = "vehicle_destroyed";  // a turret sparks, then blows up
    inline constexpr std::array<std::string_view, 4> DEATH_MOTIONS{MOTION_EXPLODE, MOTION_SPINOUT, MOTION_DETONATE,
                                                                   MOTION_DESTROYED};
    bool IsDeathMotion(std::string_view animation);
    // the deaths of a class (what fits it), and of a killstreak (its own first)
    std::span<const std::string_view> GetDeathMotions(Types::VehicleClass vehicleClass);
    std::span<const std::string_view> GetDeaths(const Info& info);
    // how long a death plays at first (seconds): until it is gone (a spin out: the user sets it, the game 0-15 s)
    float GetDeathDuration(std::string_view motion);
    // What a death of a killstreak plays, as the game has it: its Hit (the blow: an explosion, sparks) at its start,
    // and the effects and the Hide after it, at ticks from its start or its end
    struct DeathEvent
    {
        Types::ActorEventType type = Types::ActorEventType::Effect;
        std::string_view effect;
        std::string_view tag;
        std::uint32_t at = 0;
        bool afterEnd = false;
    };
    std::vector<DeathEvent> GetDeathEvents(const Info& info, std::string_view motion, std::uint32_t length);
    // A Spin out clip's spin (degrees a second it spins up to) and speed (units a second it flies on at): its own, the
    // clip's Spin and Speed rows (Types::ActorClip::spinRate, crashSpeed), else the killstreak's (Auto); and the game's
    // ranges, which the rows mark
    float GetSpinRate(const Info& info, const Types::ActorClip& clip);
    float GetCrashSpeed(const Info& info, const Types::ActorClip& clip);
    inline constexpr float GAME_SPIN_LEAST = 45.0f, GAME_SPIN_MOST = 135.0f;
    inline constexpr float GAME_CRASH_LEAST = FromMph(30.0f), GAME_CRASH_MOST = FromMph(80.0f);
    // whether a helicopter's tail rotor is broken at a tick: from the start of a spin out on, as the game swaps its
    // running tail rotor for the damaged one's effect (an event of the death), so the running one stops
    bool HasBrokenTailRotor(const Types::Actor& actor, std::uint32_t tick);

    // how high above the ground the care package falls from as the chopper drops it (850 up, 200 under the chopper)
    inline constexpr float DROP_HEIGHT = 650.0f;

    // Fire: whether a killstreak with a look has anything to fire: its guns, or what
    // the game has it drop. A care package only goes off booby-trapped.
    bool CanFire(const Info& info, std::size_t look);
    // What a Fire of the killstreak's is, from a tick (the playhead): a second of its guns' shots, or what the game
    // drops at once. A plane's goes off where the game's does, so what it drops lands under the middle of its pass
    // (at the tick given when another Fire is there already). See FireDrop::Detonate: no event for that one.
    Types::ActorEvent MakeFire(const Info& info, const Types::Actor& actor, std::uint32_t tick);
    // the words for a Fire of the killstreak's, for the menus: "Drops its bombs"
    std::string_view DescribeFire(const Info& info);

    // A killstreak's rockets beside its gun (the Gunship's, _helicopter_player.gsc fire_rocket):
    // from its tags by turns, at what its gun aims at (traced from the
    // gun's muzzle along its barrel), two 0.3 s apart before it reloads
    struct Rockets
    {
        std::array<std::string_view, 2> tags;
        std::string_view weapon;
        std::string_view muzzle;  // the gun's, where what it aims at is traced from, along its forward
        std::uint32_t shots = 2;
        std::uint32_t interval = 300;
    };
    // its rockets, none for one without
    const Rockets* GetRockets(Types::Killstreak killstreak);
    // A Fire's Guns or Rockets row (the Gunship's): the game's shots and interval for either
    void SetRockets(const Info& info, Types::ActorEvent& fire, bool rockets);

    // The Mortar Team: not one of the killstreaks above, as it has no
    // model: an actor of its own (Types::ActorMortar) under A killstreak. Its name, and a new one: three points, the
    // clicked one in the middle and the others MORTAR_SPREAD to its sides across the way the shells come (the
    // camera's), a Fire at the tick (the first shell lands then)
    inline constexpr std::string_view MORTAR_NAME = "Mortar Team";
    inline constexpr float MORTAR_SPREAD = 400.0f;
    Types::Actor MakeMortar(glm::vec3 point, float yaw, std::uint32_t tick);
    // A Fire of the Mortar Team's from a tick: the game's two shells on each point, 1.3 s apart (each point 2.5 s after
    // the one before)
    Types::ActorEvent MakeMortarFire(std::uint32_t tick);
    // the effect the B-52 plays, again and again, as it bombs (no tag of its model is named: the game plays it on the
    // plane's origin, its script's tag_bomb1 and tag_bomb2 not being on the model)
    inline constexpr std::string_view BOMB_RUN_EFFECT = "weapon/bombing_run/fx_mp_bombing_run_bomb";

    // the preset an actor of the killstreak starts at: its cruise (the Run gait's), else its first
    float GetCruiseSpeed(const Info& info);
    // the fastest it goes: its fastest preset's speed (the Cobra evading, the RC-XD at full throttle), 0 for none
    float GetTopSpeed(const Info& info);
    // whether an actor is a killstreak that speeds up and slows down along its path (a helicopter, the RC-XD: see
    // VehicleLogic::GetProfile), whose flight is fitted to its path again as the path changes
    bool HasFlights(const Types::Actor& actor);
    // the ticks it takes to get to a speed (from standing) or to stop from it at its acceleration, its corners soft
    // (see VehicleLogic::GetRampTime), as the blend of the clip that changes it (0 for one that doesn't speed up or
    // slow down)
    std::uint32_t GetSpeedChangeTicks(const Info& info, float speed);

    // An actor of the killstreak from a tick, as the game shows it, to the end of the timeline (see ActorLogic::MakeStart
    // for people): the RC-XD drives a path and stops, a helicopter flies one and hovers, a plane makes a straight pass
    // (passLength, at its speed, facing yaw, then is gone), a turret stands (the Sentry Gun scanning), a crate drops;
    // its path a point at the origin (the plane's through it) for the UI to place. Its friendly look, Locomotion Auto at
    // its cruise, no transitions, no face.
    Types::Actor MakeStart(Types::Killstreak killstreak, std::uint32_t tick, std::uint32_t end, float yaw);
    // the tick its start passes the origin at: its start tick, a plane's halfway through its pass
    std::uint32_t GetStartPlaceTick(const Types::Actor& actor, std::uint32_t tick);
}  // namespace IWXMVM::Components::KillstreakLogic
