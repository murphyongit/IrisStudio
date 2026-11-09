#pragma once
#include "UI/LayoutMath.hpp"

#include <imgui_internal.h>

// Where the editor's windows go: the view at the top left, the side panel (the tabs) to its right and the timeline
// under both, sized by the splitters between them, which config.json keeps (see LayoutMath)
namespace IWXMVM::UI::Layout
{
    // Once a frame, before the layout's windows: where they go this frame, from the window, the font and the
    // splitters
    void Update();

    // where they go, in the window's coordinates (the timeline's is empty outside demos)
    ImRect GetView();
    ImRect GetPanel();
    ImRect GetTimeline();

    // After the layout's windows: the splitters on the edges between them (a drag sizes the panel or the timeline, a
    // double-click puts it back), and the layout's windows put behind all the others, which ImGui would otherwise
    // draw in the order they were made (they keep their place when clicked, ImGuiWindowFlags_NoBringToFrontOnFocus)
    void DrawSplitters();

    // the layouts the View menu offers
    enum class Preset
    {
        Default,
        Editing,   // a wide panel and a tall timeline
        Watching,  // the view as big as it gets
    };
    void Apply(Preset preset);

    LayoutMath::Sizes GetSizes();
    // the UI scale (1 for the font sized from the window's width), saved at once
    void SetScale(float scale);

    // for the harness: a splitter ("panel", "timeline") as drawn last
    std::optional<ImRect> GetSplitter(std::string_view name);
}  // namespace IWXMVM::UI::Layout
