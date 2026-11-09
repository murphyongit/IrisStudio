#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/RestartLogic.hpp"

using namespace IWXMVM::Components;
using RestartLogic::Frame;
using RestartLogic::Kind;
using RestartLogic::Load;
using RestartLogic::ReShadeState;
using RestartLogic::Step;

// The editor's wait for a restart of the renderer: the game's change after a reset of
// the device, ReShade's effects again, then Pack and Surface depth

namespace
{
    constexpr ReShadeState NONE{.present = false};
    constexpr ReShadeState LOADING{.present = true, .effectsOn = true, .loading = true};
    constexpr ReShadeState LOADING_OFF{.present = true, .effectsOn = false, .loading = true};
    constexpr ReShadeState LOADED_OFF{.present = true, .effectsOn = false, .loading = false};

    Frame ReShadeFrame(std::uint32_t resets, const ReShadeState& reshade)
    {
        return {.resets = resets, .changed = true, .reshade = reshade};
    }

    Frame LoadFrame(const Load& pack, const Load& surface = {}, bool moving = false)
    {
        return {.resets = 1, .changed = true, .moving = moving, .pack = pack, .surface = surface};
    }

    // asked of the game, after the first frames of its screen
    RestartLogic::Wait Asked(Kind kind, std::uint64_t now, std::uint32_t resets)
    {
        auto wait = RestartLogic::Start(kind, now, resets);
        for (std::uint32_t i = 0; i + 1 < RestartLogic::SCREEN_FRAMES; ++i)
            CHECK(RestartLogic::Advance(wait, now, {.resets = resets}) == Step::Held);
        CHECK(RestartLogic::Advance(wait, now, {.resets = resets}) == Step::Ask);
        return wait;
    }

    // at Pack's step, no ReShade, at 1000 ms
    RestartLogic::Wait PastGame(Kind kind)
    {
        auto wait = RestartLogic::Start(kind, 0, 0);
        wait.held = false;
        wait.gameDone = 1000;
        wait.atStart = true;
        return wait;
    }
}

TEST("restart wait: the game's change counts once the device was reset after the wait started")
{
    auto wait = Asked(Kind::Shadows, 1000, 4);
    // the shadow map's size can change before the device's reset
    CHECK(RestartLogic::Advance(wait, 1100, {.resets = 4, .changed = true}) == Step::Game);
    CHECK(RestartLogic::Advance(wait, 1200, {.resets = 5, .changed = false}) == Step::Game);
    CHECK(!wait.gameDone);
    CHECK(RestartLogic::Advance(wait, 1600, {.resets = 5, .changed = true}) == Step::Done);
    CHECK(wait.gameDone == 1600u);
    CHECK(!wait.waitsForReShade);
    CHECK(!wait.reshadeDone);
}

TEST("restart wait: with its effects on, ReShade is waited for until it draws them again")
{
    auto wait = Asked(Kind::Back, 0, 1);
    // the test demo at 1,920 x 1,080: the game 5 s, then ReShade about 5 s more
    CHECK(RestartLogic::Advance(wait, 5000, ReShadeFrame(2, LOADING)) == Step::ReShade);
    CHECK(wait.waitsForReShade);
    CHECK(RestartLogic::Advance(wait, 9000, ReShadeFrame(2, LOADING)) == Step::ReShade);
    // a technique not made yet is left out of ReShade's first frames
    auto drawing = LOADING;
    drawing.framesSince = 2;
    CHECK(RestartLogic::Advance(wait, 10000, ReShadeFrame(2, drawing)) == Step::ReShade);
    drawing.framesSince = 3;
    CHECK(RestartLogic::Advance(wait, 10050, ReShadeFrame(2, drawing)) == Step::Done);
    CHECK(wait.gameDone == 5000u);
    CHECK(wait.reshadeDone == 10050u);
}

TEST("restart wait: with its effects off, ReShade still loads them, and is waited for until it has (#46)")
{
    auto wait = Asked(Kind::Back, 0, 1);
    CHECK(RestartLogic::Advance(wait, 5000, ReShadeFrame(2, LOADING_OFF)) == Step::ReShade);
    CHECK(wait.waitsForReShade);
    // a size it never compiled at: over two minutes, and no frame of its effects to count
    CHECK(RestartLogic::Advance(wait, 130000, ReShadeFrame(2, LOADING_OFF)) == Step::ReShade);
    CHECK(RestartLogic::Advance(wait, 131000, ReShadeFrame(2, LOADED_OFF)) == Step::Done);
    CHECK(wait.reshadeDone == 131000u);
    CHECK(!wait.reshadeTimedOut);
}

TEST("restart wait: no ReShade in the game isn't waited for")
{
    auto wait = Asked(Kind::Back, 0, 0);
    CHECK(RestartLogic::Advance(wait, 600, ReShadeFrame(1, NONE)) == Step::Done);
    CHECK(!wait.waitsForReShade);
}

TEST("restart wait: ReShade loaded with nothing to draw ends after a moment")
{
    auto wait = Asked(Kind::Back, 0, 0);
    const ReShadeState loaded{.present = true, .effectsOn = true, .loading = false};
    CHECK(RestartLogic::Advance(wait, 600, ReShadeFrame(1, loaded)) == Step::ReShade);
    CHECK(RestartLogic::Advance(wait, 3599, ReShadeFrame(1, loaded)) == Step::ReShade);
    CHECK(RestartLogic::Advance(wait, 3600, ReShadeFrame(1, loaded)) == Step::Done);
}

TEST("restart wait: the game gets 30 s and ReShade 5 minutes after it, for its compiles at a new size")
{
    auto game = Asked(Kind::Capture, 1000, 0);
    CHECK(RestartLogic::Advance(game, 30999, {.resets = 0}) == Step::Game);
    CHECK(RestartLogic::Advance(game, 31000, {.resets = 0}) == Step::GameTimedOut);

    auto reshade = Asked(Kind::Back, 0, 0);
    CHECK(RestartLogic::Advance(reshade, 4000, ReShadeFrame(1, LOADING)) == Step::ReShade);
    // 2,560 x 1,440 for the first time: over a minute
    CHECK(RestartLogic::Advance(reshade, 94000, ReShadeFrame(1, LOADING)) == Step::ReShade);
    auto drawing = LOADING;
    drawing.framesSince = 2;
    CHECK(RestartLogic::Advance(reshade, 303999, ReShadeFrame(1, drawing)) == Step::ReShade);
    CHECK(RestartLogic::Advance(reshade, 304000, ReShadeFrame(1, drawing)) == Step::Done);
    CHECK(reshade.reshadeTimedOut);
}

TEST("restart wait: a restart asked for while ReShade loads is held back, as a reset would hang inside it (#46)")
{
    auto wait = RestartLogic::Start(Kind::Back, 1000, 3);
    CHECK(wait.held);
    for (std::uint32_t i = 0; i < RestartLogic::SCREEN_FRAMES + 2; ++i)
        CHECK(RestartLogic::Advance(wait, 90000, ReShadeFrame(3, LOADING_OFF)) == Step::Held);
    CHECK(RestartLogic::Advance(wait, 91000, ReShadeFrame(3, LOADED_OFF)) == Step::Ask);
    CHECK(!wait.held);
    CHECK(wait.heldMs == 90000u);
    // the game's time starts as it's asked
    CHECK(RestartLogic::Advance(wait, 120999, ReShadeFrame(3, LOADED_OFF)) == Step::Game);
    CHECK(RestartLogic::Advance(wait, 121000, {.resets = 4, .changed = true, .reshade = LOADED_OFF}) == Step::Done);
}

TEST("restart wait: a restart isn't held back for ever, and not at all when ReShade isn't loading")
{
    auto wait = RestartLogic::Start(Kind::Shadows, 0, 0);
    for (std::uint32_t i = 0; i < RestartLogic::SCREEN_FRAMES; ++i)
        CHECK(RestartLogic::Advance(wait, 299999, ReShadeFrame(0, LOADING)) == Step::Held);
    CHECK(RestartLogic::Advance(wait, 300000, ReShadeFrame(0, LOADING)) == Step::Ask);
    CHECK(wait.heldMs == 300000u);

    auto free = Asked(Kind::Shadows, 0, 0);
    CHECK(!free.held);
    CHECK(free.heldMs == 0u);
    CHECK(RestartLogic::Advance(free, 100, ReShadeFrame(0, LOADED_OFF)) == Step::Game);
}

TEST("restart wait: the game is asked once the screen has been drawn, so the kept picture has it (#96)")
{
    auto wait = RestartLogic::Start(Kind::Shadows, 0, 0);
    for (std::uint32_t i = 0; i + 1 < RestartLogic::SCREEN_FRAMES; ++i)
        CHECK(RestartLogic::Advance(wait, i, {}) == Step::Held);
    CHECK(RestartLogic::Advance(wait, 10, {}) == Step::Ask);
}

TEST("restart wait: a capture goes to its start tick after ReShade, and Pack and Surface depth are waited for there")
{
    auto wait = Asked(Kind::Capture, 0, 0);
    CHECK(RestartLogic::Advance(wait, 5000, ReShadeFrame(1, LOADING_OFF)) == Step::ReShade);
    CHECK(RestartLogic::Advance(wait, 6000, ReShadeFrame(1, LOADED_OFF)) == Step::AtStart);
    CHECK(wait.atStart);
    // the rewind to the start tick: its frames don't count
    const Load pack{.on = true};
    for (std::uint32_t i = 0; i < RestartLogic::LOAD_READY_FRAMES + 2; ++i)
        CHECK(RestartLogic::Advance(wait, 6100 + i, LoadFrame(pack, {}, true)) == Step::Pack);
    for (std::uint32_t i = 0; i + 1 < RestartLogic::LOAD_READY_FRAMES; ++i)
        CHECK(RestartLogic::Advance(wait, 7000 + i, LoadFrame(pack)) == Step::Pack);
    CHECK(RestartLogic::Advance(wait, 7100, LoadFrame(pack)) == Step::Done);
    CHECK(wait.pack.on);
    CHECK(wait.pack.done == 7100u);
}

TEST("restart wait: Pack and Surface depth off aren't waited for")
{
    auto wait = Asked(Kind::Capture, 0, 0);
    CHECK(RestartLogic::Advance(wait, 5000, {.resets = 1, .changed = true}) == Step::AtStart);
    // what they'd have left doesn't count with them off
    CHECK(RestartLogic::Advance(wait, 5010, LoadFrame({.on = false, .drawn = 40}, {.on = false, .drawn = 7})) ==
          Step::Done);
    CHECK(!wait.pack.on);
    CHECK(!wait.surface.on);
}

TEST("restart wait: a capture waits for the pack's read-ahead too, a view in the editor only for what it draws")
{
    auto capture = PastGame(Kind::Capture);
    for (std::uint32_t i = 0; i < RestartLogic::LOAD_READY_FRAMES + 2; ++i)
        CHECK(RestartLogic::Advance(capture, 2000 + i, LoadFrame({.on = true, .ahead = 300})) == Step::Pack);

    auto back = PastGame(Kind::Back);
    for (std::uint32_t i = 0; i + 1 < RestartLogic::LOAD_READY_FRAMES; ++i)
        CHECK(RestartLogic::Advance(back, 2000 + i, LoadFrame({.on = true, .ahead = 300})) == Step::Pack);
    CHECK(RestartLogic::Advance(back, 2100, LoadFrame({.on = true, .ahead = 300})) == Step::Done);
}

TEST("restart wait: a frame with something left starts the count of clear frames again")
{
    auto wait = PastGame(Kind::Back);
    for (std::uint32_t i = 0; i + 1 < RestartLogic::LOAD_READY_FRAMES; ++i)
        CHECK(RestartLogic::Advance(wait, 2000 + i, LoadFrame({.on = true})) == Step::Pack);
    // the texture of a surface just come into view
    CHECK(RestartLogic::Advance(wait, 2100, LoadFrame({.on = true, .drawn = 1})) == Step::Pack);
    CHECK(wait.pack.clearFrames == 0u);
    CHECK(RestartLogic::Advance(wait, 2200, LoadFrame({.on = true})) == Step::Pack);
}

TEST("restart wait: Pack and then Surface depth run out of time, a capture later than a view, and the wait ends")
{
    // (images the frame never draws, over the budget, or a first build of the map's heights)
    auto capture = PastGame(Kind::Capture);
    const Load left{.on = true, .drawn = 3};
    CHECK(RestartLogic::Advance(capture, 2000, LoadFrame(left, left)) == Step::Pack);
    CHECK(RestartLogic::Advance(capture, 61999, LoadFrame(left, left)) == Step::Pack);
    CHECK(RestartLogic::Advance(capture, 62000, LoadFrame(left, left)) == Step::Surface);
    CHECK(capture.pack.timedOut);
    CHECK(RestartLogic::Advance(capture, 121999, LoadFrame(left, left)) == Step::Surface);
    CHECK(RestartLogic::Advance(capture, 122000, LoadFrame(left, left)) == Step::Done);
    CHECK(capture.surface.timedOut);

    auto back = PastGame(Kind::Back);
    CHECK(RestartLogic::Advance(back, 2000, LoadFrame(left)) == Step::Pack);
    CHECK(RestartLogic::Advance(back, 12000, LoadFrame(left)) == Step::Done);
    CHECK(back.pack.timedOut);
}

TEST("restart wait: Surface depth is waited for after Pack")
{
    auto wait = PastGame(Kind::Shadows);
    const Load clear{.on = true};
    for (std::uint32_t i = 0; i + 1 < RestartLogic::LOAD_READY_FRAMES; ++i)
        CHECK(RestartLogic::Advance(wait, 2000 + i, LoadFrame(clear, clear)) == Step::Pack);
    CHECK(RestartLogic::Advance(wait, 2100, LoadFrame(clear, {.on = true, .drawn = 2})) == Step::Surface);
    for (std::uint32_t i = 0; i + 1 < RestartLogic::LOAD_READY_FRAMES; ++i)
        CHECK(RestartLogic::Advance(wait, 2200 + i, LoadFrame(clear, clear)) == Step::Surface);
    CHECK(RestartLogic::Advance(wait, 2300, LoadFrame(clear, clear)) == Step::Done);
}

TEST("restart screen: the line says the step, with Pack's and Surface depth's counts")
{
    auto wait = RestartLogic::Start(Kind::Capture, 0, 0);
    CHECK(RestartLogic::GetScreen(wait, Step::Held, 0).line == "Restarting the renderer");
    CHECK(RestartLogic::GetScreen(wait, Step::Game, 0).line == "Restarting the renderer");
    CHECK(RestartLogic::GetScreen(wait, Step::ReShade, 0).line == "Loading ReShade");

    // before the first frame of the step there's nothing to count
    CHECK(RestartLogic::GetScreen(wait, Step::Pack, 0).line == "Loading Pack");
    wait.pack.most = 1495;
    wait.pack.left = 1075;
    CHECK(RestartLogic::GetScreen(wait, Step::Pack, 0).line == "Loading Pack 420 of 1,495");
    wait.pack.left = 0;
    CHECK(RestartLogic::GetScreen(wait, Step::Pack, 0).line == "Loading Pack 1,495 of 1,495");
    wait.surface.most = 208;
    wait.surface.left = 168;
    CHECK(RestartLogic::GetScreen(wait, Step::Surface, 0).line == "Loading Surface depth 40 of 208");
}

TEST("restart screen: the counts follow what is left, the most there was being the total")
{
    auto wait = PastGame(Kind::Back);
    CHECK(RestartLogic::Advance(wait, 2000, LoadFrame({.on = true, .drawn = 300})) == Step::Pack);
    // more come into view
    CHECK(RestartLogic::Advance(wait, 2100, LoadFrame({.on = true, .drawn = 525})) == Step::Pack);
    CHECK(RestartLogic::Advance(wait, 2200, LoadFrame({.on = true, .drawn = 105})) == Step::Pack);
    CHECK(RestartLogic::GetScreen(wait, Step::Pack, 2200).line == "Loading Pack 420 of 525");
}

TEST("restart screen: a capture's count is the whole pack left, drawn and read ahead")
{
    auto wait = PastGame(Kind::Capture);
    CHECK(RestartLogic::Advance(wait, 2000, LoadFrame({.on = true, .drawn = 500, .ahead = 995})) == Step::Pack);
    CHECK(RestartLogic::Advance(wait, 2100, LoadFrame({.on = true, .drawn = 0, .ahead = 1075})) == Step::Pack);
    CHECK(RestartLogic::GetScreen(wait, Step::Pack, 2100).line == "Loading Pack 420 of 1,495");
}

TEST("restart screen: the time shows after 10 s, from the screen coming up")
{
    auto wait = RestartLogic::Start(Kind::Capture, 1000, 0);
    CHECK(RestartLogic::GetScreen(wait, Step::Game, 10999).time.empty());
    CHECK(RestartLogic::GetScreen(wait, Step::Game, 11000).time == "0:10");
    CHECK(RestartLogic::GetScreen(wait, Step::Game, 135999).time == "2:14");
}

TEST("restart screen: after 20 s on ReShade it says why, and not before, nor on other steps")
{
    auto wait = Asked(Kind::Capture, 0, 0);
    CHECK(RestartLogic::Advance(wait, 5000, ReShadeFrame(1, LOADING)) == Step::ReShade);
    CHECK(RestartLogic::GetScreen(wait, Step::ReShade, 24999).note.empty());
    CHECK(RestartLogic::GetScreen(wait, Step::ReShade, 25000).note == "First time at this size, a few minutes");
    CHECK(RestartLogic::GetScreen(wait, Step::Pack, 25000).note.empty());

    // a restart held back: the line and the note are ReShade's
    auto held = RestartLogic::Start(Kind::Back, 1000, 3);
    CHECK(RestartLogic::Advance(held, 1000, ReShadeFrame(3, LOADING)) == Step::Held);
    CHECK(RestartLogic::GetScreen(held, Step::Held, 1000).line == "Loading ReShade");
    CHECK(RestartLogic::GetScreen(held, Step::Held, 20999).note.empty());
    CHECK(RestartLogic::GetScreen(held, Step::Held, 21000).note == "First time at this size, a few minutes");
}

TEST("restart screen: the ring turns by the clock, its arc a share of the turn from the top, clockwise")
{
    CHECK(RestartLogic::GetRingTurn(0) == 0.0f);
    CHECK(RestartLogic::GetRingTurn(270) == 0.25f);
    CHECK(RestartLogic::GetRingTurn(1080) == 0.0f);

    constexpr float RADIUS = 9.0f, THICKNESS = 3.0f;
    // off the ring
    CHECK(RestartLogic::GetRingPixel(0.0f, 0.0f, RADIUS, THICKNESS, 0.0f).track == 0.0f);
    CHECK(RestartLogic::GetRingPixel(14.0f, 0.0f, RADIUS, THICKNESS, 0.0f).track == 0.0f);
    // the arc starts at the top and takes 0.28 of a turn: a right of the top has it, below and left don't
    const auto inArc = RestartLogic::GetRingPixel(RADIUS * 0.71f, -RADIUS * 0.71f, RADIUS, THICKNESS, 0.0f);
    CHECK(inArc.track == 1.0f);
    CHECK(inArc.arc == 1.0f);
    const auto below = RestartLogic::GetRingPixel(0.0f, RADIUS, RADIUS, THICKNESS, 0.0f);
    CHECK(below.track == 1.0f);
    CHECK(below.arc == 0.0f);
    CHECK(RestartLogic::GetRingPixel(-RADIUS, 0.0f, RADIUS, THICKNESS, 0.0f).arc == 0.0f);
    // half a turn on, the arc is at the bottom
    CHECK(RestartLogic::GetRingPixel(-RADIUS * 0.71f, RADIUS * 0.71f, RADIUS, THICKNESS, 0.5f).arc == 1.0f);
    // the edge of the track is soft
    const auto edge = RestartLogic::GetRingPixel(RADIUS + THICKNESS / 2.0f, 0.0f, RADIUS, THICKNESS, 0.5f).track;
    CHECK(edge > 0.4f && edge < 0.6f);
}

TEST("restart wait: the loading screen's words and the toasts")
{
    CHECK(RestartLogic::GetLabel(Kind::Capture, "3840x2160") == "SWITCHING TO 3840x2160 FOR THE CAPTURE");
    CHECK(RestartLogic::GetLabel(Kind::Back, "1920x1080") == "SWITCHING BACK TO 1920x1080");
    CHECK(RestartLogic::GetLabel(Kind::Shadows, "High") == "SWITCHING SHADOW QUALITY TO HIGH");
    CHECK(RestartLogic::DescribeFramesWithoutReShade(12) == "ReShade was loading for 12 frames of the capture");
    CHECK(RestartLogic::DescribeFramesWithoutReShade(1) == "ReShade was loading for 1 frame of the capture");
    CHECK(RestartLogic::DescribeNotReady("Pack") == "Pack wasn't ready as the capture began");
}
