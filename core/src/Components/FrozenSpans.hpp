#pragma once
#include "Types/FrozenSpan.hpp"

// The timeline and the demo's time with bullet time spans. The timeline's ticks are the
// editor's (the campath, Look keys, markers, the capture range); the demo's are the world's (the demo, its kills, and
// actors). Without spans they are the same. Spans are sorted by their world tick, at most one at a tick, none empty.
namespace IWXMVM::Components::FrozenSpans
{
    using Spans = std::span<const Types::FrozenSpan>;

    // the timeline's tick a span starts at: its world tick after the lengths of the spans before it
    std::uint32_t GetStart(Spans spans, std::size_t index);

    // The world's tick at a timeline tick: before a span the same less the spans before, in one the tick it holds
    // at. Actors' time (actors) counts only the spans that hold actors: through one that lets them move they go on.
    std::uint32_t ToWorld(Spans spans, std::uint32_t timelineTick, bool actors = false);
    // The timeline's tick of a world tick (or an actor's, with actors); one a span holds at is the span's start
    std::uint32_t ToTimeline(Spans spans, std::uint32_t worldTick, bool actors = false);
    // The actors' tick at a timeline tick, and while Freeze time is on (frozenTick) at the tick it froze at: everything
    // holds there with the demo (in a span that lets actors move too) while the timeline runs on for the camera
    std::uint32_t ToActors(Spans spans, std::uint32_t timelineTick, std::optional<std::uint32_t> frozenTick);

    // the span a timeline tick is in (from its start to before its end)
    std::optional<std::size_t> FindAt(Spans spans, std::uint32_t timelineTick);
    // how much longer the timeline is than the demo
    std::uint32_t GetLength(Spans spans);

    // The spans with one added at a timeline tick (in a span: that one longer) and its index
    std::pair<std::vector<Types::FrozenSpan>, std::size_t> Add(Spans spans, std::uint32_t timelineTick,
                                                               std::uint32_t length);

    // Where a tick of the editor's goes as time is put in at a timeline tick: on after it
    std::uint32_t RippleInsert(std::uint32_t tick, std::uint32_t at, std::uint32_t length);
    // ... and as a span starting at a timeline tick gets another length: on (or back) after its end, a tick in a part
    // cut off to its new end
    std::uint32_t RippleResize(std::uint32_t tick, std::uint32_t start, std::uint32_t oldLength,
                               std::uint32_t newLength);
}  // namespace IWXMVM::Components::FrozenSpans
