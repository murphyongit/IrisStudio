#pragma once

namespace IWXMVM::Components::CaptureWatch
{
    // A capture that can't reach its end tick, apart from the game so the tests in
    // tests/actors-tests build it on their own. A capture ended only once the timeline passed its end tick, and wrote
    // about 1 MB a frame until then: one whose end lay past the demo's last snapshot played to the demo's end, which
    // rewinds a second and plays again, and so never ended (16.5 GB on a short cuba film). So a capture stops at the
    // demo's end, and when its timeline doesn't move for STALL_SECONDS of video.

    inline constexpr std::int32_t STALL_SECONDS = 5;

    // the frames a capture writes without its timeline moving before it stops: STALL_SECONDS of video at its
    // framerate, each pass a frame of its own (only the first pass's frame moves the timeline)
    std::uint32_t GetStallFrames(std::int32_t framerate, std::size_t passes);

    // A capture's frames as written
    struct Watch
    {
        std::optional<std::uint32_t> tick;  // the timeline's tick at the last frame that moved it
        std::uint32_t still = 0;            // frames written since
    };

    enum class Stop
    {
        None,
        DemoEnded,  // the timeline is at the demo's end, or the demo read its end (and rewinds)
        Stalled,    // the timeline didn't move for STALL_SECONDS of video
    };

    // A frame written at a timeline tick, the timeline ending at timelineEnd: whether the capture stops there
    Stop Advance(Watch& watch, std::uint32_t tick, std::uint32_t timelineEnd, std::uint32_t stallFrames);

    // why, for the toast and the log: "Capture stopped at the demo's end, 0:10.80"; time is the tick's
    std::string Describe(Stop stop, std::string_view time);
}  // namespace IWXMVM::Components::CaptureWatch
