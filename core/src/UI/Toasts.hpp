#pragma once

// Short messages at the bottom of the view, for what deletes, adds out of sight or fails: "Campath cleared · Undo",
// "There's already a node at 0:41.20"
namespace IWXMVM::UI::Toasts
{
    // With undo, the message offers to undo the step the editor's undo history got last, while it is still the latest
    void Show(std::string text, bool undo = false);
    // With an action instead: a link that runs it once ("Too slow for Run · Walk?"), while the message shows
    void Show(std::string text, std::string action, std::function<void()> run);

    // Draws the messages centred at the bottom of a rectangle of the view, up from the bottom, into the current
    // window's draw list; with mouseOver, the mouse over the rectangle can click Undo (the view's window takes no
    // mouse input of its own, so the clicks are hit-tested here). Returns where each was drawn, as min and max.
    std::vector<std::pair<ImVec2, ImVec2>> Draw(ImVec2 min, ImVec2 max, bool mouseOver);

    // the messages shown now, latest last, and none (for the harness)
    std::vector<std::string> GetShown();
    void Clear();

    // "0:41.20": minutes, seconds and hundredths of a timeline tick
    std::string FormatTime(std::uint32_t tick);
}  // namespace IWXMVM::UI::Toasts
