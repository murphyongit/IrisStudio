#include "StdInclude.hpp"
#include "TimelineGroups.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/CharacterLogic.hpp"
#include "Components/KillstreakLogic.hpp"

namespace IWXMVM::UI::TimelineGroups
{
    std::string_view GetName(Group group)
    {
        switch (group)
        {
            case Group::Actors:
                return "Actors";
            case Group::Killstreaks:
                return "Killstreaks";
            case Group::Effects:
                return "Effects";
            case Group::Props:
                return "Props";
            case Group::Lights:
                return "Lights";
        }
        return "";
    }

    std::string GetKey(Group group)
    {
        auto key = std::string(GetName(group));
        std::ranges::transform(key, key.begin(), [](unsigned char letter) { return std::tolower(letter); });
        return key;
    }

    Group GetGroup(const Types::Actor& actor)
    {
        if (actor.effect)
            return Group::Effects;
        if (actor.light)
            return Group::Lights;
        if (actor.placedProp)
            return Group::Props;
        if (actor.mortar)
            return Group::Killstreaks;
        // (dogs with them, as the Add menus have them)
        return Components::KillstreakLogic::FindByModels(actor.models) || Components::CharacterLogic::IsDog(actor.models)
                   ? Group::Killstreaks
                   : Group::Actors;
    }

    std::vector<const Types::Actor*> GetRows(std::span<const Types::Actor> actors, Group group)
    {
        std::vector<const Types::Actor*> rows;
        for (const auto& actor : actors)
        {
            if (GetGroup(actor) == group)
                rows.push_back(&actor);
        }
        return rows;
    }

    std::vector<std::uint32_t> MoveRow(std::span<const Types::Actor> actors, std::uint32_t moved,
                                       std::optional<std::uint32_t> before)
    {
        std::vector<std::uint32_t> order;
        for (const auto& actor : actors)
            order.push_back(actor.id);
        const auto row = std::ranges::find(actors, moved, &Types::Actor::id);
        if (row == actors.end() || before == moved)
            return order;
        const auto group = GetGroup(*row);
        std::erase(order, moved);
        // (before another row of its group, or after the last of them)
        if (before)
        {
            const auto target = std::ranges::find(actors, *before, &Types::Actor::id);
            if (target != actors.end() && GetGroup(*target) == group)
            {
                order.insert(std::ranges::find(order, *before), moved);
                return order;
            }
        }
        auto after = order.begin();
        for (auto id = order.begin(); id != order.end(); ++id)
        {
            const auto actor = std::ranges::find(actors, *id, &Types::Actor::id);
            if (GetGroup(*actor) == group)
                after = id + 1;
        }
        order.insert(after, moved);
        return order;
    }

    std::optional<Pin> GetPin(std::span<const std::optional<Span>> groups, float top, float headerHeight)
    {
        for (std::size_t i = 0; i < groups.size(); ++i)
        {
            const auto& span = groups[i];
            // (one scrolled past the top with something of it still in sight)
            if (span && span->top < top && span->bottom > top)
                return Pin{i, std::min(top, span->bottom - headerHeight)};
        }
        return std::nullopt;
    }

    std::optional<std::pair<std::uint32_t, std::uint32_t>> GetRowSpan(const Types::Actor& actor)
    {
        if (const auto bar = ActorLogic::GetBar(actor))
            return bar;
        if (actor.clips.empty())
            return std::nullopt;
        auto span = std::pair(actor.clips.front().start, actor.clips.front().End());
        for (const auto& clip : actor.clips)
        {
            span.first = std::min(span.first, clip.start);
            span.second = std::max(span.second, clip.End());
        }
        return span;
    }
}  // namespace IWXMVM::UI::TimelineGroups
