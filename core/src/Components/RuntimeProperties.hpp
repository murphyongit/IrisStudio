#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json_fwd.hpp"

#include "Types/KeyframeableProperty.hpp"

// Keyframeable properties made at run time: ReShade's values and effects, which are
// only known once ReShade loaded them. Each gets a type of its own, ReShade and up (Types::KeyframeablePropertyType),
// so the timeline, the Look graph and the selection, which tell properties apart by their type, take them as they take
// the others. A property is registered once per id and stays for the mod's run: its keys are filed under its name
// (KeyframeManager), which doesn't change, and its type stays the same for that run.
namespace IWXMVM::Components::RuntimeProperties
{
    // How a property's keys step (KeyframeLogic::IsStepped): not at all (eased between keys), a switch held on (1) or
    // off (0), or a whole number held until the next key (one of a list's items)
    enum class Steps
    {
        None,
        Switch,
        Whole,
    };

    struct Description
    {
        std::string id;    // its source's own: "qUINT_ssr.fx/SSR_REFLECTION_INTENSITY", "qUINT_ssr.fx/technique:SSR"
        std::string name;  // as the timeline and the graph name it: "ReShade: SSR > Reflection Intensity"
        Types::KeyframeValueType valueType = Types::KeyframeValueType::FloatingPoint;
        float min = 0.0f;  // the graph's range for it
        float max = 1.0f;
        Steps steps = Steps::None;
        std::array<std::string, 3> valueNames{};  // a Vector3's ("R", "G", "B")
    };

    // The property of the id: the one registered before, else a new one. Its name is unique among them (another's
    // name gets the id after it) and stays as first registered.
    Types::KeyframeablePropertyType Register(const Description& description);

    bool IsRuntime(Types::KeyframeablePropertyType type);
    // Which of them is a light's (its id starts "light/", Components::LightLogic), on its own track, not ReShade's in the
    // Look tab
    bool IsLight(Types::KeyframeablePropertyType type);
    // Which is a prop's (its id starts "prop/", Components::PropLogic), on its own track as a light's is
    bool IsProp(Types::KeyframeablePropertyType type);
    // Either: a property of an actor of the scene, kept out of the Look tab
    bool IsActorProperty(Types::KeyframeablePropertyType type);
    const Description* Find(Types::KeyframeablePropertyType type);
    std::optional<Types::KeyframeablePropertyType> FindById(std::string_view id);
    // all of them, in the order they were registered
    std::vector<Types::KeyframeablePropertyType> GetAll();

    // the property, with its name and value names pointing at the registry's (which stay)
    Types::KeyframeableProperty MakeProperty(Types::KeyframeablePropertyType type);

    // What a keyframe file keeps of one (KeyframeSerializer), with its id, so it can be made again while its source
    // doesn't have it (an effect not loaded) and its keys stay in the file: "id", "name", "type", "range", "steps",
    // "valueNames"; nothing read from a node without an id, or of a value type it can't have
    void Write(const Description& description, nlohmann::json& node);
    std::optional<Description> Read(const nlohmann::json& node);
}  // namespace IWXMVM::Components::RuntimeProperties
