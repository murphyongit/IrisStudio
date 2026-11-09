#include "StdInclude.hpp"
#include "LayoutMath.hpp"

namespace IWXMVM::UI::LayoutMath
{
    namespace
    {
        // One part of a total wanted at a size, the rest going to the other: each at least its least while the total
        // has room for both, else shared in the proportion of their least
        float Split(float wanted, float least, float otherLeast, float total)
        {
            if (total <= 0.0f)
                return 0.0f;
            if (least + otherLeast > total)
                return std::round(total * least / (least + otherLeast));
            return std::round(std::clamp(wanted, least, total - otherLeast));
        }

        float SplitPanel(float pixels, float width, float fontSize)
        {
            return Split(pixels, MIN_PANEL * fontSize, MIN_VIEW_WIDTH * fontSize, width);
        }

        float SplitTimeline(float pixels, float height, float fontSize)
        {
            return Split(pixels, MIN_TIMELINE * fontSize, MIN_VIEW_HEIGHT * fontSize, height);
        }
    }  // namespace

    Layout Fit(float width, float height, float fontSize, const Sizes& sizes, bool timeline, bool graph)
    {
        Layout layout;
        layout.panelWidth = SplitPanel(sizes.panel * fontSize, width, fontSize);
        layout.viewWidth = std::max(width - layout.panelWidth, 0.0f);
        if (timeline)
            layout.timelineHeight =
                SplitTimeline((graph ? sizes.timelineWithGraph : sizes.timeline) * fontSize, height, fontSize);
        layout.viewHeight = std::max(height - layout.timelineHeight, 0.0f);
        return layout;
    }

    Sizes SetPanel(Sizes sizes, float pixels, float width, float fontSize)
    {
        if (fontSize > 0.0f)
            sizes.panel = SplitPanel(pixels, width, fontSize) / fontSize;
        return sizes;
    }

    Sizes SetTimeline(Sizes sizes, float pixels, float height, float fontSize, bool graph)
    {
        if (fontSize > 0.0f)
            (graph ? sizes.timelineWithGraph : sizes.timeline) = SplitTimeline(pixels, height, fontSize) / fontSize;
        return sizes;
    }

    std::pair<float, float> PlaceReadout(const Rect& picture, float textWidth, float textHeight, float fontSize,
                                         bool right)
    {
        // (each in its half of the picture, so the two don't run together in a small one)
        const auto half = picture.width / 2.0f - textWidth - fontSize / 2.0f;
        const auto in = std::max(std::min(fontSize * READOUT_IN, half), 0.0f);
        const auto x = right ? picture.x + picture.width - in - textWidth : picture.x + in;
        const auto y = picture.y + picture.height - fontSize * READOUT_UP - textHeight;
        return {std::max(std::min(x, picture.x + picture.width - textWidth), picture.x),
                std::max(std::min(y, picture.y + picture.height - textHeight), picture.y)};
    }

    float GetFontSize(float windowWidth, float scale)
    {
        return std::max(std::floor(windowWidth / 106.0f * ValidScale(scale)), 8.0f);
    }

    float StepScale(float scale, int steps)
    {
        scale = ValidScale(scale);
        // the offered scale nearest to it, then steps from there
        const auto nearest =
            std::ranges::min_element(SCALES, {}, [&](float offered) { return std::abs(offered - scale); });
        const auto index =
            std::clamp(static_cast<int>(nearest - SCALES.begin()) + steps, 0, static_cast<int>(SCALES.size()) - 1);
        return SCALES[static_cast<std::size_t>(index)];
    }

    float ValidScale(float scale)
    {
        if (!std::isfinite(scale))
            return 1.0f;
        return std::clamp(scale, SCALES.front(), SCALES.back());
    }
}  // namespace IWXMVM::UI::LayoutMath
