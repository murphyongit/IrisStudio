#pragma once

#include "imgui_internal.h"

namespace ImGuiEx
{
    void DrawLine3D(glm::vec3 from, glm::vec3 to, ImVec4 color, float thickness);
    void DrawPoint3D(glm::vec3 point, ImVec4 color, ImVec2 size = ImVec2(3, 3));

    bool TimescaleSlider(const char* label, float* v, float v_min, float v_max, const char* format,
                         ImGuiSliderFlags flags);

    struct DragSliderOptions
    {
        std::optional<float> origin;      // where the bar fills from; 0 if the range spans it, v_min otherwise
        std::optional<float> resetValue;  // offered when right-clicking the slider
        ImGuiSliderFlags flags = ImGuiSliderFlags_None;
    };

    // Looks like a slider, with the bar filled from the origin to the value, but works like ImGui's drag widgets: a
    // click doesn't move the value, dragging across the whole bar covers the range (Alt drags slower, Shift faster),
    // and a double-click or Ctrl+click types an exact value.
    bool DragSlider(const char* label, float* v, float v_min, float v_max, const char* format = "%.2f",
                    const DragSliderOptions& options = {});

    // A right-click menu with a reset entry for the last item. Returns true when the entry is chosen. Groups such as
    // ColorEdit3 have no ID of their own, so they need a popupId.
    bool ResetMenu(const char* valueText, const char* popupId = nullptr);
}  // namespace ImGuiEx
