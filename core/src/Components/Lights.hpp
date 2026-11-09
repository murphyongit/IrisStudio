#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "LightLogic.hpp"

// Lights' keys: each value of a light that can have keys is a property made at run time
// (Components::RuntimeProperties), filed under the light's own id, which stays the same across saves as actors' don't.
namespace IWXMVM::Components::Lights
{
    void Initialize();

    // The property of a light's value, made the first time it is asked for: on the render thread, as
    // KeyframeManager::AddProperty is
    const Types::KeyframeableProperty& GetProperty(std::uint32_t light, LightLogic::Channel channel);

    // The lights' keys as of the render thread's last frame, for the game's thread, by the lights' own ids (those with
    // keys alone)
    using AllKeys = std::map<std::uint32_t, LightLogic::Keys>;
    std::shared_ptr<const AllKeys> GetKeys();
    // The lights with keys as of that frame, those not in the scene too: a demo's keys stay in its file for a scene not
    // loaded, and a new light mustn't take them on
    std::shared_ptr<const std::set<std::uint32_t>> GetKeyed();

    // A duplicate gets a copy of the light's keys; those of a deleted light go, as part of the same undo step
    void CopyKeys(std::uint32_t from, std::uint32_t to);
    void ClearKeys(std::uint32_t light);
    // A copy of a light's keys, value by value, which outlives the light (a bar cut, to paste), and giving another light
    // those keys so many ticks later
    using KeyCopy = std::vector<std::pair<LightLogic::Channel, std::vector<Types::Keyframe>>>;
    KeyCopy TakeKeys(std::uint32_t light);
    void PutKeys(std::uint32_t light, const KeyCopy& keys, std::int64_t ticks);
    // The properties of a light's values that have keys, which go with its bar as it moves
    std::vector<Types::KeyframeablePropertyType> GetKeyedProperties(std::uint32_t light);

    // The actor with its light's place as its keys have it at a timeline's tick (any thread)
    Types::Actor WithKeyedPlacement(const Types::Actor& actor, std::uint32_t tick);
    // The same from its keys as they are now (the render thread's, as a drag of its gizmo keys it)
    Types::Actor WithKeyedPlacementNow(const Types::Actor& actor, std::uint32_t tick);

    // A light moved in the view or put somewhere: its position or aim with
    // keys is keyed at the playhead, as a change of its row is, on the render thread's next frame (TakeKeyedMoves); one
    // without keys is set on it. placed is the light placed there. Any thread; returns whether the actor changed.
    bool MoveTo(Types::Actor& actor, const Types::Actor& placed, const std::string& step);
    struct KeyedMove
    {
        std::uint32_t actor = 0;
        std::optional<glm::vec3> position;
        std::optional<glm::vec3> aim;
        std::string step;
    };
    std::vector<KeyedMove> TakeKeyedMoves();

    // The actor whose light a property is of; none for others
    std::optional<std::uint32_t> FindActor(Types::KeyframeablePropertyType type);
    // A light's property as the user knows it, after the light's name as it is now: "Spot 2: Cone"; none for others
    std::optional<std::string> GetName(Types::KeyframeablePropertyType type);
}  // namespace IWXMVM::Components::Lights
