#pragma once
#include <deque>

#include "Types/BoneCamera.hpp"
#include "Types/Keyframe.hpp"

// What the bone camera does that doesn't need the game: where it is for a bone, how its sway is steadied, the bones
// it offers first, and its settings' edits for the undo history
namespace IWXMVM::Components::BoneCameraLogic
{
    // A bone or a target in the world: where it is and its axes (the columns: forward, left, up), as the game's tag
    // matrices have them
    struct Transform
    {
        glm::vec3 position{0.0f};
        glm::mat3 axes{1.0f};
    };

    // The game's angles (pitch, down positive; yaw; roll, in degrees) as axes, and back
    glm::mat3 AnglesToAxes(const glm::vec3& angles);
    glm::vec3 AxesToAngles(const glm::mat3& axes);

    // Where a target stands and the way it faces, upright
    Transform MakeFrame(const glm::vec3& origin, float yaw);

    // Where the camera is and its angles, for a bone (in the world) of a target whose frame is given
    struct Placement
    {
        glm::vec3 position{0.0f};
        glm::vec3 angles{0.0f};
    };
    Placement Place(const Transform& bone, const Transform& frame, const Types::BoneCameraSettings& settings);

    // The offsets that put the camera at a position and angles for that bone and frame (flying in the bone camera
    // moves the camera, and its offsets follow); the settings' other fields say how it turns
    std::pair<glm::vec3, glm::vec3> GetOffsets(const Transform& bone, const Transform& frame,
                                               const Types::BoneCameraSettings& settings, const glm::vec3& position,
                                               const glm::vec3& angles);

    // A tick further than this after the one before is a jump (a seek), which Steady and Glide start again from
    inline constexpr std::uint32_t MAX_STEADY_STEP = 250;

    // Steadies a bone's sway: the bone is taken in its target's frame (relative to where it stands and faces), followed
    // there over the world's ticks with a time constant, and put back on the target where it is now, so the shake goes
    // but the camera doesn't fall behind a target that moves. It starts from the bone as it is after a jump, a tick
    // before the last, another bone or target, or Reset (a recording starting).
    class Steady
    {
       public:
        struct Key
        {
            Types::BoneCameraTarget target;
            std::string bone;
            bool operator==(const Key&) const = default;
        };

        Transform Step(const Transform& bone, const Transform& frame, std::uint32_t tick, float seconds,
                       const Key& key);
        void Reset();

       private:
        std::optional<Key> key;
        std::uint32_t tick = 0;
        glm::vec3 position{0.0f};  // in the target's frame
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    // Where the view is, its FOV with it
    struct View
    {
        glm::vec3 position{0.0f};
        glm::vec3 angles{0.0f};
        float fov = 0.0f;
    };

    // Takes the view through the frames the game's clock stands still. The clock is whole milliseconds, so at a speed
    // under the frame rate / 1000 it moves on some frames only, and a bone with it, where a free camera moves on all of
    // them. There the view is the exact view averaged twice over the real time of a
    // millisecond of the clock (as its steps take it), which turns its stairs into a slope, whatever frame the
    // bone takes each step on, and keeps it moving through a step longer than the others: the view is a step and a half
    // of the bone behind at most. While the clock moves every frame it stays exact, on the bone as drawn. So it does
    // after a jump (see MAX_STEADY_STEP), a cut, another bone or target, or Reset (a recording, whose frames all move),
    // and once the clock has stood two of its milliseconds (paused) anything that moves the view does so at once.
    class Glide
    {
       public:
        // A frame: the exact view, the timeline's tick, the time in ms and the speed
        View Step(const View& exact, std::uint32_t tick, double now, float speed, const Steady::Key& key);
        void Reset();

       private:
        // a frame's exact view, which stands for the time since the frame before
        struct Sample
        {
            double time = 0.0;
            glm::vec3 position{0.0f};
            glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
            float fov = 0.0f;
        };

        std::optional<Steady::Key> key;
        std::uint32_t tick = 0;
        double tickTime = 0.0;  // when the clock last moved
        double lastFrame = 0.0;
        double frameLength = 0.0;  // a frame's ms, averaged
        double stepLength = 0.0;   // a millisecond of the clock in real time, averaged over its steps
        bool gliding = false;
        std::deque<Sample> samples;
        std::deque<Sample> averaged;  // the first average's views, which the second averages
    };

    // The view a frame's picture is drawn from. The game adds and poses a frame's models
    // (CG_AddPacketEntities) before it builds the view (CG_CalcViewValues), and hands the two to the render thread
    // together, so the camera placed from the bones as they were posed is the next view built's, and that one's only.
    // Views are counted as they are built.
    class HandOver
    {
       public:
        // The camera placed once the frame's models were posed, with the views built so far
        void Place(const View& view, std::uint32_t views);
        // The view to build, the views built so far counting it: the camera placed since the view before, none when it
        // wasn't (no models were posed, the camera was just chosen, a load)
        std::optional<View> GetView(std::uint32_t views) const;
        void Reset();

       private:
        std::optional<View> placed;
        std::uint32_t placedAt = 0;
    };

    // How the pictures drawn followed the bone (the harness's check of the hand-over): each view built against the
    // camera placed from the bones posed for it, and the views drawn against the views built, in order
    struct ViewRecord
    {
        std::uint32_t views = 0;  // the views built so far
        glm::vec3 position{0.0f};
    };
    struct DrawnCheck
    {
        int built = 0;
        int offBone = 0;  // built from other bones than those of the models drawn with it, or from none
        int stood = 0;    // built as the view before while the bones had moved: the picture stood, the models went on
        int drawn = 0;
        int unknown = 0;       // drawn from a view not built in the window
        int repeated = 0;      // drawn from the view of the picture before: the camera stood while the models moved
        int skipped = 0;       // built and never drawn
        float largest = 0.0f;  // the furthest a picture was from the view it was drawn from
    };
    DrawnCheck CheckDrawn(std::span<const ViewRecord> placed, std::span<const ViewRecord> built,
                          std::span<const glm::vec3> drawn, float tolerance);

    // The bones offered first when a target has them, by what they are
    struct NamedBone
    {
        std::string_view bone;
        std::string_view label;
    };
    std::span<const NamedBone> GetNamedBones();
    // "Eyes" for tag_eye, the bone's own name for one without a label
    std::string DescribeBone(std::string_view bone);
    // A target's bones as the bone combo lists them: the named ones it has, in GetNamedBones' order, then the others
    // in the target's order; how many named ones lead
    std::pair<std::vector<std::string>, std::size_t> OrderBones(std::span<const std::string> bones);

    // What the undo history calls a change of the settings ("follow Actor 2", "move the bone camera"), given the name
    // of the target after it
    std::string DescribeChange(const Types::BoneCameraSettings& before, const Types::BoneCameraSettings& after,
                               const std::string& targetName);

    // The keys of the offset, rotation and FOV: copies of their properties' keys, none
    // when a field isn't keyed. The target, bone, Steady and how it turns aren't keyed.
    struct Keys
    {
        std::vector<Types::Keyframe> offset;
        std::vector<Types::Keyframe> rotation;
        std::vector<Types::Keyframe> fov;
    };
    // the properties of Keys, in the Camera tab's order
    inline constexpr std::array<Types::KeyframeablePropertyType, 3> KEYED_PROPERTIES = {
        Types::KeyframeablePropertyType::BoneCameraOffset, Types::KeyframeablePropertyType::BoneCameraRotation,
        Types::KeyframeablePropertyType::BoneCameraFov};
    bool IsKeyedProperty(Types::KeyframeablePropertyType type);

    // which fields keys set
    struct Keyed
    {
        bool offset = false;
        bool rotation = false;
        bool fov = false;

        bool Any() const
        {
            return offset || rotation || fov;
        }
        bool operator==(const Keyed&) const = default;
    };
    Keyed GetKeyed(const Keys& keys);

    // The settings with the keyed fields as their keys have them at a timeline tick
    Types::BoneCameraSettings ApplyKeys(Types::BoneCameraSettings settings, const Keys& keys, double tick);
    // The settings with the keyed fields taken from others, the rest their own
    Types::BoneCameraSettings WithKeyed(Types::BoneCameraSettings settings, const Types::BoneCameraSettings& from,
                                        Keyed keyed);
    // A field's value as a key of its property holds it, and the settings with a value of it; nothing and false for
    // a property that isn't one of KEYED_PROPERTIES
    std::optional<Types::KeyframeValue> GetValue(const Types::BoneCameraSettings& settings,
                                                 Types::KeyframeablePropertyType type);
    bool SetValue(Types::BoneCameraSettings& settings, Types::KeyframeablePropertyType type,
                  const Types::KeyframeValue& value);

    // An edit of the settings: from the frame a control first changes them until no control is active (a drag,
    // typing, a pick), as one step. Keyed fields are left out: their keys change them every frame, and a change of
    // them is a change of their keys (a step of its own).
    class Edit
    {
       public:
        // A frame: the settings as they were when it began and as it left them. Returns whether the edit began now.
        bool Track(const Types::BoneCameraSettings& frameStart, const Types::BoneCameraSettings& now,
                   Keyed keyed = {});
        bool IsOpen() const
        {
            return before.has_value();
        }
        // Ends the edit: the settings before it (with the keyed fields as they are now) and now, nothing when they are
        // the same again
        std::optional<std::pair<Types::BoneCameraSettings, Types::BoneCameraSettings>> End(
            const Types::BoneCameraSettings& now, Keyed keyed = {});

       private:
        std::optional<Types::BoneCameraSettings> before;
    };
}  // namespace IWXMVM::Components::BoneCameraLogic
