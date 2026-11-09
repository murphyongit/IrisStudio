#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/CaptureWatch.hpp"

using namespace IWXMVM::Components;
using CaptureWatch::Stop;

// A capture that can't reach its end tick: it stops at the demo's end, and when its
// timeline doesn't move for five seconds of video

TEST("capture watch: five seconds of video at the capture's framerate, each pass a frame")
{
    CHECK(CaptureWatch::GetStallFrames(50, 1) == 250);
    CHECK(CaptureWatch::GetStallFrames(250, 1) == 1250);
    CHECK(CaptureWatch::GetStallFrames(50, 3) == 750);
    CHECK(CaptureWatch::GetStallFrames(50, 0) == 250);
}

TEST("capture watch: a moving timeline goes on, one that stands still stops after the stall frames")
{
    CaptureWatch::Watch watch;
    // 50 fps: 20 ms a frame
    for (std::uint32_t tick = 1000; tick < 3000; tick += 20)
        CHECK(CaptureWatch::Advance(watch, tick, 34195, 250) == Stop::None);
    // held where it is: the frame that got there, then 249 frames that stay
    for (int i = 0; i < 250; i++)
        CHECK(CaptureWatch::Advance(watch, 3000, 34195, 250) == Stop::None);
    CHECK(CaptureWatch::Advance(watch, 3000, 34195, 250) == Stop::Stalled);
}

TEST("capture watch: the frames of the later passes don't move the timeline, and don't stop it by themselves")
{
    CaptureWatch::Watch watch;
    const auto stall = CaptureWatch::GetStallFrames(50, 3);
    // three passes: the timeline moves every third frame
    for (std::uint32_t frame = 0; frame < 3000; frame++)
        CHECK(CaptureWatch::Advance(watch, 1000 + frame / 3 * 20, 100000, stall) == Stop::None);
}

TEST("capture watch: a capture stops at the demo's end, where a frozen timeline is held")
{
    // intro_cinematic: the timeline ends at 34195; frozen, it is held at 34194
    CaptureWatch::Watch watch;
    CHECK(CaptureWatch::Advance(watch, 34000, 34195, 250) == Stop::None);
    CHECK(CaptureWatch::Advance(watch, 34194, 34195, 250) == Stop::DemoEnded);
    // no end known yet
    CaptureWatch::Watch unknown;
    CHECK(CaptureWatch::Advance(unknown, 0, 0, 250) == Stop::None);
}

TEST("capture watch: the toast says why and where")
{
    CHECK(CaptureWatch::Describe(Stop::DemoEnded, "0:10.80") == "Capture stopped at the demo's end, 0:10.80");
    CHECK(CaptureWatch::Describe(Stop::Stalled, "0:10.80") ==
          "Capture stopped at 0:10.80: the timeline didn't move for 5 s");
    CHECK(CaptureWatch::Describe(Stop::None, "0:10.80").empty());
}
