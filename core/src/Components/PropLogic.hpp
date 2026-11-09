#pragma once
#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "RuntimeProperties.hpp"
#include "Types/Actor.hpp"
#include "Types/Keyframe.hpp"

// What a prop in the scene does that doesn't need the game: an actor of kind Prop (see
// Types::ActorPlacedProp) shown over its bar, whose place can have keys as a light's can, and the browser's
// list of the models the game has loaded.
namespace IWXMVM::Components::PropLogic
{
    // The values keys can change over time, each a property of its own per prop (Components::RuntimeProperties): where
    // it stands, and its turn (pitch, yaw, roll)
    enum class Channel
    {
        Position,
        Turn,
    };
    inline constexpr std::array<Channel, 2> ALL_CHANNELS = {Channel::Position, Channel::Turn};

    // A property's id stays with the prop's: "prop/<id>/position"
    std::string GetPropertyId(std::uint32_t prop, Channel channel);
    std::string_view GetChannelName(Channel channel);
    // Which prop a property is of, by its id (GetPropertyId); none for others
    std::optional<std::uint32_t> ParseProp(std::string_view propertyId);
    RuntimeProperties::Description Describe(std::uint32_t prop, Channel channel);
    Types::KeyframeValue GetValue(const Types::Actor& actor, Channel channel);
    void SetValue(Types::Actor& actor, Channel channel, const Types::KeyframeValue& value);

    // An id for a new prop: none of the scene's props has it, nor the keys of one not in the scene
    std::uint32_t MakeId(const std::vector<Types::Actor>& scene, const std::set<std::uint32_t>& keyed = {});

    // A prop's keys, by channel
    using Keys = std::array<std::vector<Types::Keyframe>, ALL_CHANNELS.size()>;
    // The actor with its prop's place as its keys have it at a tick. A turn goes the short way between two keys: each
    // key's angles are taken as those nearest the key's before it, so a yaw of 170 to -170 turns 20 degrees.
    Types::Actor ApplyKeys(Types::Actor actor, const Keys& keys, double tick);
    // The turn's keys with each angle nearest the one before it, as they turn
    std::vector<Types::Keyframe> UnwrapTurn(std::vector<Types::Keyframe> keys);
    // A turn as a key at a tick takes it (a change keys it there): its angles nearest the key before the tick, or the
    // key after it when there is none before, its own (in -180 to 180) when there are no others
    glm::vec3 GetTurnForKey(const std::vector<Types::Keyframe>& keys, std::uint32_t tick, glm::vec3 turn);

    // The browser's groups: the map's own first, then by the kind of model its name says
    enum class Group
    {
        Map,
        Props,
        Vehicles,
        Foliage,
        Destructibles,
        Weapons,
        GameModes,
        Other,
    };
    inline constexpr std::array ALL_GROUPS{Group::Map,           Group::Props,   Group::Vehicles,  Group::Foliage,
                                           Group::Destructibles, Group::Weapons, Group::GameModes, Group::Other};
    // what a group is called ("This map's"), and in the harness's rows ("map")
    std::string_view GetGroupName(Group group);
    std::string_view GetGroupKey(Group group);
    // Whether the browser lists a model: not a gun's view model or arms, a character's part (the Character picker has
    // those) or a model of no name of its own
    bool IsListed(std::string_view model);
    // The kind of model its name says, past This map's
    Group GetKind(std::string_view model);
    // The models the browser shows of a group (This map's: those of mapModels, sorted), or of every group with a
    // search, each word of which the name has; in the order given
    std::vector<std::string> GetShown(const std::vector<std::string>& models, const std::vector<std::string>& mapModels,
                                      Group group, std::string_view search);
    // How many models each group has, as the browser lists them
    std::array<std::size_t, ALL_GROUPS.size()> CountGroups(const std::vector<std::string>& models,
                                                           const std::vector<std::string>& mapModels);
}  // namespace IWXMVM::Components::PropLogic
