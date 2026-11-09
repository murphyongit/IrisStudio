#pragma once

#include "UI/Palettes.hpp"

// The editor's colors from the palette of the preferences (preferences.palette, Mono by default):
// ImGui's style, and the editor's own colors that were hard-coded (favorites, misfits, notes, problems, the
// playhead, selections, links, cards). Colors that tell kinds apart (gaits, curve channels, kills, the timeline's
// marker kinds, the view's mode chips) keep their hues, which no grey could replace.
namespace IWXMVM::UI::Theme
{
    // the palette of the preferences, the default for one that isn't one
    const Palettes::Palette& Current();

    ImVec4 Vec4(Palettes::Color color, float alpha = 1.0f);
    ImU32 U32(Palettes::Color color, float alpha = 1.0f);

    // The palette onto ImGui's colors. With Mono's light grey accent, what is selected or held is a lighter grey
    // than what is hovered, so it still stands out: hovers are a step of raised, selections a mix of it with the
    // accent that text stays readable on, and the accent itself goes to check marks, grabs and the playhead.
    void Apply();

    ImVec4 Favorite();  // a favorite's star
    ImVec4 Misfit();    // what doesn't fit an actor, and the Actors tab's warnings
    ImVec4 Note();      // a note's mark
    ImVec4 Problem();   // a key bound twice, Recording...
    ImVec4 Done();      // a step done
    ImU32 Playhead();
    ImU32 SelectionFill();  // a box dragged to select keys
    ImU32 SelectionEdge();
    ImU32 Card(float alpha = 1.0f);  // a card's or a toast's background
    ImVec4 Link(bool hovered);
    ImVec4 Keys();  // keys as the shortcuts list shows them
}  // namespace IWXMVM::UI::Theme
