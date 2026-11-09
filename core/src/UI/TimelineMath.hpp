#pragma once

// The timeline's arithmetic, apart from ImGui so it can be tested: where ticks are across the lanes, zooming and
// panning, the ruler, snapping, box selection and moving what is selected. A tick is a millisecond of the timeline.
namespace IWXMVM::UI::TimelineMath
{
    // the ticks the timeline shows, from start to end
    struct View
    {
        std::int32_t start = 0;
        std::int32_t end = 0;

        bool operator==(const View&) const = default;
    };

    // Where ticks are across the lanes: the view's start at x0, its end at x0 + width
    struct Axis
    {
        View view;
        float x0 = 0.0f;
        float width = 1.0f;

        float ToX(double tick) const;
        double ToTick(float x) const;
        // how many ticks so many pixels are
        double TicksPer(float pixels) const;
    };

    // how much one notch of the wheel zooms in or out
    inline constexpr double ZOOM_STEP = 1.2;

    // Zooms in by ZOOM_STEP a notch (out for negative notches), keeping the tick under the mouse where it is (at a
    // fraction of the width, 0 at the left), showing no fewer than minSpan ticks and nothing outside 0 to maxTick
    View Zoom(View view, float notches, float mouseFraction, std::int32_t minSpan, std::int32_t maxTick);

    // Moves the view by ticks (negative to the left), as wide as it was, within 0 to maxTick
    View Pan(View view, double ticks, std::int32_t maxTick);

    // A view of start to end with a margin of a fraction of their span on each side, at least minSpan wide, within 0
    // to maxTick
    View Fit(std::int64_t start, std::int64_t end, double margin, std::int32_t minSpan, std::int32_t maxTick);

    // The ticks of lines every spacing ticks (the multiples of it) from from up to before to: calls draw with each.
    // None for a spacing of 0.
    void ForEachLine(std::uint32_t from, std::uint32_t to, std::uint32_t spacing,
                     const std::function<void(std::uint32_t tick)>& draw);

    // The ruler's lines: labelled ones every major ticks, the smallest of 10 ms, 20 ms, 50 ms, 100 ms... 1 s, 2 s,
    // 5 s, 10 s, 15 s, 30 s, 1 min... that puts labels at least minPixels apart, and minor ones between them
    struct RulerSpacing
    {
        std::uint32_t major = 1000;
        std::uint32_t minor = 200;
    };
    RulerSpacing GetRulerSpacing(double ticksPerPixel, float minPixels);

    // "0:41.20": minutes, seconds and hundredths
    std::string FormatTime(std::int64_t tick);
    // a ruler label, with no more digits than its spacing needs: "0:41", "0:41.5", "0:41.25"
    std::string FormatRulerTime(std::uint32_t tick, std::uint32_t spacing);
    // A time typed in: seconds ("41.2"), minutes and seconds ("0:41.20", "1:02"), or ticks ("41200t"); nothing when
    // it isn't one
    std::optional<std::uint32_t> ParseTime(std::string_view text);

    // The target nearest to a tick, if one is within the tolerance (in ticks); the index into targets
    std::optional<std::size_t> Snap(std::int64_t tick, std::span<const std::int64_t> targets, double tolerance);

    // what is being moved, by where it starts and ends (the same tick for a key)
    struct Span
    {
        std::int64_t start = 0;
        std::int64_t end = 0;

        bool operator==(const Span&) const = default;
    };

    // How far to move what is being dragged
    struct Move
    {
        std::int64_t delta = 0;
        std::optional<std::size_t> target;  // the target it snapped to, the index into targets
    };

    // A drag of delta ticks of items: snapped when the start or end of one of them comes within the tolerance of a
    // target (the nearest wins, so it lands exactly on it), and held back so that none starts before minTick or
    // after maxTick (a snap that would need holding back isn't taken)
    Move SnapMove(std::span<const Span> items, std::int64_t delta, std::span<const std::int64_t> targets,
                  double tolerance, std::int64_t minTick, std::int64_t maxTick);

    // an item on a row of the lanes, or a box dragged over them: a stretch of ticks and a stretch of height
    struct Box
    {
        std::int64_t start = 0;
        std::int64_t end = 0;
        float top = 0.0f;
        float bottom = 0.0f;
    };

    // The items a box touches, by their index; the box's corners can be in any order
    std::vector<std::size_t> InBox(std::span<const Box> items, Box box);

    // The items of a row between two ticks, both included, by their index (Shift+click selects from the item
    // clicked last to this one); from and to in any order
    std::vector<std::size_t> InRange(std::span<const std::int64_t> ticks, std::int64_t from, std::int64_t to);

    // Keys of a track after some of them moved: one that moved onto the tick of another is put a tick further along
    // the way it moved (direction 1 or -1), until no two share a tick (interpolating needs them apart). Keys before 0
    // are put at 0 first.
    void SeparateTicks(std::vector<std::int64_t>& ticks, const std::vector<bool>& moved, int direction);

    // An actor's path bar across the lanes, in x: each stretch from point i to the
    // next, from when it sets off to when it reaches it; the tick where it sets off from each point but the last; each
    // wait at a point, from when it reaches it to when it sets off; and the end's handle (the last point's), none when
    // it dies on the way
    struct PathBar
    {
        struct Span
        {
            std::size_t index = 0;  // the stretch's, or the point's
            float x0 = 0.0f;
            float x1 = 0.0f;
        };
        struct Tick
        {
            std::size_t point = 0;
            float x = 0.0f;
        };
        std::vector<Span> stretches;
        std::vector<Tick> ticks;
        std::vector<Span> waits;
        std::optional<float> end;
        std::size_t endPoint = 0;
    };
    // what is under the mouse on it: a stretch, a point's tick, a wait (the point it waits at) or the end
    struct PathPiece
    {
        enum class Kind : std::uint8_t
        {
            Stretch,
            Point,
            Wait,
            End,
        };
        Kind kind = Kind::Stretch;
        std::size_t index = 0;  // the stretch's, or the point's

        bool operator==(const PathPiece&) const = default;
    };
    // The piece at x: the end's handle within the tolerance wins over the last tick, a tick within it (the nearest) over
    // the stretches and waits round it; none off the bar
    std::optional<PathPiece> PickPathBar(const PathBar& bar, float x, float tolerance);

    // A group's graph button (the Look's), given whether each of its tracks is in the graph: the tracks to put in or
    // take out, by their index. Those not in go in; when all are in already, all go out.
    std::vector<std::size_t> GetGroupToggle(const std::vector<bool>& inGraph);

    // The graph's values up its height: min at the bottom, max at the top y0 (screen y grows downwards)
    struct ValueAxis
    {
        double min = 0.0;
        double max = 1.0;
        float y0 = 0.0f;
        float height = 1.0f;

        float ToY(double value) const;
        double ToValue(float y) const;
        // how much value so many pixels are
        double ValuesPer(float pixels) const;
    };

    // The values from low to high with a margin of a fraction of their span on each side, at least minSpan wide
    // (around their middle, for values that are all the same)
    std::pair<double, double> FitValues(double low, double high, double margin, double minSpan);

    // Zooms values in by ZOOM_STEP a notch (out for negative notches), keeping a value where it is
    std::pair<double, double> ZoomValues(double min, double max, float notches, double around);

    // The graph's lines of values: every step, the smallest of 1, 2 or 5 times a power of ten that puts them at
    // least minPixels apart
    double GetValueStep(double valuesPerPixel, float minPixels);
    // a value with the decimals its step needs: "1500", "0.25"
    std::string FormatValue(double value, double step);
}  // namespace IWXMVM::UI::TimelineMath
