#include "StdInclude.hpp"
#include "FrozenSpans.hpp"

namespace IWXMVM::Components::FrozenSpans
{
    namespace
    {
        bool Holds(const Types::FrozenSpan& span, bool actors)
        {
            return !actors || !span.actorsMove;
        }
    }  // namespace

    std::uint32_t GetStart(Spans spans, std::size_t index)
    {
        std::uint32_t before = 0;
        for (std::size_t i = 0; i < index && i < spans.size(); ++i)
            before += spans[i].length;
        return spans[index].worldTick + before;
    }

    std::uint32_t ToWorld(Spans spans, std::uint32_t timelineTick, bool actors)
    {
        std::uint32_t before = 0, held = 0;
        for (const auto& span : spans)
        {
            const auto start = span.worldTick + before;
            if (timelineTick < start)
                break;
            if (Holds(span, actors))
            {
                if (timelineTick < start + span.length)
                    return start - held;
                held += span.length;
            }
            before += span.length;
        }
        return timelineTick - held;
    }

    std::uint32_t ToTimeline(Spans spans, std::uint32_t worldTick, bool actors)
    {
        std::uint32_t before = 0, held = 0;
        for (const auto& span : spans)
        {
            const auto start = span.worldTick + before;
            // where the span starts in the time asked about
            const auto mappedStart = start - held;
            if (worldTick < mappedStart || (worldTick == mappedStart && Holds(span, actors)))
                return worldTick + held;
            if (Holds(span, actors))
                held += span.length;
            before += span.length;
        }
        return worldTick + held;
    }

    std::uint32_t ToActors(Spans spans, std::uint32_t timelineTick, std::optional<std::uint32_t> frozenTick)
    {
        return ToWorld(spans, frozenTick.value_or(timelineTick), true);
    }

    std::optional<std::size_t> FindAt(Spans spans, std::uint32_t timelineTick)
    {
        std::uint32_t before = 0;
        for (std::size_t i = 0; i < spans.size(); ++i)
        {
            const auto start = spans[i].worldTick + before;
            if (timelineTick < start)
                return std::nullopt;
            if (timelineTick < start + spans[i].length)
                return i;
            before += spans[i].length;
        }
        return std::nullopt;
    }

    std::uint32_t GetLength(Spans spans)
    {
        std::uint32_t length = 0;
        for (const auto& span : spans)
            length += span.length;
        return length;
    }

    std::pair<std::vector<Types::FrozenSpan>, std::size_t> Add(Spans spans, std::uint32_t timelineTick,
                                                               std::uint32_t length)
    {
        std::vector<Types::FrozenSpan> added(spans.begin(), spans.end());
        const auto world = ToWorld(spans, timelineTick);
        const auto same = std::ranges::find(added, world, &Types::FrozenSpan::worldTick);
        if (same != added.end())
        {
            same->length += length;
            return {added, static_cast<std::size_t>(same - added.begin())};
        }
        const auto at = std::ranges::upper_bound(added, world, {}, &Types::FrozenSpan::worldTick);
        const auto index = static_cast<std::size_t>(at - added.begin());
        added.insert(at, Types::FrozenSpan{.worldTick = world, .length = length});
        return {added, index};
    }

    std::uint32_t RippleInsert(std::uint32_t tick, std::uint32_t at, std::uint32_t length)
    {
        return tick > at ? tick + length : tick;
    }

    std::uint32_t RippleResize(std::uint32_t tick, std::uint32_t start, std::uint32_t oldLength,
                               std::uint32_t newLength)
    {
        if (tick <= start)
            return tick;
        if (tick >= start + oldLength)
            return tick + newLength - oldLength;
        return std::min(tick, start + newLength);
    }
}  // namespace IWXMVM::Components::FrozenSpans
