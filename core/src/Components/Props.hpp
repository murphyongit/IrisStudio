#pragma once
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "PropLogic.hpp"

// Props' keys: a prop's position and turn are each a property made at run
// time (Components::RuntimeProperties), filed under the prop's own id, which stays the same across saves as actors'
// don't. And the browser's preview: the model a row is hovered for, standing where the camera looks.
namespace IWXMVM::Components::Props
{
    void Initialize();

    // The property of a prop's value, made the first time it is asked for: on the render thread, as
    // KeyframeManager::AddProperty is
    const Types::KeyframeableProperty& GetProperty(std::uint32_t prop, PropLogic::Channel channel);

    // The props' keys as of the render thread's last frame, for the game's thread, by the props' own ids (those with
    // keys alone)
    using AllKeys = std::map<std::uint32_t, PropLogic::Keys>;
    std::shared_ptr<const AllKeys> GetKeys();
    // The props with keys as of that frame, those not in the scene too: a demo's keys stay in its file for a scene not
    // loaded, and a new prop mustn't take them on
    std::shared_ptr<const std::set<std::uint32_t>> GetKeyed();

    // A duplicate gets a copy of the prop's keys; those of a deleted prop go, as part of the same undo step
    void CopyKeys(std::uint32_t from, std::uint32_t to);
    void ClearKeys(std::uint32_t prop);
    // A copy of a prop's keys which outlives the prop (a bar cut, to paste), and giving another prop those keys so many
    // ticks later
    using KeyCopy = std::vector<std::pair<PropLogic::Channel, std::vector<Types::Keyframe>>>;
    KeyCopy TakeKeys(std::uint32_t prop);
    void PutKeys(std::uint32_t prop, const KeyCopy& keys, std::int64_t ticks);
    // The properties of a prop's values that have keys, which go with its bar as it moves
    std::vector<Types::KeyframeablePropertyType> GetKeyedProperties(std::uint32_t prop);

    // The actor with its prop's place as its keys have it at a timeline's tick (any thread)
    Types::Actor WithKeyedPlacement(const Types::Actor& actor, std::uint32_t tick);
    // The same from its keys as they are now (the render thread's, as a drag of its gizmo keys it)
    Types::Actor WithKeyedPlacementNow(const Types::Actor& actor, std::uint32_t tick);

    // A prop moved in the view or put somewhere: its position or turn with keys is keyed at the playhead, as a change
    // of its row is, on the render thread's next frame (TakeKeyedMoves); one without keys is set on it. placed is the
    // prop placed there. Any thread; returns whether the actor changed.
    bool MoveTo(Types::Actor& actor, const Types::Actor& placed, const std::string& step);
    struct KeyedMove
    {
        std::uint32_t actor = 0;
        std::optional<glm::vec3> position;
        std::optional<glm::vec3> turn;
        std::string step;
    };
    std::vector<KeyedMove> TakeKeyedMoves();

    // The actor whose prop a property is of; none for others
    std::optional<std::uint32_t> FindActor(Types::KeyframeablePropertyType type);
    // A prop's property as the user knows it, after the prop's name as it is now: "Chair metal 2: Turn"; none for
    // others
    std::optional<std::string> GetName(Types::KeyframeablePropertyType type);

    // The browser's preview: the model of the row hovered, standing on what the camera looks at as an added prop would,
    // while the browser asks for it each frame; none once it stops asking (the row left, the browser shut)
    struct Preview
    {
        std::string model;
        glm::vec3 origin{0.0f};
        glm::vec3 direction{1.0f, 0.0f, 0.0f};
        float yaw = 0.0f;  // facing the camera
    };
    void SetPreview(const Preview& preview);
    // (any thread: what was asked for in the last two frames of the render thread's)
    std::optional<Preview> GetPreview();
}  // namespace IWXMVM::Components::Props
