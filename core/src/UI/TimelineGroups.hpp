#pragma once
#include "Types/Actor.hpp"

// The timeline's rows by type, apart from ImGui so they can be tested: each actor of the
// scene is a row of a group, in the order of the scene, which a row dragged by its name changes. A group's header stays
// under the ruler while its rows scroll by, and a folded group still shows each row's time.
namespace IWXMVM::UI::TimelineGroups
{
    enum class Group : std::uint8_t
    {
        Actors,
        Killstreaks,
        Effects,
        Props,
        Lights,
    };
    inline constexpr std::array ALL{Group::Actors, Group::Killstreaks, Group::Effects, Group::Props, Group::Lights};

    // what a group is called, and in the harness's rows ("killstreaks")
    std::string_view GetName(Group group);
    std::string GetKey(Group group);

    Group GetGroup(const Types::Actor& actor);
    // the rows of a group, in the scene's order
    std::vector<const Types::Actor*> GetRows(std::span<const Types::Actor> actors, Group group);

    // The scene's order of actors with a row moved before another of its group, or after the last of its group
    std::vector<std::uint32_t> MoveRow(std::span<const Types::Actor> actors, std::uint32_t moved,
                                       std::optional<std::uint32_t> before);

    // where a group was drawn, up and down
    struct Span
    {
        float top = 0.0f;
        float bottom = 0.0f;
    };
    // the group whose header stays at the top of the tracks, and where: there, pushed up as its last row goes; a folded
    // group, only its header, isn't among those given
    struct Pin
    {
        std::size_t group = 0;
        float y = 0.0f;
    };
    std::optional<Pin> GetPin(std::span<const std::optional<Span>> groups, float top, float headerHeight);

    // when a row has something, for a folded group's summary: its clips' first start and last end, or its bar; none
    // without either
    std::optional<std::pair<std::uint32_t, std::uint32_t>> GetRowSpan(const Types::Actor& actor);
    // how many lines of thin bars a folded group's summary has: rows go on them in turn
    inline constexpr std::size_t SUMMARY_LINES = 5;
}  // namespace IWXMVM::UI::TimelineGroups
