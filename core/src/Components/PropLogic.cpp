#include "StdInclude.hpp"
#include <charconv>
#include <ranges>
#include "PropLogic.hpp"

#include "KeyframeLogic.hpp"

namespace IWXMVM::Components::PropLogic
{
    namespace
    {
        constexpr std::array<std::string_view, ALL_CHANNELS.size()> CHANNEL_KEYS = {"position", "turn"};
        constexpr std::array<std::string_view, ALL_CHANNELS.size()> CHANNEL_NAMES = {"Position", "Turn"};
        // the graph's range for a position: the campaign's levels go well past the multiplayer maps'
        constexpr float MAX_POSITION = 131072.0f;
        constexpr std::string_view PREFIX = "prop/";

        std::size_t IndexOf(Channel channel)
        {
            return static_cast<std::size_t>(channel);
        }

        // an angle moved by whole turns to be as near another as it can be (190 for -170 to be near 170)
        float Nearest(float angle, float to)
        {
            return to + std::remainder(angle - to, 360.0f);
        }

        glm::vec3 Nearest(glm::vec3 turn, glm::vec3 to)
        {
            return {Nearest(turn.x, to.x), Nearest(turn.y, to.y), Nearest(turn.z, to.z)};
        }

        std::string Lower(std::string_view text)
        {
            std::string lower(text);
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
            return lower;
        }
    }  // namespace

    std::string GetPropertyId(std::uint32_t prop, Channel channel)
    {
        return std::format("prop/{}/{}", prop, CHANNEL_KEYS[IndexOf(channel)]);
    }

    std::string_view GetChannelName(Channel channel)
    {
        return CHANNEL_NAMES[IndexOf(channel)];
    }

    std::optional<std::uint32_t> ParseProp(std::string_view propertyId)
    {
        if (!propertyId.starts_with(PREFIX))
            return std::nullopt;
        const auto digits = propertyId.substr(PREFIX.size());
        std::uint32_t prop = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), prop);
        if (error != std::errc{} || end == digits.data() || end == digits.data() + digits.size() || *end != '/')
            return std::nullopt;
        return prop;
    }

    RuntimeProperties::Description Describe(std::uint32_t prop, Channel channel)
    {
        RuntimeProperties::Description description;
        description.id = GetPropertyId(prop, channel);
        description.name = std::format("Prop {}: {}", prop, CHANNEL_NAMES[IndexOf(channel)]);
        description.valueType = Types::KeyframeValueType::Vector3;
        if (channel == Channel::Position)
        {
            description.min = -MAX_POSITION;
            description.max = MAX_POSITION;
            description.valueNames = {"X", "Y", "Z"};
        }
        else
        {
            description.min = -360.0f;
            description.max = 360.0f;
            description.valueNames = {"Pitch", "Yaw", "Roll"};
        }
        return description;
    }

    Types::KeyframeValue GetValue(const Types::Actor& actor, Channel channel)
    {
        if (!actor.placedProp)
            return glm::vec3(0.0f);
        if (channel == Channel::Position)
            return actor.position;
        return glm::vec3(actor.placedProp->pitch, actor.yaw, actor.placedProp->roll);
    }

    void SetValue(Types::Actor& actor, Channel channel, const Types::KeyframeValue& value)
    {
        if (!actor.placedProp)
            return;
        if (channel == Channel::Position)
        {
            actor.position = value.vector3;
            return;
        }
        actor.placedProp->pitch = value.vector3.x;
        actor.yaw = value.vector3.y;
        actor.placedProp->roll = value.vector3.z;
    }

    std::uint32_t MakeId(const std::vector<Types::Actor>& scene, const std::set<std::uint32_t>& keyed)
    {
        std::uint32_t id = keyed.empty() ? 0 : *keyed.rbegin();
        for (const auto& actor : scene)
            if (actor.placedProp)
                id = std::max(id, actor.placedProp->id);
        return id + 1;
    }

    Types::Actor ApplyKeys(Types::Actor actor, const Keys& keys, double tick)
    {
        if (const auto& position = keys[IndexOf(Channel::Position)]; !position.empty())
            SetValue(actor, Channel::Position,
                     KeyframeLogic::Evaluate(Types::KeyframeValueType::Vector3, position, tick));
        if (const auto& turn = keys[IndexOf(Channel::Turn)]; !turn.empty())
            SetValue(actor, Channel::Turn,
                     KeyframeLogic::Evaluate(Types::KeyframeValueType::Vector3, UnwrapTurn(turn), tick));
        return actor;
    }

    std::vector<Types::Keyframe> UnwrapTurn(std::vector<Types::Keyframe> keys)
    {
        for (std::size_t i = 1; i < keys.size(); ++i)
            keys[i].value.vector3 = Nearest(keys[i].value.vector3, keys[i - 1].value.vector3);
        return keys;
    }

    glm::vec3 GetTurnForKey(const std::vector<Types::Keyframe>& keys, std::uint32_t tick, glm::vec3 turn)
    {
        const auto unwrapped = UnwrapTurn(keys);
        const auto before = std::ranges::find_if(unwrapped | std::views::reverse,
                                                 [&](const Types::Keyframe& key) { return key.tick < tick; });
        const auto after = std::ranges::find_if(unwrapped, [&](const Types::Keyframe& key) { return key.tick > tick; });
        if (before != std::ranges::end(unwrapped | std::views::reverse))
            return Nearest(turn, before->value.vector3);
        if (after != unwrapped.end())
            return Nearest(turn, after->value.vector3);
        return {std::remainder(turn.x, 360.0f), std::remainder(turn.y, 360.0f), std::remainder(turn.z, 360.0f)};
    }

    std::string_view GetGroupName(Group group)
    {
        switch (group)
        {
            case Group::Map:
                return "This map's";
            case Group::Props:
                return "Props";
            case Group::Vehicles:
                return "Vehicles";
            case Group::Foliage:
                return "Foliage";
            case Group::Destructibles:
                return "Destructibles";
            case Group::Weapons:
                return "Weapons";
            case Group::GameModes:
                return "Game modes";
            case Group::Other:
                return "Other";
        }
        return "";
    }

    std::string_view GetGroupKey(Group group)
    {
        constexpr std::array<std::string_view, ALL_GROUPS.size()> KEYS = {
            "map", "props", "vehicles", "foliage", "destructibles", "weapons", "gamemodes", "other"};
        return KEYS[static_cast<std::size_t>(group)];
    }

    bool IsListed(std::string_view model)
    {
        const auto name = Lower(model);
        if (name.empty() || name.find("viewmodel") != std::string::npos ||
            name.find("viewhands") != std::string::npos || name.starts_with("c_"))
            return false;
        // (a model named by a hash, which the game keeps for one with no name of its own)
        const bool hashed = name.size() >= 6 && std::ranges::all_of(name, [](char c) {
                                return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'a' && c <= 'f');
                            });
        return !hashed;
    }

    Group GetKind(std::string_view model)
    {
        const auto name = Lower(model);
        if (name.starts_with("p_"))
            return Group::Props;
        if (name.starts_with("t5_veh") || name.starts_with("veh"))
            return Group::Vehicles;
        if (name.find("foliage") != std::string::npos || name.find("tree") != std::string::npos)
            return Group::Foliage;
        if (name.starts_with("dest_"))
            return Group::Destructibles;
        if (name.starts_with("t5_weapon") || name.starts_with("weapon_"))
            return Group::Weapons;
        if (name.starts_with("mp_"))
            return Group::GameModes;
        return Group::Other;
    }

    std::vector<std::string> GetShown(const std::vector<std::string>& models, const std::vector<std::string>& mapModels,
                                      Group group, std::string_view search)
    {
        std::vector<std::string> words;
        {
            std::string word;
            for (const auto c : std::string(search) + " ")
            {
                if (c != ' ')
                    word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                else if (!word.empty())
                    words.push_back(std::exchange(word, {}));
            }
        }
        std::vector<std::string> shown;
        for (const auto& model : models)
        {
            if (!IsListed(model))
                continue;
            if (!words.empty())
            {
                const auto name = Lower(model);
                if (std::ranges::all_of(words,
                                        [&](const std::string& word) { return name.find(word) != std::string::npos; }))
                    shown.push_back(model);
                continue;
            }
            const bool inGroup =
                group == Group::Map ? std::ranges::binary_search(mapModels, model) : GetKind(model) == group;
            if (inGroup)
                shown.push_back(model);
        }
        return shown;
    }

    std::array<std::size_t, ALL_GROUPS.size()> CountGroups(const std::vector<std::string>& models,
                                                           const std::vector<std::string>& mapModels)
    {
        std::array<std::size_t, ALL_GROUPS.size()> counts{};
        for (const auto& model : models)
        {
            if (!IsListed(model))
                continue;
            if (std::ranges::binary_search(mapModels, model))
                ++counts[static_cast<std::size_t>(Group::Map)];
            ++counts[static_cast<std::size_t>(GetKind(model))];
        }
        return counts;
    }
}  // namespace IWXMVM::Components::PropLogic
