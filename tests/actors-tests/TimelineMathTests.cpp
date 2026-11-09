#include "StdInclude.hpp"
#include "Test.hpp"

#include "UI/TimelineMath.hpp"

using namespace IWXMVM::UI;

namespace
{
    std::vector<std::uint32_t> Lines(std::uint32_t from, std::uint32_t to, std::uint32_t spacing)
    {
        std::vector<std::uint32_t> ticks;
        TimelineMath::ForEachLine(from, to, spacing, [&](std::uint32_t tick) { ticks.push_back(tick); });
        return ticks;
    }

    // the tick at a fraction of the width
    double TickAt(TimelineMath::View view, float fraction)
    {
        return view.start + (view.end - view.start) * static_cast<double>(fraction);
    }
}  // namespace

TEST("a notch of the wheel zooms by the same step, whatever the frame rate")
{
    const TimelineMath::View view{0, 12000};
    const auto in = TimelineMath::Zoom(view, 1.0f, 0.5f, 500, 12000);
    CHECK_NEAR(in.end - in.start, 10000, 1);
    // the middle stays where it was
    CHECK_NEAR(TickAt(in, 0.5f), 6000, 1);
    // two notches at once are two steps
    const auto twice = TimelineMath::Zoom(view, 2.0f, 0.5f, 500, 12000);
    CHECK_NEAR(twice.end - twice.start, 12000 / 1.44, 1);
    // and a notch back out undoes one
    CHECK(TimelineMath::Zoom(in, -1.0f, 0.5f, 500, 12000) == view);
}

TEST("zooming keeps the tick under the mouse where it is")
{
    const TimelineMath::View view{2000, 8000};
    for (const auto fraction : {0.0f, 0.2f, 0.75f, 1.0f})
    {
        const auto zoomed = TimelineMath::Zoom(view, 3.0f, fraction, 500, 100000);
        CHECK_NEAR(TickAt(zoomed, fraction), TickAt(view, fraction), 1);
        CHECK(zoomed.end - zoomed.start < view.end - view.start);
    }
}

TEST("zooming stays within the demo and shows at least the shortest span")
{
    // in as far as it goes
    const auto in = TimelineMath::Zoom({1000, 9000}, 50.0f, 0.3f, 500, 10000);
    CHECK(in.end - in.start == 500);
    // out as far as it goes: the whole demo
    CHECK(TimelineMath::Zoom({1000, 9000}, -50.0f, 0.3f, 500, 10000) == (TimelineMath::View{0, 10000}));
    // out near an end: moved back in, the same width
    const auto nearEnd = TimelineMath::Zoom({8000, 10000}, -1.0f, 0.9f, 500, 10000);
    CHECK(nearEnd.end == 10000);
    CHECK_NEAR(nearEnd.end - nearEnd.start, 2400, 1);
    const auto nearStart = TimelineMath::Zoom({0, 2000}, -1.0f, 0.1f, 500, 10000);
    CHECK(nearStart.start == 0);
    CHECK_NEAR(nearStart.end - nearStart.start, 2400, 1);
    // a demo shorter than the shortest span shows all of it
    CHECK(TimelineMath::Zoom({0, 300}, 5.0f, 0.5f, 500, 300) == (TimelineMath::View{0, 300}));
    // no wheel, nothing changes
    CHECK(TimelineMath::Zoom({1234, 5678}, 0.0f, 0.5f, 500, 10000) == (TimelineMath::View{1234, 5678}));
}

TEST("the timeline's lines are the multiples of their spacing in the stretch")
{
    CHECK(Lines(0, 100, 30) == (std::vector<std::uint32_t>{0, 30, 60, 90}));
    CHECK(Lines(31, 100, 30) == (std::vector<std::uint32_t>{60, 90}));
    CHECK(Lines(30, 90, 30) == (std::vector<std::uint32_t>{30, 60}));
    CHECK(Lines(50, 40, 10).empty());
    // a demo too short for a spacing has none, rather than dividing by zero
    CHECK(Lines(0, 1000, 0).empty());
    // no wrapping around past the last tick
    CHECK(Lines(4294967000u, 4294967295u, 1000000000u).empty());
    CHECK(Lines(4000000000u, 4294967295u, 1000000000u) == (std::vector<std::uint32_t>{4000000000u}));
}

TEST("ticks and x across the lanes go both ways")
{
    const TimelineMath::Axis axis{{2000, 6000}, 100.0f, 800.0f};
    CHECK_NEAR(axis.ToX(2000), 100, 0.001);
    CHECK_NEAR(axis.ToX(6000), 900, 0.001);
    CHECK_NEAR(axis.ToX(4000), 500, 0.001);
    CHECK_NEAR(axis.ToTick(500.0f), 4000, 0.001);
    CHECK_NEAR(axis.ToTick(axis.ToX(2345)), 2345, 0.01);
    // 8 pixels are 40 ticks at 5 ticks a pixel
    CHECK_NEAR(axis.TicksPer(8.0f), 40, 0.001);
    // an empty view doesn't divide by zero
    const TimelineMath::Axis empty{{100, 100}, 0.0f, 0.0f};
    CHECK(std::isfinite(empty.ToX(150)));
    CHECK(std::isfinite(empty.ToTick(10.0f)));
}

TEST("panning keeps the view's width and stays within the demo")
{
    CHECK(TimelineMath::Pan({1000, 3000}, 500, 10000) == (TimelineMath::View{1500, 3500}));
    CHECK(TimelineMath::Pan({1000, 3000}, -5000, 10000) == (TimelineMath::View{0, 2000}));
    CHECK(TimelineMath::Pan({1000, 3000}, 50000, 10000) == (TimelineMath::View{8000, 10000}));
    CHECK(TimelineMath::Pan({0, 10000}, 500, 10000) == (TimelineMath::View{0, 10000}));
}

TEST("fitting shows a stretch with a margin, within the demo")
{
    // the capture range with 5% on each side
    CHECK(TimelineMath::Fit(4000, 6000, 0.05, 500, 100000) == (TimelineMath::View{3900, 6100}));
    // either way round
    CHECK(TimelineMath::Fit(6000, 4000, 0.05, 500, 100000) == (TimelineMath::View{3900, 6100}));
    // the whole demo
    CHECK(TimelineMath::Fit(0, 10000, 0.0, 500, 10000) == (TimelineMath::View{0, 10000}));
    // at least the shortest span, moved into the demo
    CHECK(TimelineMath::Fit(100, 100, 0.05, 500, 10000) == (TimelineMath::View{0, 500}));
    CHECK(TimelineMath::Fit(9990, 10000, 0.05, 500, 10000) == (TimelineMath::View{9500, 10000}));
}

TEST("the ruler's labels are a round step, far enough apart")
{
    // 10 ticks a pixel: 1 s is 100 px
    const auto seconds = TimelineMath::GetRulerSpacing(10.0, 60.0f);
    CHECK(seconds.major == 1000);
    CHECK(seconds.minor == 200);
    // zoomed in to 0.1 ticks a pixel: 10 ms is 100 px
    CHECK(TimelineMath::GetRulerSpacing(0.1, 60.0f).major == 10);
    // a three minute demo over 1200 px: 150 ticks a pixel, 5 s is 33 px and 10 s 67 px
    CHECK(TimelineMath::GetRulerSpacing(150.0, 60.0f).major == 10000);
    // a minor step divides its major one
    for (const auto ticksPerPixel : {0.01, 0.3, 2.0, 7.0, 40.0, 300.0, 5000.0})
    {
        const auto spacing = TimelineMath::GetRulerSpacing(ticksPerPixel, 60.0f);
        CHECK(spacing.major % spacing.minor == 0);
    }
}

TEST("times are shown and typed in seconds")
{
    CHECK(TimelineMath::FormatTime(41200) == "0:41.20");
    CHECK(TimelineMath::FormatTime(192000) == "3:12.00");
    CHECK(TimelineMath::FormatTime(-1000) == "-0:01.00");
    CHECK(TimelineMath::FormatRulerTime(41000, 1000) == "0:41");
    CHECK(TimelineMath::FormatRulerTime(41500, 500) == "0:41.5");
    CHECK(TimelineMath::FormatRulerTime(41250, 50) == "0:41.25");
    CHECK(TimelineMath::FormatRulerTime(125000, 5000) == "2:05");

    CHECK(TimelineMath::ParseTime("41.2") == 41200u);
    CHECK(TimelineMath::ParseTime(" 0:41.20 ") == 41200u);
    CHECK(TimelineMath::ParseTime("1:02") == 62000u);
    CHECK(TimelineMath::ParseTime("1:00:00") == 3600000u);
    CHECK(TimelineMath::ParseTime("41200t") == 41200u);
    CHECK(TimelineMath::ParseTime(TimelineMath::FormatTime(98760)) == 98760u);
    CHECK(!TimelineMath::ParseTime(""));
    CHECK(!TimelineMath::ParseTime("abc"));
    CHECK(!TimelineMath::ParseTime("-3"));
    CHECK(!TimelineMath::ParseTime("1.5:00"));
    CHECK(!TimelineMath::ParseTime("1:2:3:4"));
}

TEST("snapping finds the nearest target within the tolerance")
{
    const std::vector<std::int64_t> targets{1000, 2000, 2030};
    CHECK(TimelineMath::Snap(1030, targets, 40) == std::optional<std::size_t>(0));
    CHECK(TimelineMath::Snap(1050, targets, 40) == std::nullopt);
    // the nearest of two in reach
    CHECK(TimelineMath::Snap(2020, targets, 40) == std::optional<std::size_t>(2));
    CHECK(TimelineMath::Snap(2010, targets, 40) == std::optional<std::size_t>(1));
    CHECK(TimelineMath::Snap(5000, {}, 40) == std::nullopt);
}

TEST("a dragged selection snaps by whichever edge comes nearest a target")
{
    // a key at 1000 and a clip from 3000 to 5000, dragged 990 ticks: the key reaches 1990, 10 from 2000, and the
    // clip's end 5990, 10 from 6000; either lands exactly
    const std::vector<TimelineMath::Span> items{{1000, 1000}, {3000, 5000}};
    const std::vector<std::int64_t> targets{2000, 6000};
    const auto move = TimelineMath::SnapMove(items, 990, targets, 40, 0, 100000);
    CHECK(move.delta == 1000);
    CHECK(move.target.has_value());

    // the clip's end nearer than the key
    const auto byEnd = TimelineMath::SnapMove(items, 985, std::vector<std::int64_t>{1950, 6000}, 40, 0, 100000);
    CHECK(byEnd.delta == 1000);
    CHECK(byEnd.target == std::optional<std::size_t>(1));

    // the clip's start
    const auto byStart = TimelineMath::SnapMove(items, 480, std::vector<std::int64_t>{3500}, 40, 0, 100000);
    CHECK(byStart.delta == 500);

    // nothing in reach: as dragged
    const auto free = TimelineMath::SnapMove(items, 500, targets, 40, 0, 100000);
    CHECK(free.delta == 500);
    CHECK(!free.target);

    // no snapping with no targets (Alt held)
    CHECK(TimelineMath::SnapMove(items, 990, {}, 40, 0, 100000).delta == 990);
}

TEST("a dragged selection stays within the timeline")
{
    const std::vector<TimelineMath::Span> items{{1000, 1000}, {3000, 5000}};
    // not before 0: the earliest start stops there
    const auto back = TimelineMath::SnapMove(items, -2500, {}, 40, 0, 100000);
    CHECK(back.delta == -1000);
    // not past the end: the latest start stops there (a clip may run past it)
    const auto forward = TimelineMath::SnapMove(items, 99000, {}, 40, 0, 10000);
    CHECK(forward.delta == 7000);
    // a snap that would go past is held back, and isn't a snap
    const auto snapped = TimelineMath::SnapMove(items, -990, std::vector<std::int64_t>{-10}, 40, 0, 100000);
    CHECK(snapped.delta == -990);
    CHECK(!snapped.target);
}

TEST("box select takes what the box touches, however it was dragged")
{
    // two keys on a row from 0 to 20 px, a clip on a row below
    const std::vector<TimelineMath::Box> items{
        {1000, 1000, 0.0f, 20.0f},
        {2000, 2000, 0.0f, 20.0f},
        {1500, 4000, 20.0f, 40.0f},
    };
    CHECK(TimelineMath::InBox(items, {900, 1100, 5.0f, 15.0f}) == (std::vector<std::size_t>{0}));
    // dragged up and to the left
    CHECK(TimelineMath::InBox(items, {2100, 900, 15.0f, 5.0f}) == (std::vector<std::size_t>{0, 1}));
    // the clip by its middle, down over both rows
    CHECK(TimelineMath::InBox(items, {1800, 2500, 10.0f, 30.0f}) == (std::vector<std::size_t>{1, 2}));
    CHECK(TimelineMath::InBox(items, {5000, 6000, 0.0f, 40.0f}).empty());
}

TEST("shift+click takes the items of a row between two ticks")
{
    const std::vector<std::int64_t> ticks{500, 1000, 1500, 2000, 2500};
    CHECK(TimelineMath::InRange(ticks, 1000, 2000) == (std::vector<std::size_t>{1, 2, 3}));
    CHECK(TimelineMath::InRange(ticks, 2000, 1000) == (std::vector<std::size_t>{1, 2, 3}));
    CHECK(TimelineMath::InRange(ticks, 2500, 2500) == (std::vector<std::size_t>{4}));
}

TEST("a group's graph button puts in its tracks that aren't in, or takes all out once all are")
{
    CHECK(TimelineMath::GetGroupToggle({false, false}) == (std::vector<std::size_t>{0, 1}));
    // those in already stay in
    CHECK(TimelineMath::GetGroupToggle({true, false, true, false}) == (std::vector<std::size_t>{1, 3}));
    CHECK(TimelineMath::GetGroupToggle({true, true, true}) == (std::vector<std::size_t>{0, 1, 2}));
    CHECK(TimelineMath::GetGroupToggle({}).empty());
}

TEST("keys moved onto others are put a tick along, so none share a tick")
{
    // the key at 2000 moved onto the one at 1000, dragging left
    std::vector<std::int64_t> ticks{1000, 1000, 3000};
    TimelineMath::SeparateTicks(ticks, {false, true, false}, -1);
    CHECK(ticks == (std::vector<std::int64_t>{1000, 999, 3000}));
    // dragging right, onto two in a row
    ticks = {1000, 1001, 1000};
    TimelineMath::SeparateTicks(ticks, {false, false, true}, 1);
    CHECK(ticks == (std::vector<std::int64_t>{1000, 1001, 1002}));
    // not before 0: the other way
    ticks = {0, 0};
    TimelineMath::SeparateTicks(ticks, {false, true}, -1);
    CHECK(ticks == (std::vector<std::int64_t>{0, 1}));
    // two moved together keep apart
    ticks = {-5, 200, 200};
    TimelineMath::SeparateTicks(ticks, {true, true, false}, 1);
    CHECK(ticks == (std::vector<std::int64_t>{0, 201, 200}));
}

TEST("the graph puts its values up its height, the largest at the top")
{
    const TimelineMath::ValueAxis axis{-10.0, 30.0, 100.0f, 200.0f};
    CHECK_NEAR(axis.ToY(30.0), 100.0f, 1e-4);
    CHECK_NEAR(axis.ToY(-10.0), 300.0f, 1e-4);
    CHECK_NEAR(axis.ToY(10.0), 200.0f, 1e-4);
    CHECK_NEAR(axis.ToValue(250.0f), 0.0, 1e-9);
    CHECK_NEAR(axis.ValuesPer(50.0f), 10.0, 1e-9);
    // nothing to show: the middle
    CHECK_NEAR((TimelineMath::ValueAxis{5.0, 5.0, 0.0f, 80.0f}.ToY(5.0)), 40.0f, 1e-4);
}

TEST("the graph fits the values it shows, with a margin, and zooms them about the mouse")
{
    const auto [low, high] = TimelineMath::FitValues(0.0, 100.0, 0.1, 1.0);
    CHECK_NEAR(low, -10.0, 1e-9);
    CHECK_NEAR(high, 110.0, 1e-9);
    // all the same value: around it
    const auto [flatLow, flatHigh] = TimelineMath::FitValues(80.0, 80.0, 0.1, 2.0);
    CHECK_NEAR(flatLow, 79.0, 1e-9);
    CHECK_NEAR(flatHigh, 81.0, 1e-9);
    // in any order
    CHECK(TimelineMath::FitValues(5.0, -5.0, 0.0, 1.0) == std::pair(-5.0, 5.0));

    const auto [inLow, inHigh] = TimelineMath::ZoomValues(0.0, 120.0, 1.0f, 60.0);
    CHECK_NEAR(inHigh - inLow, 100.0, 1e-9);
    CHECK_NEAR((inLow + inHigh) / 2, 60.0, 1e-9);
    // the value under the mouse stays where it is
    const auto [atLow, atHigh] = TimelineMath::ZoomValues(0.0, 120.0, 2.0f, 30.0);
    CHECK_NEAR((30.0 - atLow) / (atHigh - atLow), 0.25, 1e-9);
    const auto [outLow, outHigh] = TimelineMath::ZoomValues(inLow, inHigh, -1.0f, 60.0);
    CHECK_NEAR(outLow, 0.0, 1e-9);
    CHECK_NEAR(outHigh, 120.0, 1e-9);
}

TEST("the graph's lines of values are 1, 2 or 5 times a power of ten apart, and labelled with the decimals they need")
{
    CHECK_NEAR(TimelineMath::GetValueStep(1.0, 30.0f), 50.0, 1e-9);
    CHECK_NEAR(TimelineMath::GetValueStep(0.01, 30.0f), 0.5, 1e-9);
    CHECK_NEAR(TimelineMath::GetValueStep(0.004, 25.0f), 0.1, 1e-9);
    CHECK_NEAR(TimelineMath::GetValueStep(3.0, 40.0f), 200.0, 1e-9);
    CHECK_NEAR(TimelineMath::GetValueStep(0.02, 50.0f), 1.0, 1e-9);
    CHECK(TimelineMath::FormatValue(1500.0, 500.0) == "1500");
    CHECK(TimelineMath::FormatValue(0.25, 0.05) == "0.25");
    CHECK(TimelineMath::FormatValue(-0.5, 0.5) == "-0.5");
    CHECK(TimelineMath::FormatValue(-1e-12, 0.1) == "0.0");
}

TEST("a click on an actor's path bar picks the stretch, the point's tick, the wait or the end under it")
{
    using Kind = TimelineMath::PathPiece::Kind;
    // three points: start at 100, a 20 px wait at point 1 (200-220), arriving at 320
    TimelineMath::PathBar bar;
    bar.stretches = {{0, 100.0f, 200.0f}, {1, 220.0f, 320.0f}};
    bar.ticks = {{0, 100.0f}, {1, 220.0f}};
    bar.waits = {{1, 200.0f, 220.0f}};
    bar.end = 320.0f;
    bar.endPoint = 2;
    const auto pick = [&](float x) { return TimelineMath::PickPathBar(bar, x, 4.0f); };
    // a stretch between the ticks
    CHECK((pick(150.0f) == TimelineMath::PathPiece{Kind::Stretch, 0}));
    CHECK((pick(270.0f) == TimelineMath::PathPiece{Kind::Stretch, 1}));
    // a tick within 4 px wins over the stretch round it
    CHECK((pick(103.0f) == TimelineMath::PathPiece{Kind::Point, 0}));
    CHECK((pick(97.0f) == TimelineMath::PathPiece{Kind::Point, 0}));
    CHECK((pick(223.5f) == TimelineMath::PathPiece{Kind::Point, 1}));
    CHECK((pick(105.0f) == TimelineMath::PathPiece{Kind::Stretch, 0}));
    // a wait picks the point it waits at, its tick the same point
    CHECK((pick(208.0f) == TimelineMath::PathPiece{Kind::Wait, 1}));
    // the end's handle wins over the last stretch
    CHECK((pick(318.0f) == TimelineMath::PathPiece{Kind::End, 2}));
    CHECK((pick(323.0f) == TimelineMath::PathPiece{Kind::End, 2}));
    // outside the bar
    CHECK(!pick(50.0f));
    CHECK(!pick(400.0f));
    // the nearest of two ticks close together; the end over a tick on it
    bar.ticks.push_back({2, 318.0f});
    CHECK((pick(319.0f) == TimelineMath::PathPiece{Kind::End, 2}));
    bar.ticks = {{0, 100.0f}, {1, 106.0f}};
    CHECK((pick(104.0f) == TimelineMath::PathPiece{Kind::Point, 1}));
    // a path cut short by a death has no end
    bar.end.reset();
    CHECK((pick(318.0f) == TimelineMath::PathPiece{Kind::Stretch, 1}));
}
