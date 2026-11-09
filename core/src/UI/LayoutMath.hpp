#pragma once

// The editor's layout, apart from ImGui so it can be tested: the view at the top left, the side panel to its right
// and the timeline under both, sized by the splitters between them, and the size of the UI's font. What the splitters
// are set to is kept in ems (font sizes), so the panel and the timeline show as much when the UI scale or the window
// changes.
namespace IWXMVM::UI::LayoutMath
{
    inline constexpr float DEFAULT_PANEL = 26.0f;
    inline constexpr float DEFAULT_TIMELINE = 14.0f;
    inline constexpr float DEFAULT_TIMELINE_WITH_GRAPH = 25.0f;

    // the least each part gets while the window has room for them all, in ems (the panel 360 px at 1600x900)
    inline constexpr float MIN_PANEL = 24.0f;
    inline constexpr float MIN_VIEW_WIDTH = 20.0f;
    inline constexpr float MIN_VIEW_HEIGHT = 10.0f;
    inline constexpr float MIN_TIMELINE = 8.0f;

    // what the splitters were set to, in ems
    struct Sizes
    {
        float panel = DEFAULT_PANEL;                            // the side panel's width
        float timeline = DEFAULT_TIMELINE;                      // the timeline's height with the graph closed
        float timelineWithGraph = DEFAULT_TIMELINE_WITH_GRAPH;  // and with it open

        bool operator==(const Sizes&) const = default;
    };

    // where the parts go, in whole pixels
    struct Layout
    {
        float viewWidth = 0.0f;  // the view at the top left, under the menu bar; the panel is as tall, to its right
        float viewHeight = 0.0f;
        float panelWidth = 0.0f;
        float timelineHeight = 0.0f;  // under both, as wide as the window; 0 without one (outside demos)

        bool operator==(const Layout&) const = default;
    };

    // The parts in a window of width by height (under the menu bar) at a font size, as the splitters were set, with
    // a timeline or not and its graph open or not: each part as set, held within its least and what leaves the other
    // part its least; a window too small for both their least is split between them in that proportion
    Layout Fit(float width, float height, float fontSize, const Sizes& sizes, bool timeline, bool graph);

    // A splitter dragged: the sizes with the panel (or the timeline, with the graph open or not) at so many pixels,
    // held as Fit would hold it
    Sizes SetPanel(Sizes sizes, float pixels, float width, float fontSize);
    Sizes SetTimeline(Sizes sizes, float pixels, float height, float fontSize, bool graph);

    // Where a readout over the game's picture goes, its text's top left: READOUT_IN and READOUT_UP ems in from a bottom
    // corner of the picture, in its half of the picture so two don't run together, and inside it however small the
    // picture or large the text
    inline constexpr float READOUT_IN = 5.0f;
    inline constexpr float READOUT_UP = 4.0f;
    struct Rect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
    };
    std::pair<float, float> PlaceReadout(const Rect& picture, float textWidth, float textHeight, float fontSize,
                                         bool right);

    // The UI scales offered; at 1 the font is a 106th of the window's width, as it always was (15 px at 1600 wide)
    inline constexpr std::array<float, 8> SCALES = {0.75f, 0.85f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f};
    float GetFontSize(float windowWidth, float scale);
    // The scale so many steps up (down for negative steps) the ones offered from a scale; the first or last at the ends
    float StepScale(float scale, int steps);
    // a scale read from a file, within the ones offered (1 for one that isn't a number)
    float ValidScale(float scale);
}  // namespace IWXMVM::UI::LayoutMath
