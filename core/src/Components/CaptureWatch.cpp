#include "StdInclude.hpp"
#include "CaptureWatch.hpp"

namespace IWXMVM::Components::CaptureWatch
{
    std::uint32_t GetStallFrames(std::int32_t framerate, std::size_t passes)
    {
        return static_cast<std::uint32_t>(std::max(framerate, 1) * STALL_SECONDS) *
               static_cast<std::uint32_t>(std::max<std::size_t>(passes, 1));
    }

    Stop Advance(Watch& watch, std::uint32_t tick, std::uint32_t timelineEnd, std::uint32_t stallFrames)
    {
        // a frozen timeline is held a tick short of the end, see Playback::CalculatePlaybackDelta
        if (timelineEnd > 0 && tick + 1 >= timelineEnd)
            return Stop::DemoEnded;

        if (watch.tick != tick)
        {
            watch.tick = tick;
            watch.still = 0;
            return Stop::None;
        }
        return ++watch.still >= stallFrames ? Stop::Stalled : Stop::None;
    }

    std::string Describe(Stop stop, std::string_view time)
    {
        switch (stop)
        {
            case Stop::DemoEnded:
                return std::format("Capture stopped at the demo's end, {}", time);
            case Stop::Stalled:
                return std::format("Capture stopped at {}: the timeline didn't move for {} s", time, STALL_SECONDS);
            default:
                return {};
        }
    }
}  // namespace IWXMVM::Components::CaptureWatch
