#pragma once
#include "Types/FrozenSpan.hpp"

namespace IWXMVM::Components::BulletTime
{
    // Edits of the bullet time spans, each a step of the undo history. The editor's ticks
    // after a span (the campath, Look keys, markers, the capture range) move along with it, so what is keyed after it
    // stays on the same moment of the demo; actors and kills are in the demo's time and stay. Render thread.

    inline constexpr std::uint32_t DEFAULT_LENGTH = 2000;
    inline constexpr std::uint32_t MIN_LENGTH = 100;

    // a span at a timeline tick (in one: that one longer); returns its index
    std::size_t Add(std::uint32_t timelineTick, std::uint32_t length = DEFAULT_LENGTH);
    void SetLength(std::size_t index, std::uint32_t length);
    void Remove(std::size_t index);
    void SetActorsMove(std::size_t index, bool move);

    // a drag of a span's end: its length frame by frame, one step once it ends
    void BeginResize(std::size_t index);
    void UpdateResize(std::uint32_t length);
    void EndResize();

    // "Bullet time 2.0 s at 0:45.47" (the demo's tick it holds at, on the timeline)
    std::string Describe(std::size_t index);
}  // namespace IWXMVM::Components::BulletTime
