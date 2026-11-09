#pragma once
#include <imgui_internal.h>

#include "Types/Keyframe.hpp"
#include "UI/TimelineMath.hpp"

// The graph area under the timeline's tracks, in place of the curve editors each
// property had: the curves of the values chosen with the tracks' graph buttons in one area, fitted to them, with their
// keys to select and drag in time and value (one step a drag), and the values at the playhead to type. The timeline
// (Timeline.cpp) gives it where to draw and what its gestures need of the rest of the timeline.
namespace IWXMVM::UI::TimelineGraph
{
    bool IsOpen();
    // whether a property's values are in it
    bool Shows(Types::KeyframeablePropertyType type);
    // a track's graph button: its values in (all of them shown, but only the position of the campath's), or out
    void Toggle(Types::KeyframeablePropertyType type);
    void Close();
    // fits the values shown again when it is drawn next (keys were read from a file)
    void Refit();
    // nothing in it (another demo)
    void Clear();

    // What it needs of the timeline each frame
    struct Frame
    {
        TimelineMath::Axis axis;  // the lanes' ticks across
        float headerX = 0.0f;     // where the tracks' headers start
        ImRect rect;              // its area, headers and all
        std::uint32_t endTick = 0;
        // what a key dragged in time snaps to (but not what moves with it), and how close, in ticks; none when not
        // snapping
        std::function<std::vector<std::int64_t>()> getSnapTargets;
        double snapTolerance = 0.0;
        // Ctrl+wheel zooms the time, Shift+wheel and dragging with the middle button pan it
        std::function<void(TimelineMath::View view)> setView;
        // the timeline's menu of the selection, after a key was right-clicked (and selected)
        std::function<void(std::uint32_t tick)> openItemMenu;
        // the timeline's menus, which open in this area too
        std::function<void()> drawMenus;
    };
    void Draw(const Frame& frame);
    bool IsDragging();

    // One value of a property, in the graph
    struct Curve
    {
        Types::KeyframeablePropertyType type = Types::KeyframeablePropertyType::CampathCamera;
        std::uint32_t index = 0;
        bool shown = true;
    };
    const std::vector<Curve>& GetCurves();
    // its legend's swatch: shows or hides a value in it
    void SetShown(Types::KeyframeablePropertyType type, std::uint32_t index, bool shown);

    // For the harness: where its plot was drawn last, the values it shows up its height, and where a key of a value
    // is drawn (by the key's index)
    ImRect GetPlot();
    TimelineMath::ValueAxis GetValueAxis();
    std::optional<ImVec2> GetKeyPoint(Types::KeyframeablePropertyType type, std::uint32_t index, std::size_t key);
    // the legend's rows: "<property type>.<value index>" and their rectangles
    std::vector<std::pair<std::string, ImRect>> GetLegend();
}  // namespace IWXMVM::UI::TimelineGraph
