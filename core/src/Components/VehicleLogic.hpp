#pragma once
#include "Types/Actor.hpp"
#include "Components/KillstreakLogic.hpp"

#include <functional>
#include <span>

namespace IWXMVM::Components::VehicleLogic
{
    // How a killstreak's body moves as it goes: what the game's vehicle code does to
    // it, worked out from where the actor is around a tick, so scrubbing, rewinds and captures give the same. No game
    // state; tested in tests/actors-tests.

    inline constexpr float GRAVITY = 800.0f;  // units a second squared (g_gravity)
    // a helicopter going slower than this hovers: drifts about where it is
    inline constexpr float HOVER_SPEED = 150.0f;

    struct Body
    {
        // the whole vehicle's tilt, as the game's angles: nose down and right side down (degrees)
        float pitch = 0.0f;
        float roll = 0.0f;
        glm::vec3 drift{0.0f};  // a hovering helicopter's, from where it is (in the world)
        // a car's: its body's lean over its wheels (the game's angles), how far its wheels have turned (degrees, rolling
        // forward) and how far its front wheels steer (degrees, to the left)
        float bodyPitch = 0.0f;
        float bodyRoll = 0.0f;
        float wheelTurn = 0.0f;
        float steer = 0.0f;
        // how it moves then, as measured for its tilt: its speed the way it faces (back: below 0), and its
        // acceleration ahead and to its left (units a second, a second squared)
        float speed = 0.0f;
        float ahead = 0.0f;
        float aside = 0.0f;
    };

    // Flights. A killstreak that speeds up and slows down (a helicopter, the
    // RC-XD) goes along its path at a speed of its own, not as the sum of its clips' blends: each of its clips changes
    // the speed it has as it starts to its own (its motion's speed, none for one that stays) over its blend in, along
    // a soft S (the acceleration rising over RAMP_CORNER at the change's start and falling over it at its end). A
    // flight or drive so sets off from rest over its Fly's (Drive's) blend in and stops over the Hover's (Idle's) after
    // it; a Fly here while it still goes carries its speed on. Its last flight gets to its path's end: its last moving
    // clip goes as fast as gets it there in its clips' time (a short hop peaks lower).
    inline constexpr float RAMP_CORNER = 0.3f;  // seconds

    // the seconds a change of speed takes at an acceleration, its corners soft (the acceleration reaching it over
    // RAMP_CORNER at each end, or a change too small for that as fast as it gets)
    float GetRampTime(float from, float to, float acceleration);
    std::uint32_t GetRampTicks(float from, float to, float acceleration);
    // a ramp of a time: the share of its change made a time into it (0 to 1), and the integral of that share from its
    // start (seconds of the whole change: half its time at its end)
    float GetRampShare(float seconds, float time);
    float GetRampArea(float seconds, float time);

    // A vehicle's speed along its path over time, piece by piece: each from a clip's start to the next one's (or its
    // own end, after which it stands until the next starts)
    struct Profile
    {
        struct Piece
        {
            std::uint32_t start = 0;
            std::uint32_t end = UINT32_MAX;
            std::uint32_t ramp = 0;  // ticks it takes from the speed it starts at to its own
            float from = 0.0f;       // units a second, as it starts
            float to = 0.0f;         // its own
            float distance = 0.0f;   // along the path by its start
        };
        std::vector<Piece> pieces;

        // how far along its path it has gone by a tick, and how fast it goes then
        float Distance(std::uint32_t tick) const;
        float Speed(std::uint32_t tick) const;
    };
    // One of its clips as its flights go: when it starts and stops showing, its blend in, and how fast its motion goes
    // (none for one that stays where it is)
    struct Leg
    {
        std::uint32_t start = 0;
        std::uint32_t end = UINT32_MAX;
        std::uint32_t blend = 0;
        std::optional<float> speed;
        bool dies = false;  // a death: the flight before it is cut short there, not brought to rest
    };
    // Its profile through its clips (in order), its last flight that comes to rest (a still clip's blend over before
    // the next clip starts, and no clip after it moving) stopping at the end given (its path's): its last moving clip
    // goes as fast as gets it there in its clips' time, up to the fastest it goes (KillstreakLogic::GetTopSpeed: clips
    // too short for the way leave it short of the end, never flying at speeds the game's never do). The flights before
    // go as fast as their clips say (see ActorLogic::WalkTo, which keeps the speed a flight got to as a new one starts).
    Profile GetProfile(std::span<const Leg> legs, float end, float fastest = std::numeric_limits<float>::max());

    // How a flight's clips go to get it a distance from a speed it has: speeding up to the speed it goes at (or as
    // fast as it gets on a short way, a triangle), holding it, slowing to rest, at its acceleration. Ticks: its moving
    // clip's blend in (the speed-up) and length, and the blend in of the clip after it (the slow-down).
    struct FlightFit
    {
        std::uint32_t speedUp = 0;
        std::uint32_t length = 0;
        std::uint32_t slowDown = 0;
        float peak = 0.0f;  // the speed it gets to
    };
    FlightFit FitFlight(float distance, float from, float speed, float acceleration);

    // where the actor is at a tick (Components::Actors::EvaluatePlacement)
    using PlacementAt = std::function<Types::ActorPlacement(std::uint32_t tick)>;

    // Its body at a tick, from its velocity and acceleration then, each averaged over its tilt time either side, with
    // a bell-shaped (Hann) window, so a change of speed swings it smoothly:
    // - a helicopter's nose goes down with its speed and as it speeds up (up as it slows down), it banks into turns
    //   and as it goes sideways, within its most pitch and roll; slower than HOVER_SPEED it drifts about where it is
    //   (within its hover drift, slowly, rising and sinking about 3/4 of it) and tilts with that, rocking with its drift
    //   to its side as the game's do
    // - a plane banks into turns and pitches up as it climbs
    // - a car's body leans out of turns and squats as it speeds up (dips as it slows down), within its most pitch and
    //   roll; its front wheels steer as it turns and its wheels roll the distance it has gone (along its path); how it
    //   lies on the ground is the game's to say (T5 traces it under its wheels)
    // seed: the actor's id, so two hover out of step
    Body GetBody(const KillstreakLogic::Info& info, std::uint32_t tick, const PlacementAt& placementAt, float distance,
                 std::uint32_t seed);

    // A rotation the game's controllers give a bone in place of its own, relative to its parent (the game's angles:
    // pitch, yaw, roll)
    struct Joint
    {
        std::string_view bone;
        glm::vec3 angles{0.0f};
    };
    // the bones its body turns: a car's body, steering and wheels
    std::vector<Joint> GetJoints(const KillstreakLogic::Info& info, const Body& body);

    // The radius of the corners of its path at a speed: the turn a helicopter or plane makes banked as far as it banks
    // (v^2 / (g tan roll)), a car as tight as its steering lets it and its tyres hold it
    float GetTurnRadius(const KillstreakLogic::Info& info, float speed);

    // Rotor wash (the game's aircraft_dustkick): a helicopter within DUST_HIGHEST of
    // the ground raises dust there, every DUST_STEP of actor time near it, up to every three at its highest, none
    // higher; on the timeline's ticks (multiples of DUST_STEP), so a scrub or a recording plays the same dust
    inline constexpr float DUST_LOWEST = 350.0f;
    inline constexpr float DUST_HIGHEST = 1200.0f;
    inline constexpr std::uint32_t DUST_STEP = 50;
    // whether dust plays at a tick (a multiple of DUST_STEP) at a height above the ground: one of every so many steps
    bool IsDustTick(std::uint32_t tick, float height);

    // Treads (the game's _rcbomb.csc): every TREAD_STEP of actor time the RC-XD drives, the tread effects of its speed
    // then, by the ground under its back wheels: driving at a quarter of its top speed or more, slower but moving,
    // backwards; a trail at three quarters or more, sliding (cornering as hard as its tyres hold) or peeling out
    // (speeding up as hard as it can from below half its speed), with a drift or a peel
    inline constexpr std::uint32_t TREAD_STEP = 100;
    enum class Tread
    {
        Driving,
        Slow,
        Reverse,
        Trail,
        Drift,
        Peel,
    };
    std::vector<Tread> GetTreads(const KillstreakLogic::Info& info, const Body& body);
    // the effect of a tread on a surface's treads ("dust", "concrete", "snow": KillstreakLogic::GetTreadSurface)
    std::string GetTreadEffect(std::string_view surface, Tread tread);

    // How a killstreak goes on as it dies, from the velocity it had as its death
    // started, a time into it: on at that, a helicopter spinning out going to its crash speed at its acceleration (its
    // climb or fall stopping over a second) as it spins up to its spin rate over SPIN_OUT_TIME; how far it has gone and
    // turned (degrees, to the left) by then
    inline constexpr float SPIN_OUT_TIME = 3.0f;  // seconds
    struct DeathMotion
    {
        glm::vec3 offset{0.0f};
        float yaw = 0.0f;
    };
    DeathMotion GetDeathMotion(const KillstreakLogic::Info& info, std::string_view motion, glm::vec3 velocity,
                               float seconds);

    // Its guns. Where a gun points from where it faces at rest: its yaw (degrees to
    // the left) and pitch (the game's: down)
    struct Aim
    {
        float yaw = 0.0f;
        float pitch = 0.0f;
    };
    // a gun turned toward a direction in its own axes at rest (x where it faces, y its left, z its up), as far as its
    // arcs let it
    Aim AimAt(const KillstreakLogic::Gun& gun, glm::vec3 direction);
    // a gun scanning, seconds into its Scan clip: from where it faces to its left arc, across to its right and back at
    // its scan speed, level (one whose arcs meet behind it goes round to the left)
    Aim GetScan(const KillstreakLogic::Gun& gun, float seconds);
    // a share of the way from one aim to another (the yaw the short way round)
    Aim Blend(const Aim& from, const Aim& to, float share);
    // a gun's aim at a tick without a Look event: scanning while one of the actor's clips is its Scan motion, else none
    // (its bones are left to its animations: the Huey's door gun where its door's animation puts it)
    std::optional<Aim> GetIdleAim(const KillstreakLogic::Gun& gun, const Types::Actor& actor, std::uint32_t tick);

    // How far a gun's barrels have spun by a tick (degrees, rolling): spinning up over SPIN_UP from each Fire event's
    // first shot to SPIN_RATE, and down as long after its last (its shots end a shot's time after it)
    inline constexpr float SPIN_RATE = 1080.0f;  // degrees a second
    inline constexpr float SPIN_UP = 1.0f;       // seconds (auto_gun_turret_mp's spin-up)
    float GetSpin(std::span<const Types::ActorEvent> events, std::uint32_t tick, std::uint32_t fireTime);

    // How high above where it lands the care package is at a tick: falling from DROP_HEIGHT under gravity through its
    // Drop clips, on the ground after (0 for other actors); and how long its fall takes
    float GetDropHeight(const Types::Actor& actor, std::uint32_t tick);
    std::uint32_t GetDropTicks();
}  // namespace IWXMVM::Components::VehicleLogic
