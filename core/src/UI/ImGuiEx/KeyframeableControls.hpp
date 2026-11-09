#pragma once

#include "imgui_internal.h"

#include "ImGuiExtensions.hpp"
#include "Types/Keyframe.hpp"

namespace ImGuiEx::Keyframeable
{
    // Rows of the Visuals tab: a diamond, the label and the control. The diamond
    // says whether the property is animated: ◇ not (a click keys it at the playhead), ◈ animated but no key at the
    // playhead (a click keys it there), ◆ a key at the playhead (a click deletes it); a right-click has its menu. A
    // control is never read-only: while the property has keys it shows their value at the playhead, and changing it
    // keys it there (the key there changes, or one is added), the whole drag one step to undo. They return whether
    // the value is to be applied: it changed, or keys set it. name names the property in steps and messages when the
    // label doesn't say enough ("Sun Brightness" for the Sun's "Brightness"; the label when null).
    bool SliderFloat(const char* label, float* v, float v_min, float v_max,
                     IWXMVM::Types::KeyframeablePropertyType propertyType, const char* format = "%.2f",
                     const DragSliderOptions& options = {}, const char* name = nullptr);
    bool SliderFloat3(const char* label, float* v, float v_min, float v_max,
                      IWXMVM::Types::KeyframeablePropertyType propertyType, const char* format = "%.1f",
                      std::optional<glm::vec3> resetValue = std::nullopt, const char* name = nullptr);
    bool ColorEdit3(const char* label, float col[3], IWXMVM::Types::KeyframeablePropertyType propertyType,
                    std::optional<glm::vec3> resetValue = std::nullopt, const char* name = nullptr);
    // A switch's checkbox (KeyframeLogic::IsStepped): its keys hold it on or off until the next. name is what it turns
    // on in steps and messages ("turn Filmtweaks on").
    bool Checkbox(const char* label, bool* v, IWXMVM::Types::KeyframeablePropertyType propertyType, const char* name);
    // The row of a property, when it's drawn next: scrolled to, flashing a moment (a key picked on the timeline)
    void FlashRow(IWXMVM::Types::KeyframeablePropertyType propertyType);
    // A section's band on the Camera and Record tabs: open at first, closed ones kept in config.json's
    // panelSectionsClosed by name; the harness clicks it as section.<name>. Says whether it's open.
    bool PanelSection(const char* label, const std::string& name);
    // A row of a control the ones above don't make (one of three values, the three moved together...): draw changes
    // value (all of the property's values, as a key holds them) in place and says whether it did
    bool Row(const char* label, IWXMVM::Types::KeyframeablePropertyType propertyType,
             IWXMVM::Types::KeyframeValue& value,
             const std::function<bool(const char* id, IWXMVM::Types::KeyframeValue& value)>& draw,
             const char* name = nullptr);
    // A row of a control that changes several properties at once (the tints of all three ranges): values are theirs
    // (at the playhead for those with keys) and draw changes them in place; a change keys those with keys, as one
    // step. Its diamond keys all of them at the playhead, or deletes their keys there when all have one; filled when
    // all have one there, with a dot when any has keys.
    bool GroupRow(const char* label, std::span<const IWXMVM::Types::KeyframeablePropertyType> propertyTypes,
                  std::span<IWXMVM::Types::KeyframeValue> values, const std::function<bool(const char* id)>& draw,
                  const char* name = nullptr);

    // The rows' controls start this far across the window (0.4 by default) while it lives: ReShade's rows, whose labels
    // are longer, give them more room
    struct LabelColumn
    {
        explicit LabelColumn(float fraction);
        ~LabelColumn();
        LabelColumn(const LabelColumn&) = delete;
        LabelColumn& operator=(const LabelColumn&) = delete;

       private:
        float before;
    };

    enum class KeyState
    {
        None,      // ◇ no keys
        Animated,  // ◈ keys, none at the playhead
        Here,      // ◆ a key at the playhead
    };
    KeyState GetKeyState(const IWXMVM::Types::KeyframeableProperty& property);

    // The diamond of a property, its click and its menu, at the cursor, size pixels square; value is what a key
    // added by a click gets (the control's value now) and label names the property in messages
    void Diamond(const IWXMVM::Types::KeyframeableProperty& property, const IWXMVM::Types::KeyframeValue& value,
                 const char* label, float size);

    // A value of a property changed in a control (all its values, as a key holds them): while the property has keys
    // that keys it at the playhead. The edit (a drag, typing) goes on as one step named step ("change <label>" when
    // empty), keying the same key even if the playhead moves meanwhile, until no control is active or a control of
    // another label changes one; the properties a control of the label changes meanwhile are in the same step.
    void KeyValueHere(const IWXMVM::Types::KeyframeableProperty& property, IWXMVM::Types::KeyframeValue value,
                      const char* label, const std::string& step = {});
    // ends an edit once no control is active; each frame
    void EndEditWhenIdle();

    // The name of the row whose control the user changed last since this was asked last (the Visuals tab names its
    // own step after it when what changed has no keys), nothing when none
    std::optional<std::string> TakeChanged();

    // How much a key (or the keys of a menu) eases in or out, 0 to 1: a tick with the label that eases it fully or
    // not at all, and a slider of percents, width wide, for how much (mixed for keys that differ), at sliderX in the
    // window (0: after the label, lined up for "Ease in" and "Ease out"). A change of the slider is one edit, one
    // step of the undo history named step until it is let go or the amount typed is entered: edit gets each amount,
    // and ended once the edit ends (a tick's change ends at once). record is called after each of the two items with
    // the names the harness clicks them by, name and name.amount.
    void Ease(const char* label, std::optional<float> amount, float width, float sliderX, const std::string& step,
              const std::function<void(float amount, bool ended)>& edit, const char* tooltip, const std::string& name,
              const std::function<void(const std::string& name)>& record);

    // Where the diamonds and controls were drawn, for the harness: "diamond.<property type>",
    // "control.<property type>" (with ".0" to ".6" for each value of those that have a control each); a group row's
    // are named after all its property types, joined by "+"
    struct Drawn
    {
        ImVec2 min;
        ImVec2 max;
        int frame = 0;  // ImGui's frame it was drawn in
    };
    // the item drawn last
    void Record(const std::string& name);
    // what was drawn there by hand
    void RecordRect(const std::string& name, ImVec2 min, ImVec2 max);
    // those drawn in the last few frames
    std::vector<std::pair<std::string, Drawn>> GetDrawn();
}  // namespace ImGuiEx::Keyframeable
