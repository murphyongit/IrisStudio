#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/FrozenSpans.hpp"

namespace FrozenSpans = IWXMVM::Components::FrozenSpans;
using IWXMVM::Types::FrozenSpan;

TEST("without bullet time the timeline is the demo's time")
{
    const std::vector<FrozenSpan> none;
    CHECK(FrozenSpans::ToWorld(none, 41200) == 41200);
    CHECK(FrozenSpans::ToTimeline(none, 41200) == 41200);
    CHECK(!FrozenSpans::FindAt(none, 41200));
    CHECK(FrozenSpans::GetLength(none) == 0);
}

TEST("in a span the demo holds at its tick, after it the demo goes on from there")
{
    const std::vector<FrozenSpan> spans = {{1000, 500}, {2000, 300}};
    CHECK(FrozenSpans::GetStart(spans, 0) == 1000);
    CHECK(FrozenSpans::GetStart(spans, 1) == 2500);
    CHECK(FrozenSpans::GetLength(spans) == 800);

    CHECK(FrozenSpans::ToWorld(spans, 999) == 999);
    CHECK(FrozenSpans::ToWorld(spans, 1000) == 1000);
    CHECK(FrozenSpans::ToWorld(spans, 1499) == 1000);
    CHECK(FrozenSpans::ToWorld(spans, 1500) == 1000);
    CHECK(FrozenSpans::ToWorld(spans, 1501) == 1001);
    CHECK(FrozenSpans::ToWorld(spans, 2600) == 2000);
    CHECK(FrozenSpans::ToWorld(spans, 2900) == 2100);

    CHECK(FrozenSpans::FindAt(spans, 999) == std::nullopt);
    CHECK(FrozenSpans::FindAt(spans, 1000) == 0);
    CHECK(FrozenSpans::FindAt(spans, 1499) == 0);
    CHECK(FrozenSpans::FindAt(spans, 1500) == std::nullopt);
    CHECK(FrozenSpans::FindAt(spans, 2799) == 1);

    // the demo's tick a span holds at is the span's start, those after it are later by the spans before them
    CHECK(FrozenSpans::ToTimeline(spans, 999) == 999);
    CHECK(FrozenSpans::ToTimeline(spans, 1000) == 1000);
    CHECK(FrozenSpans::ToTimeline(spans, 1001) == 1501);
    CHECK(FrozenSpans::ToTimeline(spans, 2000) == 2500);
    CHECK(FrozenSpans::ToTimeline(spans, 2001) == 2801);

    // and back, outside the spans (at a span's end the demo is still at its tick, which is the span's start)
    for (std::uint32_t tick : {0u, 999u, 1501u, 1700u, 2499u, 2801u, 5000u})
        CHECK(FrozenSpans::ToTimeline(spans, FrozenSpans::ToWorld(spans, tick)) == tick);
}

TEST("actors go on through a span that lets them move, and hold through one that doesn't")
{
    const std::vector<FrozenSpan> spans = {{1000, 500, true}, {2000, 300}};
    // the world holds in both
    CHECK(FrozenSpans::ToWorld(spans, 1200) == 1000);
    CHECK(FrozenSpans::ToWorld(spans, 2600) == 2000);
    // actors only in the second, where they are ahead of the world by the first's length
    CHECK(FrozenSpans::ToWorld(spans, 1200, true) == 1200);
    CHECK(FrozenSpans::ToWorld(spans, 2499, true) == 2499);
    CHECK(FrozenSpans::ToWorld(spans, 2600, true) == 2500);
    CHECK(FrozenSpans::ToWorld(spans, 2900, true) == 2600);
    CHECK(FrozenSpans::ToTimeline(spans, 1200, true) == 1200);
    CHECK(FrozenSpans::ToTimeline(spans, 2500, true) == 2500);
    CHECK(FrozenSpans::ToTimeline(spans, 2600, true) == 2900);
}

TEST("with Freeze time on, actors hold where it froze while the timeline runs on (#54)")
{
    const std::vector<FrozenSpan> none;
    CHECK(FrozenSpans::ToActors(none, 41200, std::nullopt) == 41200);
    // frozen at 41200: the timeline at 41200, 43000 or back at 39000 leaves them there
    for (std::uint32_t tick : {41200u, 43000u, 39000u})
        CHECK(FrozenSpans::ToActors(none, tick, 41200u) == 41200);

    // with spans, at the actors' tick of the frozen one: in a span that lets them move too
    const std::vector<FrozenSpan> spans = {{1000, 500, true}, {2000, 300}};
    CHECK(FrozenSpans::ToActors(spans, 2900, std::nullopt) == 2600);
    CHECK(FrozenSpans::ToActors(spans, 2900, 1200u) == 1200);
    CHECK(FrozenSpans::ToActors(spans, 1300, 1200u) == 1200);
    CHECK(FrozenSpans::ToActors(spans, 1300, 2600u) == 2500);
}

TEST("a span is added where the timeline is, or makes the span there longer")
{
    std::vector<FrozenSpan> spans = {{2000, 300}};
    auto [added, index] = FrozenSpans::Add(spans, 1000, 500);
    CHECK(index == 0);
    CHECK(added == std::vector<FrozenSpan>({{1000, 500}, {2000, 300}}));

    // after a span, at the world's tick there
    std::tie(added, index) = FrozenSpans::Add(added, 3000, 200);
    CHECK(index == 2);
    CHECK(added.back().worldTick == 2200);

    // inside one: that one longer
    std::tie(added, index) = FrozenSpans::Add(added, 1200, 100);
    CHECK(index == 0);
    CHECK(added.size() == 3);
    CHECK(added[0].length == 600);
}

TEST("the editor's ticks after a span move with its length")
{
    // put in at 1000: what is after moves on, what is at it stays at the span's start
    CHECK(FrozenSpans::RippleInsert(999, 1000, 500) == 999);
    CHECK(FrozenSpans::RippleInsert(1000, 1000, 500) == 1000);
    CHECK(FrozenSpans::RippleInsert(1001, 1000, 500) == 1501);

    // a span from 1000 for 500 made 800 long: after its end on by 300, inside it as it is
    CHECK(FrozenSpans::RippleResize(1200, 1000, 500, 800) == 1200);
    CHECK(FrozenSpans::RippleResize(1500, 1000, 500, 800) == 1800);
    CHECK(FrozenSpans::RippleResize(900, 1000, 500, 800) == 900);
    // made 200 long: after its end back by 300, in the part cut off to its new end
    CHECK(FrozenSpans::RippleResize(1400, 1000, 500, 200) == 1200);
    CHECK(FrozenSpans::RippleResize(1100, 1000, 500, 200) == 1100);
    CHECK(FrozenSpans::RippleResize(1600, 1000, 500, 200) == 1300);
    // taken out
    CHECK(FrozenSpans::RippleResize(1400, 1000, 500, 0) == 1000);
    CHECK(FrozenSpans::RippleResize(1600, 1000, 500, 0) == 1100);
}
