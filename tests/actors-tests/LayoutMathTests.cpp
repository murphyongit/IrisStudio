#include "StdInclude.hpp"
#include "Test.hpp"

#include "UI/LayoutMath.hpp"

using namespace IWXMVM::UI;

namespace
{
    // a 1600x900 window under a menu bar of 19 px, its font 15 px
    constexpr float WIDTH = 1600.0f;
    constexpr float HEIGHT = 900.0f - 19.0f;
    constexpr float FONT = 15.0f;
}  // namespace

TEST("the default layout gives the panel 26 ems and the timeline 14, the view the rest")
{
    const auto layout = LayoutMath::Fit(WIDTH, HEIGHT, FONT, {}, true, false);
    CHECK_NEAR(layout.panelWidth, 390.0f, 0.0f);
    CHECK_NEAR(layout.viewWidth, 1210.0f, 0.0f);
    CHECK_NEAR(layout.timelineHeight, 210.0f, 0.0f);
    CHECK_NEAR(layout.viewHeight, 671.0f, 0.0f);
    // the parts fill the window
    CHECK_NEAR(layout.viewWidth + layout.panelWidth, WIDTH, 0.0f);
    CHECK_NEAR(layout.viewHeight + layout.timelineHeight, HEIGHT, 0.0f);
}

TEST("outside demos there is no timeline and the view and panel are as tall as the window")
{
    const auto layout = LayoutMath::Fit(WIDTH, HEIGHT, FONT, {}, false, false);
    CHECK_NEAR(layout.timelineHeight, 0.0f, 0.0f);
    CHECK_NEAR(layout.viewHeight, HEIGHT, 0.0f);
    CHECK_NEAR(layout.panelWidth, 390.0f, 0.0f);
}

TEST("the panel is held at 24 ems at least, and leaves the view 20")
{
    // dragged narrower than 360 px
    auto sizes = LayoutMath::SetPanel({}, 200.0f, WIDTH, FONT);
    CHECK_NEAR(sizes.panel, LayoutMath::MIN_PANEL, 1e-4f);
    CHECK_NEAR(LayoutMath::Fit(WIDTH, HEIGHT, FONT, sizes, true, false).panelWidth, 360.0f, 0.0f);
    // and wider than the view allows
    sizes = LayoutMath::SetPanel({}, 1500.0f, WIDTH, FONT);
    const auto layout = LayoutMath::Fit(WIDTH, HEIGHT, FONT, sizes, true, false);
    CHECK_NEAR(layout.viewWidth, 300.0f, 0.0f);
    CHECK_NEAR(layout.panelWidth, 1300.0f, 0.0f);
    // a size in between is kept, to the pixel
    sizes = LayoutMath::SetPanel({}, 512.0f, WIDTH, FONT);
    CHECK_NEAR(LayoutMath::Fit(WIDTH, HEIGHT, FONT, sizes, true, false).panelWidth, 512.0f, 0.0f);
}

TEST("a size kept in the file beyond the limits is held when it is laid out")
{
    LayoutMath::Sizes sizes;
    sizes.panel = 2.0f;
    sizes.timeline = 500.0f;
    const auto layout = LayoutMath::Fit(WIDTH, HEIGHT, FONT, sizes, true, false);
    CHECK_NEAR(layout.panelWidth, 360.0f, 0.0f);
    CHECK_NEAR(layout.viewHeight, 150.0f, 0.0f);
    CHECK_NEAR(layout.timelineHeight, HEIGHT - 150.0f, 0.0f);
}

TEST("the timeline has its own height with the graph open, each kept as it was set")
{
    auto sizes = LayoutMath::SetTimeline({}, 300.0f, HEIGHT, FONT, false);
    sizes = LayoutMath::SetTimeline(sizes, 450.0f, HEIGHT, FONT, true);
    CHECK_NEAR(LayoutMath::Fit(WIDTH, HEIGHT, FONT, sizes, true, false).timelineHeight, 300.0f, 0.0f);
    CHECK_NEAR(LayoutMath::Fit(WIDTH, HEIGHT, FONT, sizes, true, true).timelineHeight, 450.0f, 0.0f);
    // the graph's default is taller than the timeline's
    const auto defaults = LayoutMath::Fit(WIDTH, HEIGHT, FONT, {}, true, true);
    CHECK_NEAR(defaults.timelineHeight, 375.0f, 0.0f);
    // and dragging the timeline down to nothing leaves it 8 ems
    sizes = LayoutMath::SetTimeline({}, 10.0f, HEIGHT, FONT, false);
    CHECK_NEAR(sizes.timeline, LayoutMath::MIN_TIMELINE, 1e-4f);
}

TEST("ems keep what the panel shows at another font size")
{
    const auto sizes = LayoutMath::SetPanel({}, 450.0f, WIDTH, FONT);
    // 30 ems: 540 px at 18 px a font
    CHECK_NEAR(LayoutMath::Fit(1920.0f, 1080.0f - 22.0f, 18.0f, sizes, true, false).panelWidth, 540.0f, 0.0f);
}

TEST("a window too small for every part's least splits it in their proportion")
{
    // 24 + 20 ems wanted across, 30 ems there
    const auto layout = LayoutMath::Fit(450.0f, 200.0f, FONT, {}, true, false);
    CHECK_NEAR(layout.panelWidth, std::round(450.0f * 24.0f / 44.0f), 0.0f);
    CHECK_NEAR(layout.viewWidth + layout.panelWidth, 450.0f, 0.0f);
    CHECK_NEAR(layout.timelineHeight, std::round(200.0f * 8.0f / 18.0f), 0.0f);
    CHECK_NEAR(layout.viewHeight + layout.timelineHeight, 200.0f, 0.0f);
    // and nothing for no window
    CHECK(LayoutMath::Fit(0.0f, 0.0f, FONT, {}, true, false) == LayoutMath::Layout{});
}

TEST("the font is a 106th of the window's width at a scale of 1, as it always was")
{
    CHECK_NEAR(LayoutMath::GetFontSize(1600.0f, 1.0f), 15.0f, 0.0f);
    CHECK_NEAR(LayoutMath::GetFontSize(1920.0f, 1.0f), 18.0f, 0.0f);
    CHECK_NEAR(LayoutMath::GetFontSize(1600.0f, 1.25f), 18.0f, 0.0f);
    CHECK_NEAR(LayoutMath::GetFontSize(1600.0f, 2.0f), 30.0f, 0.0f);
    CHECK_NEAR(LayoutMath::GetFontSize(1600.0f, 0.75f), 11.0f, 0.0f);
    // never under 8 px
    CHECK_NEAR(LayoutMath::GetFontSize(320.0f, 0.75f), 8.0f, 0.0f);
}

TEST("UI scales step through the ones offered and stop at the ends")
{
    CHECK_NEAR(LayoutMath::StepScale(1.0f, 1), 1.1f, 1e-6f);
    CHECK_NEAR(LayoutMath::StepScale(1.0f, -1), 0.85f, 1e-6f);
    CHECK_NEAR(LayoutMath::StepScale(1.0f, 3), 1.5f, 1e-6f);
    CHECK_NEAR(LayoutMath::StepScale(2.0f, 1), 2.0f, 1e-6f);
    CHECK_NEAR(LayoutMath::StepScale(0.75f, -1), 0.75f, 1e-6f);
    // from one that isn't offered, from the nearest
    CHECK_NEAR(LayoutMath::StepScale(1.3f, 1), 1.5f, 1e-6f);
    // read from a file
    CHECK_NEAR(LayoutMath::ValidScale(5.0f), 2.0f, 1e-6f);
    CHECK_NEAR(LayoutMath::ValidScale(0.1f), 0.75f, 1e-6f);
    CHECK_NEAR(LayoutMath::ValidScale(std::nanf("")), 1.0f, 1e-6f);
    CHECK_NEAR(LayoutMath::ValidScale(1.3f), 1.3f, 1e-6f);
}

TEST("a readout sits 5 ems in and 4 up from a bottom corner of the picture, not of the window around it")
{
    // the picture of the Editing layout at 75%: 96 px of black on its left, where the tilt readout used to go
    const LayoutMath::Rect picture{96.0f, 64.0f, 1056.0f, 594.0f};
    const auto [leftX, leftY] = LayoutMath::PlaceReadout(picture, 18.0f, 11.0f, 11.0f, false);
    CHECK_NEAR(leftX, 96.0f + 55.0f, 0.0f);
    CHECK_NEAR(leftY, 64.0f + 594.0f - 44.0f - 11.0f, 0.0f);
    const auto [rightX, rightY] = LayoutMath::PlaceReadout(picture, 24.0f, 11.0f, 11.0f, true);
    CHECK_NEAR(rightX, 96.0f + 1056.0f - 55.0f - 24.0f, 0.0f);
    CHECK_NEAR(rightY, leftY, 0.0f);
}

TEST("a readout stays inside the picture at every UI scale, however small the picture")
{
    for (const auto windowWidth : {1280.0f, 1600.0f, 1920.0f, 2560.0f, 3840.0f})
    {
        for (const auto scale : LayoutMath::SCALES)
        {
            const auto font = LayoutMath::GetFontSize(windowWidth, scale);
            const auto text = font * 2.5f;
            for (const auto width : {2000.0f, 1056.0f, 397.0f, 120.0f, 40.0f, 10.0f})
            {
                const LayoutMath::Rect picture{211.0f, 122.0f, width, width * 9.0f / 16.0f};
                for (const bool right : {false, true})
                {
                    const auto [x, y] = LayoutMath::PlaceReadout(picture, text, font, font, right);
                    CHECK(x >= picture.x && y >= picture.y);
                    // (a picture smaller than the text has it from its top left)
                    if (text <= picture.width)
                        CHECK(x + text <= picture.x + picture.width);
                    if (font <= picture.height)
                        CHECK(y + font <= picture.y + picture.height);
                }
            }
        }
    }
}

TEST("two readouts don't run together in a small picture, and sit where they did in a large one")
{
    // the 200% picture of a timeline dragged tall: 13 ems wide
    const LayoutMath::Rect small{211.0f, 122.0f, 397.0f, 223.0f};
    const auto tilt = LayoutMath::PlaceReadout(small, 49.0f, 30.0f, 30.0f, false);
    const auto fov = LayoutMath::PlaceReadout(small, 67.0f, 30.0f, 30.0f, true);
    CHECK(tilt.first + 49.0f < fov.first);
    CHECK(tilt.first + 49.0f <= small.x + small.width / 2.0f);
    CHECK(fov.first >= small.x + small.width / 2.0f);
    // the 200% picture of the default layout: 5 ems in
    const LayoutMath::Rect large{92.0f, 122.0f, 635.0f, 357.0f};
    CHECK_NEAR(LayoutMath::PlaceReadout(large, 49.0f, 30.0f, 30.0f, false).first, 92.0f + 150.0f, 0.0f);
}
