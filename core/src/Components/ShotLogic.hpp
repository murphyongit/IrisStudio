#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <glm/glm.hpp>

namespace IWXMVM::Components::ShotLogic
{
    // Where an actor's bullet lands: the game's own effect for what it hits, chosen as
    // its bullets choose it, on the world, on a demo's player or on another actor. Actors are drawn from free handles and
    // aren't entities, so the game's trace can't see them and they are tested here, as capsules along their bones. Nothing
    // is kept between shots: a shot comes from its Fire event, so a rewind plays it again the same way. No game state;
    // tested in tests/actors-tests.

    // the game's surface types where it has its own effects (bg_surfaceTypes)
    inline constexpr std::uint32_t SURFACE_FLESH = 7;
    inline constexpr std::uint32_t SURFACE_METAL = 13;

    // how far a bullet goes, and the bones of a body it can hit, with how thick a body is around them
    inline constexpr float SHOT_RANGE = 8192.0f;
    struct BodyPart
    {
        std::string_view from;
        std::string_view to;
        float radius = 0.0f;
    };
    inline constexpr std::array<BodyPart, 11> BODY_PARTS = {{
        {"j_neck", "j_head", 5.5f},
        {"j_spine4", "j_neck", 6.0f},
        {"j_mainroot", "j_spine4", 9.0f},
        {"j_shoulder_le", "j_elbow_le", 3.5f},
        {"j_elbow_le", "j_wrist_le", 3.0f},
        {"j_shoulder_ri", "j_elbow_ri", 3.5f},
        {"j_elbow_ri", "j_wrist_ri", 3.0f},
        {"j_mainroot", "j_knee_le", 5.5f},
        {"j_knee_le", "j_ankle_le", 4.0f},
        {"j_mainroot", "j_knee_ri", 5.5f},
        {"j_knee_ri", "j_ankle_ri", 4.0f},
    }};
    // nothing further from a body's middle than this, up or out: what is further away isn't asked for its bones
    inline constexpr float BODY_REACH = 64.0f;

    // How much of a line from a point along a unit direction lies before it meets a capsule: the distance, none if it
    // misses or the capsule is behind it. A start inside the capsule counts as a miss: a gun inside a body doesn't hit it.
    std::optional<float> HitCapsule(const glm::vec3& start, const glm::vec3& direction, float length, const glm::vec3& a,
                                    const glm::vec3& b, float radius);

    // a capsule of a body as it stands now
    struct Capsule
    {
        glm::vec3 a{0.0f};
        glm::vec3 b{0.0f};
        float radius = 0.0f;
        std::size_t part = 0;  // of BODY_PARTS
    };
    struct BodyHit
    {
        float distance = 0.0f;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};  // out of the body, at the point
        std::size_t part = 0;
    };
    // the first of a body's capsules the line meets within a length
    std::optional<BodyHit> FindBodyHit(const glm::vec3& start, const glm::vec3& direction, float length,
                                       std::span<const Capsule> capsules);

    // whether a line can come near a body around a point (BODY_REACH): so a shot asks few bodies for their bones
    bool PassesNear(const glm::vec3& start, const glm::vec3& direction, float length, const glm::vec3& point, float reach);

    // Axes for an effect on a surface, as the game's bullets make them (0x48C290): x along the normal, the other two
    // turned about it. The game turns them at random; here the turn comes from the shot's tick, so a rewind gives the
    // same decal.
    struct Axes
    {
        glm::vec3 forward{1.0f, 0.0f, 0.0f};
        glm::vec3 left{0.0f, 1.0f, 0.0f};
        glm::vec3 up{0.0f, 0.0f, 1.0f};
    };
    Axes GetImpactAxes(const glm::vec3& normal, float turnDegrees);
    float GetImpactTurn(std::uint32_t tick);

    // How a shot leaves the barrel: a player aiming a rifle has no spread, and an actor's gun doesn't move as a player's
    // kicks, so every bullet of a burst would land in one spot, a dark blot of a dozen marks. So each goes a little off the
    // barrel, by its tick, the same at every play. A shotgun's pellets spread as the gun's file has it when aimed, the
    // Death Machine's three a shot as a rifle's. A Fire event can say otherwise (ActorEvent::spread).
    inline constexpr float GUN_SCATTER = 0.6f;
    inline constexpr float MOST_SPREAD = 6.0f;  // (the slider's end: the game's widest shotguns have 5.5)
    inline constexpr std::uint32_t MOST_PELLETS = 16;
    struct Spread
    {
        std::uint32_t pellets = 1;
        float degrees = GUN_SCATTER;
    };
    Spread GetSpread(std::int32_t shotCount, float aimedSpread, float chosen = -1.0f);
    // a pellet's direction, within that many degrees of the axes' forward (a unit vector)
    glm::vec3 GetPelletDirection(const Axes& barrel, std::uint32_t tick, std::uint32_t pellet, float degrees);

    // A launcher's rocket, from the muzzle in a straight line to what the barrel points at, as fast as its weapon file
    // says. It comes from its shot: a rewind leaves it behind, and the Fire event launches it again as the timeline plays
    // past it. The timeline's speed and Freeze time move it as they move actors, by its tick.
    struct RocketFlight
    {
        std::uint32_t tick = 0;  // of the shot
        glm::vec3 start{0.0f};
        glm::vec3 direction{1.0f, 0.0f, 0.0f};
        float speed = 0.0f;     // units a second
        float distance = 0.0f;  // to where it lands
    };
    // where it is at a tick, none before its shot or once it got there
    std::optional<glm::vec3> GetRocketPoint(const RocketFlight& flight, std::uint32_t tick);
    bool HasLanded(const RocketFlight& flight, std::uint32_t tick);
    // A missile that homes on what it aims at (the SAM Turret's): from its
    // start it flies as fast as its flight says along the line to where its target is at each tick, and is there once
    // it has flown that far, so the same tick and target give the same point. None before its shot or once it got
    // there.
    std::optional<glm::vec3> GetHomingPoint(const RocketFlight& flight, glm::vec3 target, std::uint32_t tick);
    bool HasHomed(const RocketFlight& flight, glm::vec3 target, std::uint32_t tick);

    // the most effects in use (of the game's 1024) at which a shot still plays its hit: past it the shot's flash and
    // sound go on, and the hit is skipped, so that a long burst can't take the room of what the user added
    inline constexpr std::size_t MOST_EFFECTS_FOR_HITS = 700;
    inline bool HasRoomForHit(std::size_t effectsInUse)
    {
        return effectsInUse < MOST_EFFECTS_FOR_HITS;
    }

    // whether the game's entity type is flesh to a bullet with no surface of its own (a player, a corpse, an AI: from
    // what 0x7FBE60 does with such an entity)
    bool IsFleshEntity(std::int32_t entityType);
}  // namespace IWXMVM::Components::ShotLogic
