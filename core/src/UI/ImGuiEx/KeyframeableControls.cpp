#include "StdInclude.hpp"
#include "KeyframeableControls.hpp"

#include "Components/KeyframeLogic.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/Playback.hpp"
#include "Components/UndoHistory.hpp"
#include "Configuration/Configuration.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Mod.hpp"
#include "Resources.hpp"
#include "UI/Components/Timeline.hpp"
#include "UI/Components/TimelineGraph.hpp"
#include "UI/TimelineMath.hpp"
#include "UI/Toasts.hpp"

namespace ImGuiEx::Keyframeable
{
    using namespace IWXMVM;
    using Components::KeyframeManager;

    namespace
    {
        // An edit going on in a control: named by the label of the control (the properties that controls of the
        // label change meanwhile are in it), and for each property it changed the key it keys (once there is one) and
        // whether that key's value change was begun
        struct EditedKey
        {
            std::optional<std::int32_t> key;
            bool modifying = false;
        };
        struct Edit
        {
            std::string label;
            std::map<Types::KeyframeablePropertyType, EditedKey> keys;
        };
        std::optional<Edit> editing;

        // the row whose control was changed last, until TakeChanged
        std::optional<std::string> changedRow;

        // where the rows' controls start, a fraction of the window's width (LabelColumn)
        float labelColumn = 0.4f;

        // FlashRow's row, and when it was asked for
        std::optional<Types::KeyframeablePropertyType> flashedRow;
        std::chrono::steady_clock::time_point rowFlashedAt;
        bool scrollToFlashedRow = false;

        std::map<std::string, Drawn> drawn;
        constexpr int RECENT_FRAMES = 3;

        std::uint32_t GetTick()
        {
            return Components::Playback::GetTimelineTick();
        }

        std::string GetTypeName(Types::KeyframeablePropertyType type)
        {
            return KeyframeManager::GetTypeName(type);
        }

        // a group's types, joined: "FilmtweakTintDark+FilmtweakTintMid+FilmtweakTintLight"
        std::string GetTypesName(std::span<const Types::KeyframeablePropertyType> types)
        {
            std::string name;
            for (const auto type : types)
                name += (name.empty() ? "" : "+") + GetTypeName(type);
            return name;
        }

        bool SameValue(const Types::KeyframeableProperty& property, const Types::KeyframeValue& a,
                       const Types::KeyframeValue& b)
        {
            for (std::int32_t i = 0; i < property.GetValueCount(); ++i)
            {
                if (a.GetByIndex(i) != b.GetByIndex(i))
                    return false;
            }
            return true;
        }

        void EndEdit()
        {
            if (!editing)
                return;
            auto& keyframeManager = KeyframeManager::Get();
            for (const auto& [type, edited] : editing->keys)
            {
                const auto& property = keyframeManager.GetProperty(type);
                auto& keys = keyframeManager.GetKeyframes(property);
                if (const auto key = std::ranges::find(keys, edited.key.value_or(-1), &Types::Keyframe::id);
                    edited.modifying && key != keys.end() && keyframeManager.IsKeyframeValueBeingModified(*key))
                    keyframeManager.EndModifyingKeyframeValue(property, *key);
            }
            Components::UndoHistory::Get().EndGroup();
            editing.reset();
        }

        // A key's interpolation and eases, for the menus of the diamonds: each change one step
        void DrawKeyOptions(const Types::KeyframeableProperty& property, const Types::Keyframe& key)
        {
            // (a switch's keys hold it until the next, and have no eases)
            if (KeyframeLogic::IsStepped(property.type))
            {
                ImGui::TextDisabled("On or off until the next key");
                return;
            }
            auto& keyframeManager = KeyframeManager::Get();
            const auto change = [&](const std::function<void(Types::Keyframe&)>& edit, std::string label) {
                auto changed = key;
                edit(changed);
                keyframeManager.ChangeKeyframes(property, {changed}, std::move(label));
            };
            ImGui::TextDisabled("Interpolation to the next key");
            // (Auto for keys that have it, and for campath nodes, which have it when they are added)
            const bool offerAuto = key.interpolation == Types::KeyInterpolation::Auto ||
                                   property.type == Types::KeyframeablePropertyType::CampathCamera;
            for (const auto interpolation : {Types::KeyInterpolation::Smooth, Types::KeyInterpolation::Linear,
                                             Types::KeyInterpolation::Hold, Types::KeyInterpolation::Auto})
            {
                if (interpolation == Types::KeyInterpolation::Auto && !offerAuto)
                    continue;
                const auto name = magic_enum::enum_name(interpolation);
                if (ImGui::MenuItem(std::string(name).c_str(), nullptr, key.interpolation == interpolation))
                    change([interpolation](Types::Keyframe& edited) { edited.interpolation = interpolation; },
                           std::format("make the key {}", name));
            }
            for (const bool in : {true, false})
            {
                const auto end = in ? "in" : "out";
                Ease(
                    in ? "Ease in" : "Ease out", in ? key.easeIn : key.easeOut, ImGui::GetFontSize() * 7.0f, 0.0f,
                    std::format("change the ease {} of the key", end),
                    [&](float amount, bool) {
                        change([&](Types::Keyframe& edited) { (in ? edited.easeIn : edited.easeOut) = amount; },
                               amount > 0.0f ? std::format("ease {} the key", end)
                                             : std::format("no ease {} on the key", end));
                    },
                    in ? "Slow down to rest arriving at the key" : "Start from rest leaving the key",
                    std::format("diamondmenu.ease{}", end), Record);
            }
        }

        // The ◇, ◈ or ◆ of a diamond in a square at min, and whether it is hovered
        void DrawDiamondShape(ImVec2 min, float size, KeyState state, bool hovered)
        {
            auto drawList = ImGui::GetWindowDrawList();
            if (hovered)
                drawList->AddRectFilled(min, min + ImVec2(size, size), ImGui::GetColorU32(ImGuiCol_FrameBgHovered),
                                        ImGui::GetStyle().FrameRounding);
            const auto center = ImVec2(std::floor(min.x + size / 2), std::floor(min.y + size / 2));
            const auto corners = [&](float radius) {
                return std::array{ImVec2(center.x, center.y - radius), ImVec2(center.x + radius, center.y),
                                  ImVec2(center.x, center.y + radius), ImVec2(center.x - radius, center.y)};
            };
            const auto radius = std::floor(size * 0.32f);
            const auto accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
            const auto outer = corners(radius);
            switch (state)
            {
                case KeyState::None:
                    drawList->AddQuad(outer[0], outer[1], outer[2], outer[3],
                                      ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled), 1.5f);
                    break;
                case KeyState::Animated:
                {
                    drawList->AddQuad(outer[0], outer[1], outer[2], outer[3], accent, 1.5f);
                    const auto inner = corners(std::max(std::floor(radius * 0.45f), 2.0f));
                    drawList->AddQuadFilled(inner[0], inner[1], inner[2], inner[3], accent);
                    break;
                }
                case KeyState::Here:
                    drawList->AddQuadFilled(outer[0], outer[1], outer[2], outer[3], accent);
                    break;
            }
        }

        // The diamond of a group row's properties: ◇ when none has keys, ◆ when all have a key at the playhead, ◈
        // otherwise. A click keys those without a key here, or deletes the keys here when all have one, as one step.
        void GroupDiamond(std::span<const Types::KeyframeableProperty* const> properties,
                          std::span<const Types::KeyframeValue> values, const char* label, float size,
                          const std::string& recordName)
        {
            auto& keyframeManager = KeyframeManager::Get();
            const auto tick = GetTick();
            bool anyKeyed = false, allHere = true;
            for (const auto property : properties)
            {
                const auto state = GetKeyState(*property);
                anyKeyed = anyKeyed || state != KeyState::None;
                allHere = allHere && state == KeyState::Here;
            }
            const auto state = allHere ? KeyState::Here : anyKeyed ? KeyState::Animated : KeyState::None;

            ImGui::PushID("diamond");
            const auto min = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##diamond", ImVec2(size, size));
            const bool hovered = ImGui::IsItemHovered();
            Record("diamond." + recordName);
            DrawDiamondShape(min, size, state, hovered);

            const auto time = UI::TimelineMath::FormatTime(tick);
            const auto keyThem = [&] {
                Components::UndoHistory::Get().BeginGroup(std::format("key {}", label));
                for (std::size_t i = 0; i < properties.size(); ++i)
                {
                    auto& keys = keyframeManager.GetKeyframes(*properties[i]);
                    if (std::ranges::find(keys, tick, &Types::Keyframe::tick) != keys.end())
                        continue;
                    keyframeManager.AddKeyframe(*properties[i], Types::Keyframe(*properties[i], tick, values[i]));
                    keyframeManager.SortAndSaveKeyframes(keys);
                }
                Components::UndoHistory::Get().EndGroup();
            };
            const auto deleteHere = [&] {
                Components::UndoHistory::Get().BeginGroup(std::format("delete the keys of {}", label));
                for (const auto property : properties)
                {
                    auto& keys = keyframeManager.GetKeyframes(*property);
                    if (const auto key = std::ranges::find(keys, tick, &Types::Keyframe::tick); key != keys.end())
                    {
                        keyframeManager.RemoveKeyframe(*property, key);
                        keyframeManager.SortAndSaveKeyframes(keys);
                    }
                }
                Components::UndoHistory::Get().EndGroup();
            };
            if (clicked)
            {
                EndEdit();
                if (state == KeyState::Here)
                {
                    deleteHere();
                    UI::Toasts::Show(std::format("Deleted the keys of {} at {}", label, time), true);
                }
                else
                {
                    keyThem();
                    UI::Toasts::Show(std::format("{} keyed at {}", label, time), true);
                }
            }
            if (hovered && !ImGui::IsPopupOpen("##diamondMenu"))
            {
                switch (state)
                {
                    case KeyState::None:
                        ImGui::SetTooltip("Click to key all at %s", time.c_str());
                        break;
                    case KeyState::Animated:
                        ImGui::SetTooltip("Animated \xC2\xB7 click to key all at %s", time.c_str());
                        break;
                    case KeyState::Here:
                        ImGui::SetTooltip("Keys at %s \xC2\xB7 click to delete them", time.c_str());
                        break;
                }
            }

            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("##diamondMenu");
            if (ImGui::BeginPopup("##diamondMenu"))
            {
                ImGui::TextDisabled("%s at %s", label, time.c_str());
                if (state != KeyState::Here && ImGui::MenuItem(ICON_FA_DIAMOND " Key them all here"))
                    keyThem();
                if (state != KeyState::None && ImGui::MenuItem(ICON_FA_TRASH " Delete their keys here"))
                    deleteHere();
                if (anyKeyed)
                {
                    ImGui::Separator();
                    std::optional<std::uint32_t> before, after;
                    std::vector<bool> inGraph;
                    for (const auto property : properties)
                    {
                        for (const auto& key : keyframeManager.GetKeyframes(*property))
                        {
                            if (key.tick < tick && (!before || key.tick > *before))
                                before = key.tick;
                            if (key.tick > tick && (!after || key.tick < *after))
                                after = key.tick;
                        }
                        inGraph.push_back(UI::TimelineGraph::Shows(property->type));
                    }
                    if (ImGui::MenuItem(ICON_FA_BACKWARD_STEP " Go to the key before", nullptr, false,
                                        before.has_value()))
                        UI::Seek(*before, false);
                    if (ImGui::MenuItem(ICON_FA_FORWARD_STEP " Go to the key after", nullptr, false, after.has_value()))
                        UI::Seek(*after, false);
                    const bool allInGraph = std::ranges::all_of(inGraph, std::identity{});
                    if (ImGui::MenuItem(ICON_FA_CHART_LINE " Show in the graph", nullptr, allInGraph))
                    {
                        for (const auto index : UI::TimelineMath::GetGroupToggle(inGraph))
                            UI::TimelineGraph::Toggle(properties[index]->type);
                    }
                    if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Delete all their keys"))
                    {
                        Components::UndoHistory::Get().BeginGroup(std::format("delete the keys of {}", label));
                        for (const auto property : properties)
                        {
                            keyframeManager.ClearKeyframes(*property);
                            keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(*property));
                        }
                        Components::UndoHistory::Get().EndGroup();
                        UI::Toasts::Show(std::format("Deleted the keys of {}", label), true);
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        // A row: the diamond (of its property, or of all of a group's), the label, and the control drawn by draw,
        // which changes the values in place and says whether it did. With keys, a property's value is theirs at the
        // playhead first, and a change keys it there, the edit named step (by the label when empty).
        bool DrawRow(const char* label, std::span<const Types::KeyframeablePropertyType> types,
                     std::span<Types::KeyframeValue> values, const std::function<bool(const char* id)>& draw,
                     const char* name, const std::function<std::string()>& step = {})
        {
            auto& keyframeManager = KeyframeManager::Get();
            const std::string shown = name ? name : label;
            std::vector<const Types::KeyframeableProperty*> properties;
            std::vector<bool> keyed;
            for (std::size_t i = 0; i < types.size(); ++i)
            {
                const auto& property = keyframeManager.GetProperty(types[i]);
                properties.push_back(&property);
                keyed.push_back(!keyframeManager.GetKeyframes(property).empty());
                if (keyed.back())
                    values[i] = keyframeManager.Interpolate(property, GetTick());
            }

            const auto typesName = GetTypesName(types);
            if (flashedRow && std::ranges::find(types, *flashedRow) != types.end())
            {
                const auto age = std::chrono::duration<float>(std::chrono::steady_clock::now() - rowFlashedAt).count();
                if (age < 0.6f)
                {
                    const auto min = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2(min.x - 4.0f, min.y),
                        ImVec2(min.x + ImGui::GetContentRegionAvail().x, min.y + ImGui::GetFrameHeight()),
                        ImGui::GetColorU32(ImGuiCol_HeaderActive, 1.0f - age / 0.6f));
                }
                else
                {
                    flashedRow.reset();
                }
                if (std::exchange(scrollToFlashedRow, false))
                    ImGui::SetScrollHereY(0.25f);
            }
            ImGui::PushID(typesName.c_str());
            const auto size = ImGui::GetFontSize() * 1.5f;
            if (types.size() == 1)
                Diamond(*properties.front(), values.front(), shown.c_str(), size);
            else
                GroupDiamond(properties, values, shown.c_str(), size, typesName);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            // (a label longer than its column, as ReShade's can be, ends with "..." and shows whole in a tooltip)
            const auto controlX = ImGui::GetWindowWidth() * labelColumn;
            const auto room = controlX - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x;
            const auto labelSize = ImGui::CalcTextSize(label);
            if (labelSize.x > room && room > 0.0f)
            {
                const auto at = ImGui::GetCursorScreenPos() + ImVec2(0.0f, ImGui::GetStyle().FramePadding.y);
                ImGui::Dummy(ImVec2(room, labelSize.y));
                ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), at, at + ImVec2(room, labelSize.y),
                                          at.x + room, at.x + room, label, nullptr, &labelSize);
                ImGui::SetItemTooltip("%s", label);
            }
            else
            {
                ImGui::TextUnformatted(label);
            }
            ImGui::SameLine();
            ImGui::SetCursorPosX(controlX);
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - labelColumn) - ImGui::GetStyle().WindowPadding.x);
            const std::vector<Types::KeyframeValue> before(values.begin(), values.end());
            const bool changed = draw("##control");
            Record("control." + typesName);
            if (changed)
            {
                changedRow = shown;
                for (std::size_t i = 0; i < types.size(); ++i)
                {
                    // (one property: a change keys it as it is, as the controls always did)
                    if (keyed[i] && (types.size() == 1 || !SameValue(*properties[i], before[i], values[i])))
                        KeyValueHere(*properties[i], values[i], shown.c_str(), step ? step() : std::string());
                }
            }
            ImGui::PopID();
            EndEditWhenIdle();
            return changed || std::ranges::any_of(keyed, std::identity{});
        }

        bool DrawRow(const char* label, Types::KeyframeablePropertyType type, Types::KeyframeValue& value,
                     const std::function<bool(const char* id)>& draw, const char* name,
                     const std::function<std::string()>& step = {})
        {
            return DrawRow(label, std::span(&type, 1), std::span(&value, 1), draw, name, step);
        }
    }  // namespace

    void FlashRow(Types::KeyframeablePropertyType propertyType)
    {
        flashedRow = propertyType;
        rowFlashedAt = std::chrono::steady_clock::now();
        scrollToFlashedRow = true;
    }

    bool PanelSection(const char* label, const std::string& name)
    {
        auto& closed = PreferencesConfiguration::Get().panelSectionsClosed;
        const auto found = std::ranges::find(closed, name);
        const bool wasOpen = found == closed.end();
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::SetNextItemOpen(wasOpen);
        const bool open = ImGui::CollapsingHeader(std::format("{}##panelSection{}", label, name).c_str());
        Record("section." + name);
        if (open != wasOpen)
        {
            if (open)
                closed.erase(found);
            else
                closed.push_back(name);
            Configuration::Get().Write(true);
        }
        return open;
    }

    LabelColumn::LabelColumn(float fraction) : before(labelColumn)
    {
        labelColumn = fraction;
    }

    LabelColumn::~LabelColumn()
    {
        labelColumn = before;
    }

    KeyState GetKeyState(const Types::KeyframeableProperty& property)
    {
        const auto& keys = KeyframeManager::Get().GetKeyframes(property);
        if (keys.empty())
            return KeyState::None;
        return std::ranges::find(keys, GetTick(), &Types::Keyframe::tick) != keys.end() ? KeyState::Here
                                                                                        : KeyState::Animated;
    }

    void Diamond(const Types::KeyframeableProperty& property, const Types::KeyframeValue& value, const char* label,
                 float size)
    {
        auto& keyframeManager = KeyframeManager::Get();
        auto& keys = keyframeManager.GetKeyframes(property);
        const auto tick = GetTick();
        const auto state = GetKeyState(property);
        const bool campath = property.type == Types::KeyframeablePropertyType::CampathCamera;
        const auto noun = campath ? "node" : "key";

        ImGui::PushID("diamond");
        const auto min = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##diamond", ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();
        Record("diamond." + GetTypeName(property.type));
        DrawDiamondShape(min, size, state, hovered);

        const auto time = UI::TimelineMath::FormatTime(tick);
        if (clicked)
        {
            EndEdit();
            if (state == KeyState::Here)
            {
                const auto key = std::ranges::find(keys, tick, &Types::Keyframe::tick);
                keyframeManager.RemoveKeyframe(property, key);
                keyframeManager.SortAndSaveKeyframes(keys);
                UI::Toasts::Show(std::format("Deleted the {} of {} at {}", noun, label, time), true);
            }
            else
            {
                keyframeManager.AddKeyframe(property, Types::Keyframe(property, tick, value));
                keyframeManager.SortAndSaveKeyframes(keys);
                UI::Toasts::Show(std::format("{} keyed at {}", label, time), true);
            }
        }
        if (hovered && !ImGui::IsPopupOpen("##diamondMenu"))
        {
            switch (state)
            {
                case KeyState::None:
                    ImGui::SetTooltip("Click to key at %s", time.c_str());
                    break;
                case KeyState::Animated:
                    ImGui::SetTooltip("Animated \xC2\xB7 click to key at %s", time.c_str());
                    break;
                case KeyState::Here:
                    ImGui::SetTooltip("%s at %s \xC2\xB7 click to delete", campath ? "Node" : "Key", time.c_str());
                    break;
            }
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##diamondMenu");
        if (ImGui::BeginPopup("##diamondMenu"))
        {
            ImGui::TextDisabled("%s at %s", label, time.c_str());
            const auto here = std::ranges::find(keys, tick, &Types::Keyframe::tick);
            if (here == keys.end())
            {
                if (ImGui::MenuItem(ICON_FA_DIAMOND " Key it here"))
                {
                    keyframeManager.AddKeyframe(property, Types::Keyframe(property, tick, value));
                    keyframeManager.SortAndSaveKeyframes(keys);
                }
            }
            else
            {
                if (ImGui::MenuItem(ICON_FA_TRASH " Delete the key here"))
                {
                    keyframeManager.RemoveKeyframe(property, here);
                    keyframeManager.SortAndSaveKeyframes(keys);
                }
                else
                {
                    ImGui::Separator();
                    DrawKeyOptions(property, *here);
                }
            }
            if (!keys.empty())
            {
                ImGui::Separator();
                const auto before = std::ranges::find_if(keys.rbegin(), keys.rend(),
                                                         [&](const Types::Keyframe& key) { return key.tick < tick; });
                if (ImGui::MenuItem(ICON_FA_BACKWARD_STEP " Go to the key before", nullptr, false,
                                    before != keys.rend()))
                    UI::Seek(before->tick, false);
                const auto after =
                    std::ranges::find_if(keys, [&](const Types::Keyframe& key) { return key.tick > tick; });
                if (ImGui::MenuItem(ICON_FA_FORWARD_STEP " Go to the key after", nullptr, false, after != keys.end()))
                    UI::Seek(after->tick, false);
                const bool inGraph = UI::TimelineGraph::Shows(property.type);
                if (ImGui::MenuItem(ICON_FA_CHART_LINE " Show in the graph", nullptr, inGraph))
                    UI::TimelineGraph::Toggle(property.type);
                if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Delete all its keys"))
                {
                    keyframeManager.ClearKeyframes(property);
                    keyframeManager.SortAndSaveKeyframes(keys);
                    UI::Toasts::Show(std::format("Deleted the keys of {}", label), true);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    void KeyValueHere(const Types::KeyframeableProperty& property, Types::KeyframeValue value, const char* label,
                      const std::string& step)
    {
        auto& keyframeManager = KeyframeManager::Get();
        auto& keys = keyframeManager.GetKeyframes(property);
        if (keys.empty())
            return;
        if (!editing || editing->label != label)
        {
            EndEdit();
            Components::UndoHistory::Get().BeginGroup(step.empty() ? std::format("change {}", label) : step);
            editing = Edit{label};
        }

        auto& edited = editing->keys[property.type];
        auto key = std::ranges::find(keys, edited.key.value_or(-1), &Types::Keyframe::id);
        if (key == keys.end())
            key = std::ranges::find(keys, GetTick(), &Types::Keyframe::tick);
        if (key == keys.end())
        {
            const Types::Keyframe added(property, GetTick(), value);
            keyframeManager.AddKeyframe(property, added);
            keyframeManager.SortAndSaveKeyframes(keys);
            edited.key = added.id;
            return;
        }
        edited.key = key->id;
        if (!edited.modifying)
        {
            keyframeManager.BeginModifyingKeyframeValue(*key);
            edited.modifying = true;
        }
        key->value = value;
        keyframeManager.SortAndSaveKeyframes(keys);
    }

    std::optional<std::string> TakeChanged()
    {
        return std::exchange(changedRow, std::nullopt);
    }

    namespace
    {
        // the amount of the ease slider being changed, while it is (an open group of the undo history)
        std::optional<float> easeEdit;
    }  // namespace

    void EndEditWhenIdle()
    {
        if (editing && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            EndEdit();
        // (an ease slider's menu that closed while it was changed)
        if (easeEdit && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            Components::UndoHistory::Get().EndGroup();
            easeEdit.reset();
        }
    }

    void Ease(const char* label, std::optional<float> amount, float width, float sliderX, const std::string& step,
              const std::function<void(float amount, bool ended)>& edit, const char* tooltip, const std::string& name,
              const std::function<void(const std::string& name)>& record)
    {
        ImGui::PushID(label);
        // (the sliders of "Ease in" and "Ease out" line up)
        const auto& style = ImGui::GetStyle();
        if (sliderX <= 0.0f)
            sliderX = ImGui::GetCursorPosX() + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                      ImGui::CalcTextSize("Ease out").x + style.ItemSpacing.x;
        bool eased = amount.value_or(0.0f) > 0.0f;
        ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, !amount);
        if (ImGui::Checkbox(label, &eased))
            edit(eased ? 1.0f : 0.0f, true);
        ImGui::PopItemFlag();
        record(name);
        ImGui::SetItemTooltip("%s", tooltip);

        ImGui::SameLine(sliderX);
        ImGui::SetNextItemWidth(width);
        float percent = amount.value_or(0.0f) * 100.0f;
        const bool changed = ImGui::SliderFloat("##amount", &percent, 0.0f, 100.0f, amount ? "%.0f%%" : "mixed",
                                                ImGuiSliderFlags_AlwaysClamp);
        record(name + ".amount");
        ImGui::SetItemTooltip("%s (Ctrl+click to type)", tooltip);
        if (changed)
        {
            if (!easeEdit)
                Components::UndoHistory::Get().BeginGroup(step);
            easeEdit = std::clamp(percent / 100.0f, 0.0f, 1.0f);
            edit(*easeEdit, false);
        }
        if (ImGui::IsItemDeactivated() && easeEdit)
        {
            edit(*easeEdit, true);
            Components::UndoHistory::Get().EndGroup();
            easeEdit.reset();
        }
        ImGui::PopID();
    }

    void Record(const std::string& name)
    {
        RecordRect(name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    void RecordRect(const std::string& name, ImVec2 min, ImVec2 max)
    {
        drawn[name] = {min, max, ImGui::GetFrameCount()};
    }

    std::vector<std::pair<std::string, Drawn>> GetDrawn()
    {
        std::vector<std::pair<std::string, Drawn>> recent;
        for (const auto& [name, each] : drawn)
        {
            // (the count starts again when the UI does, as the game resets its device: what was drawn before is old)
            const auto age = ImGui::GetFrameCount() - each.frame;
            if (age >= 0 && age <= RECENT_FRAMES)
                recent.emplace_back(name, each);
        }
        return recent;
    }

    bool SliderFloat(const char* label, float* v, float v_min, float v_max,
                     IWXMVM::Types::KeyframeablePropertyType propertyType, const char* format,
                     const DragSliderOptions& options, const char* name)
    {
        if (KeyframeManager::Get().GetProperty(propertyType).valueType != Types::KeyframeValueType::FloatingPoint)
            throw std::invalid_argument("Property is not a floating point value");

        Types::KeyframeValue value(*v);
        const bool result = DrawRow(
            label, propertyType, value,
            [&](const char* id) { return DragSlider(id, &value.floatingPoint, v_min, v_max, format, options); }, name);
        *v = value.floatingPoint;
        return result;
    }

    bool SliderFloat3(const char* label, float v[3], float v_min, float v_max,
                      IWXMVM::Types::KeyframeablePropertyType propertyType, const char* format,
                      std::optional<glm::vec3> resetValue, const char* name)
    {
        if (KeyframeManager::Get().GetProperty(propertyType).valueType != Types::KeyframeValueType::Vector3)
            throw std::invalid_argument("Property is not a vector3 value");

        Types::KeyframeValue value(glm::vec3(v[0], v[1], v[2]));
        const bool result = DrawRow(
            label, propertyType, value,
            [&](const char* id) {
                bool changed = false;
                ImGui::PushID(id);
                ImGui::PushMultiItemsWidths(3, ImGui::CalcItemWidth());
                for (int i = 0; i < 3; i++)
                {
                    if (i > 0)
                        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);

                    DragSliderOptions options;
                    if (resetValue.has_value())
                        options.resetValue = resetValue.value()[i];

                    ImGui::PushID(i);
                    changed = DragSlider("##component", &value.vector3[i], v_min, v_max, format, options) || changed;
                    Record(std::format("control.{}.{}", GetTypeName(propertyType), i));
                    ImGui::PopID();
                    ImGui::PopItemWidth();
                }
                ImGui::PopID();
                return changed;
            },
            name);
        v[0] = value.vector3.x;
        v[1] = value.vector3.y;
        v[2] = value.vector3.z;
        return result;
    }

    bool ColorEdit3(const char* label, float col[3], IWXMVM::Types::KeyframeablePropertyType propertyType,
                    std::optional<glm::vec3> resetValue, const char* name)
    {
        if (KeyframeManager::Get().GetProperty(propertyType).valueType != Types::KeyframeValueType::Vector3)
            throw std::invalid_argument("Property is not a vector3 value");

        Types::KeyframeValue value(glm::vec3(col[0], col[1], col[2]));
        const bool result = DrawRow(
            label, propertyType, value,
            [&](const char* id) {
                // the reset menu takes the right-click, which otherwise opens the display options
                bool changed = ImGui::ColorEdit3(id, glm::value_ptr(value.vector3),
                                                 resetValue.has_value() ? ImGuiColorEditFlags_NoOptions : 0);
                if (resetValue.has_value())
                {
                    const auto& reset = resetValue.value();
                    if (ResetMenu(std::format("{:.2f}, {:.2f}, {:.2f}", reset.x, reset.y, reset.z).c_str(),
                                  std::format("##{0}Reset", label).c_str()))
                    {
                        value.vector3 = reset;
                        changed = true;
                    }
                }
                return changed;
            },
            name);
        col[0] = value.vector3.x;
        col[1] = value.vector3.y;
        col[2] = value.vector3.z;
        return result;
    }

    bool Checkbox(const char* label, bool* v, IWXMVM::Types::KeyframeablePropertyType propertyType, const char* name)
    {
        if (!KeyframeLogic::IsStepped(propertyType))
            throw std::invalid_argument("Property is not a switch");

        Types::KeyframeValue value(*v ? 1.0f : 0.0f);
        bool checked = *v;
        const bool result = DrawRow(
            label, propertyType, value,
            [&](const char* id) {
                // (keys say whether it is on here)
                checked = KeyframeLogic::GetStepValue(value.floatingPoint) != 0.0f;
                if (!ImGui::Checkbox(id, &checked))
                    return false;
                value.floatingPoint = checked ? 1.0f : 0.0f;
                return true;
            },
            name, [&] { return std::format("turn {} {}", name, checked ? "on" : "off"); });
        *v = KeyframeLogic::GetStepValue(value.floatingPoint) != 0.0f;
        return result;
    }

    bool Row(const char* label, IWXMVM::Types::KeyframeablePropertyType propertyType,
             IWXMVM::Types::KeyframeValue& value,
             const std::function<bool(const char* id, IWXMVM::Types::KeyframeValue& value)>& draw, const char* name)
    {
        return DrawRow(label, propertyType, value, [&](const char* id) { return draw(id, value); }, name);
    }

    bool GroupRow(const char* label, std::span<const IWXMVM::Types::KeyframeablePropertyType> propertyTypes,
                  std::span<IWXMVM::Types::KeyframeValue> values, const std::function<bool(const char* id)>& draw,
                  const char* name)
    {
        if (propertyTypes.empty() || propertyTypes.size() != values.size())
            throw std::invalid_argument("A group row needs a value for each of its properties");
        return DrawRow(label, propertyTypes, values, draw, name);
    }
}  // namespace ImGuiEx::Keyframeable
