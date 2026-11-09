#pragma once

namespace IWXMVM::UI
{
    // A strip of one play of a reload animation, from 0 at its left to 1 at its right, with the reload's moments as
    // markers to drag (the Actors tab's Animation page, see ActorsMenu): the stretch the magazine is off the gun (from
    // the first marker to the last), the parts of the play the clip doesn't show, and where the clip is at the
    // playhead. It only draws and tells what the mouse does; what that changes is up to the caller.
    struct ReloadStripMarker
    {
        std::string label;  // "out", "swap", "in"
        float time = 0.0f;
        ImU32 color = 0;
        bool own = false;  // the user's (drawn filled)
        std::string tooltip;
    };
    struct ReloadStripInput
    {
        std::vector<ReloadStripMarker> markers;  // in order
        float shownFrom = 0.0f;                  // the part of the play the clip shows
        float shownTo = 1.0f;
        std::optional<float> playhead;
    };
    struct ReloadStripResult
    {
        std::optional<std::size_t> dragged;  // the marker being dragged
        float time = 0.0f;                   // where to, from 0 to 1
        bool released = false;               // the drag ended this frame
        std::optional<float> scrubbed;       // the strip clicked or dragged off the markers: there
        std::optional<std::size_t> menu;     // the marker right-clicked
    };
    ReloadStripResult ReloadStrip(const char* id, const ReloadStripInput& input);

    // Where the reload controls were drawn, for the harness: the strip ("strip"), its markers (by label) and the
    // controls around it as the Animation page names them ("swap.playhead", "reset", "magazine", "magazine.off"...)
    struct ReloadControl
    {
        ImVec2 min;
        ImVec2 max;
        int frame = 0;  // ImGui's frame it was drawn in
    };
    // the item drawn last, or a rectangle
    void RecordReloadControl(const std::string& name);
    void RecordReloadControl(const std::string& name, ImVec2 min, ImVec2 max);
    // those drawn in the last few frames
    std::vector<std::pair<std::string, ReloadControl>> GetReloadControls();
}  // namespace IWXMVM::UI
