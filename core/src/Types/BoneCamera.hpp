#pragma once

#include <glm/glm.hpp>

namespace IWXMVM::Types
{
    // What the bone camera follows: a player of the demo (by its entity number, through its corpse once it died) or
    // an actor (by Actor::id, which stays the same through renames and other actors coming and going)
    enum class BoneCameraTargetKind
    {
        Player,
        Actor,
    };

    struct BoneCameraTarget
    {
        BoneCameraTargetKind kind = BoneCameraTargetKind::Player;
        std::int32_t id = 0;

        bool operator==(const BoneCameraTarget&) const = default;
    };

    // How the bone camera turns: with the bone, with the bone but keeping the horizon level (no roll), or only as the
    // target turns where it faces (its yaw), never with the bone's own sway
    enum class BoneCameraTurn
    {
        Bone,
        Level,
        Facing,
    };

    // A bone of a target in the world (its axes as columns: forward, left, up), with where the target stands and the
    // way it faces
    struct TargetBone
    {
        glm::vec3 position{0.0f};
        glm::mat3 axes{1.0f};
        glm::vec3 origin{0.0f};
        float yaw = 0.0f;
    };

    // The bone camera's settings, saved with the demo's keyframes
    struct BoneCameraSettings
    {
        BoneCameraTarget target;
        std::string bone = "tag_eye";
        glm::vec3 positionOffset{0.0f};  // in the bone's space: x along where it faces, y to its left, z up
        glm::vec3 rotationOffset{0.0f};  // pitch, yaw and roll from the bone's axes, in degrees
        float fov = 90.0f;
        float steady = 0.0f;  // how long the bone's sway is smoothed over, in seconds (0 for none)
        BoneCameraTurn turn = BoneCameraTurn::Bone;
        bool showBone = true;

        bool operator==(const BoneCameraSettings&) const = default;
    };
}  // namespace IWXMVM::Types
