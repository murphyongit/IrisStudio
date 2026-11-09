#include "StdInclude.hpp"
#include "TimelineMath.hpp"

#include <cctype>
#include <charconv>
#include <limits>

namespace IWXMVM::UI::TimelineMath
{
    float Axis::ToX(double tick) const
    {
        const auto span = std::max(static_cast<double>(view.end) - view.start, 1.0);
        return x0 + static_cast<float>((tick - view.start) / span * width);
    }

    double Axis::ToTick(float x) const
    {
        const auto span = std::max(static_cast<double>(view.end) - view.start, 1.0);
        return view.start + (x - x0) / std::max(width, 1.0f) * span;
    }

    double Axis::TicksPer(float pixels) const
    {
        const auto span = std::max(static_cast<double>(view.end) - view.start, 1.0);
        return pixels / std::max(width, 1.0f) * span;
    }

    View Zoom(View view, float notches, float mouseFraction, std::int32_t minSpan, std::int32_t maxTick)
    {
        const auto span = static_cast<double>(view.end) - view.start;
        if (notches == 0.0f || span <= 0.0 || maxTick <= 0)
            return view;

        mouseFraction = std::clamp(mouseFraction, 0.0f, 1.0f);
        const auto pivot = view.start + span * mouseFraction;
        const auto newSpan = std::clamp(span / std::pow(ZOOM_STEP, notches),
                                        static_cast<double>(std::min(minSpan, maxTick)), static_cast<double>(maxTick));
        // the same width, moved back into the demo where it would go past an end
        const auto start = std::clamp(pivot - newSpan * mouseFraction, 0.0, maxTick - newSpan);
        const auto zoomedStart = static_cast<std::int32_t>(std::lround(start));
        return {zoomedStart, std::min(zoomedStart + static_cast<std::int32_t>(std::lround(newSpan)), maxTick)};
    }

    View Pan(View view, double ticks, std::int32_t maxTick)
    {
        const auto span = std::min(view.end - view.start, maxTick);
        if (span <= 0)
            return view;
        const auto start = static_cast<std::int32_t>(
            std::clamp(std::llround(view.start + ticks), 0ll, static_cast<long long>(maxTick - span)));
        return {start, start + span};
    }

    View Fit(std::int64_t start, std::int64_t end, double margin, std::int32_t minSpan, std::int32_t maxTick)
    {
        if (maxTick <= 0)
            return {0, 0};
        if (end < start)
            std::swap(start, end);
        const auto middle = (start + end) / 2.0;
        const auto span = std::clamp((end - start) * (1.0 + 2.0 * margin),
                                     static_cast<double>(std::min(minSpan, maxTick)), static_cast<double>(maxTick));
        const auto fitted = std::clamp(middle - span / 2.0, 0.0, maxTick - span);
        const auto fittedStart = static_cast<std::int32_t>(std::lround(fitted));
        return {fittedStart, std::min(fittedStart + static_cast<std::int32_t>(std::lround(span)), maxTick)};
    }

    void ForEachLine(std::uint32_t from, std::uint32_t to, std::uint32_t spacing,
                     const std::function<void(std::uint32_t tick)>& draw)
    {
        if (spacing == 0)
            return;
        // (wide, so stepping past the last tick doesn't wrap around)
        for (std::uint64_t tick = (from + static_cast<std::uint64_t>(spacing) - 1) / spacing * spacing; tick < to;
             tick += spacing)
            draw(static_cast<std::uint32_t>(tick));
    }

    RulerSpacing GetRulerSpacing(double ticksPerPixel, float minPixels)
    {
        // each with the minor spacing between its labels
        static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 17> STEPS = {{
            {10, 2},
            {20, 5},
            {50, 10},
            {100, 20},
            {200, 50},
            {500, 100},
            {1000, 200},
            {2000, 500},
            {5000, 1000},
            {10000, 2000},
            {15000, 5000},
            {30000, 5000},
            {60000, 10000},
            {120000, 30000},
            {300000, 60000},
            {600000, 120000},
            {1800000, 300000},
        }};
        for (const auto& [major, minor] : STEPS)
        {
            if (major / std::max(ticksPerPixel, 1e-9) >= minPixels)
                return {major, minor};
        }
        return {STEPS.back().first, STEPS.back().second};
    }

    std::string FormatTime(std::int64_t tick)
    {
        const auto sign = tick < 0 ? "-" : "";
        const auto hundredths = std::abs(tick) / 10;
        return std::format("{}{}:{:02}.{:02}", sign, hundredths / 6000, hundredths / 100 % 60, hundredths % 100);
    }

    std::string FormatRulerTime(std::uint32_t tick, std::uint32_t spacing)
    {
        const auto minutes = tick / 60000;
        const auto seconds = tick / 1000 % 60;
        const auto milliseconds = tick % 1000;
        if (spacing % 1000 == 0)
            return std::format("{}:{:02}", minutes, seconds);
        if (spacing % 100 == 0)
            return std::format("{}:{:02}.{}", minutes, seconds, milliseconds / 100);
        return std::format("{}:{:02}.{:02}", minutes, seconds, milliseconds / 10);
    }

    std::optional<std::uint32_t> ParseTime(std::string_view text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        if (text.empty())
            return std::nullopt;

        const auto parseNumber = [](std::string_view part) -> std::optional<double> {
            if (part.empty())
                return std::nullopt;
            double value = 0;
            const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
            if (error != std::errc() || end != part.data() + part.size() || value < 0)
                return std::nullopt;
            return value;
        };

        // ticks
        if (text.back() == 't')
        {
            const auto ticks = parseNumber(text.substr(0, text.size() - 1));
            if (!ticks || *ticks > std::numeric_limits<std::uint32_t>::max())
                return std::nullopt;
            return static_cast<std::uint32_t>(std::llround(*ticks));
        }

        // [hours:]minutes:seconds, or seconds
        double seconds = 0;
        std::size_t parts = 0;
        while (true)
        {
            const auto colon = text.find(':');
            const auto part = parseNumber(text.substr(0, colon));
            if (!part || (colon != std::string_view::npos && *part != std::floor(*part)) || ++parts > 3)
                return std::nullopt;
            seconds = seconds * 60 + *part;
            if (colon == std::string_view::npos)
                break;
            text.remove_prefix(colon + 1);
        }
        const auto ticks = seconds * 1000.0;
        if (ticks > std::numeric_limits<std::uint32_t>::max())
            return std::nullopt;
        return static_cast<std::uint32_t>(std::llround(ticks));
    }

    std::optional<std::size_t> Snap(std::int64_t tick, std::span<const std::int64_t> targets, double tolerance)
    {
        std::optional<std::size_t> nearest;
        double distance = tolerance;
        for (std::size_t i = 0; i < targets.size(); ++i)
        {
            const auto away = std::abs(static_cast<double>(targets[i] - tick));
            if (away <= distance && (!nearest || away < distance))
            {
                nearest = i;
                distance = away;
            }
        }
        return nearest;
    }

    Move SnapMove(std::span<const Span> items, std::int64_t delta, std::span<const std::int64_t> targets,
                  double tolerance, std::int64_t minTick, std::int64_t maxTick)
    {
        Move move{delta, std::nullopt};
        if (items.empty())
            return move;

        // the edge that comes nearest to a target
        double best = tolerance;
        for (const auto& item : items)
        {
            for (const auto edge : {item.start, item.end})
            {
                const auto target = Snap(edge + delta, targets, best);
                if (!target)
                    continue;
                const auto away = std::abs(static_cast<double>(targets[*target] - (edge + delta)));
                if (!move.target || away < best)
                {
                    best = away;
                    move.delta = targets[*target] - edge;
                    move.target = target;
                }
            }
        }

        // held within the timeline
        std::int64_t earliest = std::numeric_limits<std::int64_t>::max();
        std::int64_t latest = std::numeric_limits<std::int64_t>::min();
        for (const auto& item : items)
        {
            earliest = std::min(earliest, item.start);
            latest = std::max(latest, item.start);
        }
        const auto hold = [&](std::int64_t value) {
            return std::clamp(value, minTick - earliest, std::max(minTick - earliest, maxTick - latest));
        };
        // (a snap that would go past isn't one: as dragged instead)
        if (hold(move.delta) != move.delta)
            move = {hold(delta), std::nullopt};
        return move;
    }

    std::vector<std::size_t> InBox(std::span<const Box> items, Box box)
    {
        if (box.end < box.start)
            std::swap(box.start, box.end);
        if (box.bottom < box.top)
            std::swap(box.top, box.bottom);
        std::vector<std::size_t> inside;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const auto& item = items[i];
            if (item.start <= box.end && item.end >= box.start && item.top <= box.bottom && item.bottom >= box.top)
                inside.push_back(i);
        }
        return inside;
    }

    std::vector<std::size_t> InRange(std::span<const std::int64_t> ticks, std::int64_t from, std::int64_t to)
    {
        if (to < from)
            std::swap(from, to);
        std::vector<std::size_t> inside;
        for (std::size_t i = 0; i < ticks.size(); ++i)
        {
            if (ticks[i] >= from && ticks[i] <= to)
                inside.push_back(i);
        }
        return inside;
    }

    std::optional<PathPiece> PickPathBar(const PathBar& bar, float x, float tolerance)
    {
        using Kind = PathPiece::Kind;
        if (bar.end && std::abs(x - *bar.end) <= tolerance)
            return PathPiece{Kind::End, bar.endPoint};
        const PathBar::Tick* nearest = nullptr;
        for (const auto& tick : bar.ticks)
        {
            if (std::abs(x - tick.x) <= tolerance && (!nearest || std::abs(x - tick.x) < std::abs(x - nearest->x)))
                nearest = &tick;
        }
        if (nearest)
            return PathPiece{Kind::Point, nearest->point};
        for (const auto& wait : bar.waits)
        {
            if (x >= wait.x0 && x <= wait.x1)
                return PathPiece{Kind::Wait, wait.index};
        }
        for (const auto& stretch : bar.stretches)
        {
            if (x >= stretch.x0 && x <= stretch.x1)
                return PathPiece{Kind::Stretch, stretch.index};
        }
        return std::nullopt;
    }

    std::vector<std::size_t> GetGroupToggle(const std::vector<bool>& inGraph)
    {
        const bool all = std::ranges::all_of(inGraph, [](bool in) { return in; });
        std::vector<std::size_t> toggled;
        for (std::size_t i = 0; i < inGraph.size(); ++i)
        {
            if (all || !inGraph[i])
                toggled.push_back(i);
        }
        return toggled;
    }

    void SeparateTicks(std::vector<std::int64_t>& ticks, const std::vector<bool>& moved, int direction)
    {
        direction = direction < 0 ? -1 : 1;
        for (auto& tick : ticks)
            tick = std::max<std::int64_t>(tick, 0);
        const auto taken = [&](std::size_t i) {
            for (std::size_t j = 0; j < ticks.size(); ++j)
            {
                if (j != i && ticks[j] == ticks[i])
                    return true;
            }
            return false;
        };
        for (std::size_t i = 0; i < ticks.size() && i < moved.size(); ++i)
        {
            if (!moved[i])
                continue;
            auto step = direction;
            while (taken(i))
            {
                // (not before the start: the other way instead)
                if (ticks[i] + step < 0)
                    step = 1;
                ticks[i] += step;
            }
        }
    }

    float ValueAxis::ToY(double value) const
    {
        const auto span = max - min;
        if (span <= 0.0)
            return y0 + height / 2;
        return y0 + static_cast<float>((max - value) / span * height);
    }

    double ValueAxis::ToValue(float y) const
    {
        return max - (y - y0) / std::max(height, 1.0f) * (max - min);
    }

    double ValueAxis::ValuesPer(float pixels) const
    {
        return pixels / std::max(height, 1.0f) * (max - min);
    }

    std::pair<double, double> FitValues(double low, double high, double margin, double minSpan)
    {
        if (high < low)
            std::swap(low, high);
        const auto middle = (low + high) / 2;
        const auto span = std::max((high - low) * (1.0 + 2.0 * margin), minSpan);
        return {middle - span / 2, middle + span / 2};
    }

    std::pair<double, double> ZoomValues(double min, double max, float notches, double around)
    {
        const auto factor = std::pow(ZOOM_STEP, -static_cast<double>(notches));
        return {around + (min - around) * factor, around + (max - around) * factor};
    }

    double GetValueStep(double valuesPerPixel, float minPixels)
    {
        const auto smallest = std::max(std::abs(valuesPerPixel) * minPixels, 1e-6);
        auto power = std::pow(10.0, std::floor(std::log10(smallest)));
        for (;; power *= 10)
        {
            for (const auto multiple : {1.0, 2.0, 5.0})
            {
                if (power * multiple >= smallest * (1 - 1e-9))
                    return power * multiple;
            }
        }
    }

    std::string FormatValue(double value, double step)
    {
        const auto decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(step) - 1e-9)), 0, 6);
        // (no "-0")
        if (std::abs(value) < step * 1e-6)
            value = 0.0;
        return std::format("{:.{}f}", value, decimals);
    }
}  // namespace IWXMVM::UI::TimelineMath
