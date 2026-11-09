#include "StdInclude.hpp"
#include "RuntimeProperties.hpp"

#include <deque>
#include <mutex>

#include "nlohmann/json.hpp"

namespace IWXMVM::Components::RuntimeProperties
{
    namespace
    {
        // (a deque: what properties point at stays where it is as more are registered)
        std::mutex mutex;
        std::deque<Description> registered;

        Types::KeyframeablePropertyType TypeAt(std::size_t index)
        {
            return static_cast<Types::KeyframeablePropertyType>(
                static_cast<std::uint32_t>(Types::KeyframeablePropertyType::ReShade) + index);
        }

        std::optional<std::size_t> IndexOf(Types::KeyframeablePropertyType type)
        {
            if (!IsRuntime(type))
                return std::nullopt;
            return static_cast<std::uint32_t>(type) - static_cast<std::uint32_t>(Types::KeyframeablePropertyType::ReShade);
        }
    }  // namespace

    Types::KeyframeablePropertyType Register(const Description& description)
    {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < registered.size(); ++i)
        {
            if (registered[i].id == description.id)
                return TypeAt(i);
        }
        auto added = description;
        if (std::ranges::any_of(registered, [&](const Description& each) { return each.name == added.name; }))
            added.name += " (" + added.id + ")";
        registered.push_back(std::move(added));
        return TypeAt(registered.size() - 1);
    }

    bool IsRuntime(Types::KeyframeablePropertyType type)
    {
        return static_cast<std::uint32_t>(type) >= static_cast<std::uint32_t>(Types::KeyframeablePropertyType::ReShade);
    }

    bool IsLight(Types::KeyframeablePropertyType type)
    {
        const auto description = Find(type);
        return description && description->id.starts_with("light/");
    }

    bool IsProp(Types::KeyframeablePropertyType type)
    {
        const auto description = Find(type);
        return description && description->id.starts_with("prop/");
    }

    bool IsActorProperty(Types::KeyframeablePropertyType type)
    {
        return IsLight(type) || IsProp(type);
    }

    const Description* Find(Types::KeyframeablePropertyType type)
    {
        const auto index = IndexOf(type);
        std::lock_guard lock(mutex);
        return index && *index < registered.size() ? &registered[*index] : nullptr;
    }

    std::optional<Types::KeyframeablePropertyType> FindById(std::string_view id)
    {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < registered.size(); ++i)
        {
            if (registered[i].id == id)
                return TypeAt(i);
        }
        return std::nullopt;
    }

    std::vector<Types::KeyframeablePropertyType> GetAll()
    {
        std::lock_guard lock(mutex);
        std::vector<Types::KeyframeablePropertyType> all;
        for (std::size_t i = 0; i < registered.size(); ++i)
            all.push_back(TypeAt(i));
        return all;
    }

    Types::KeyframeableProperty MakeProperty(Types::KeyframeablePropertyType type)
    {
        const auto description = Find(type);
        if (!description)
            throw std::invalid_argument("Not a registered run-time property");
        return Types::KeyframeableProperty(
            type, description->name, description->valueType, description->min, description->max,
            {description->valueNames[0], description->valueNames[1], description->valueNames[2]});
    }

    namespace
    {
        // (as magic_enum names them in the keys' values, which the tests' build doesn't have)
        constexpr std::array<std::pair<Types::KeyframeValueType, std::string_view>, 2> VALUE_TYPES = {
            std::pair(Types::KeyframeValueType::FloatingPoint, "FloatingPoint"),
            std::pair(Types::KeyframeValueType::Vector3, "Vector3")};
        constexpr std::array<std::pair<Steps, std::string_view>, 3> STEPS = {
            std::pair(Steps::None, "None"), std::pair(Steps::Switch, "Switch"), std::pair(Steps::Whole, "Whole")};
    }  // namespace

    void Write(const Description& description, nlohmann::json& node)
    {
        node["id"] = description.id;
        node["name"] = description.name;
        for (const auto& [type, name] : VALUE_TYPES)
            if (type == description.valueType)
                node["type"] = name;
        node["range"] = {description.min, description.max};
        for (const auto& [steps, name] : STEPS)
            if (steps == description.steps)
                node["steps"] = name;
        if (description.valueType == Types::KeyframeValueType::Vector3)
            node["valueNames"] = description.valueNames;
    }

    std::optional<Description> Read(const nlohmann::json& node)
    {
        if (!node.is_object())
            return std::nullopt;
        Description description;
        description.id = node.value("id", std::string());
        if (description.id.empty())
            return std::nullopt;
        description.name = node.value("name", description.id);
        const auto type = node.value("type", std::string("FloatingPoint"));
        const auto valueType = std::ranges::find(VALUE_TYPES, type, &std::pair<Types::KeyframeValueType, std::string_view>::second);
        if (valueType == VALUE_TYPES.end())
            return std::nullopt;
        description.valueType = valueType->first;
        const auto range = node.value("range", std::vector<float>{});
        if (range.size() == 2 && range[1] > range[0])
        {
            description.min = range[0];
            description.max = range[1];
        }
        const auto steps = node.value("steps", std::string("None"));
        if (const auto found = std::ranges::find(STEPS, steps, &std::pair<Steps, std::string_view>::second);
            found != STEPS.end())
            description.steps = found->first;
        const auto names = node.value("valueNames", std::vector<std::string>{});
        for (std::size_t i = 0; i < names.size() && i < description.valueNames.size(); ++i)
            description.valueNames[i] = names[i];
        return description;
    }
}  // namespace IWXMVM::Components::RuntimeProperties
