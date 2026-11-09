#include "StdInclude.hpp"
#include "TimelineGraph.hpp"

#include "UI/Theme.hpp"

#include <IconsFontAwesome6.h>
#include <set>

#include "Components/CaptureManager.hpp"
#include "Components/KeyframeLogic.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/Lights.hpp"
#include "Components/Props.hpp"
#include "Components/Playback.hpp"
#include "Components/UndoHistory.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/TimelineSelection.hpp"
#include "UI/Toasts.hpp"

namespace IWXMVM::UI::TimelineGraph
{
    namespace
    {
        using Components::KeyframeManager;
        using TimelineSelection::Item;

        // a press moves this far before it is a drag; a key is picked this close
        constexpr float DRAG_PIXELS = 3.0f;
        constexpr float PICK_PIXELS = 7.0f;
        constexpr float POINT_RADIUS = 4.5f;
        // the lines of values are at least this far apart
        constexpr float VALUE_LINE_PIXELS = 26.0f;
        constexpr double FIT_MARGIN = 0.08;
        constexpr std::int32_t MIN_SPAN = 100;

        constexpr ImU32 CAPTURE_COLOR = IM_COL32(225, 55, 55, 150);
        constexpr ImU32 FROZEN_COLOR = IM_COL32(255, 230, 0, 170);

        std::vector<Curve> curves;
        double low = 0.0;
        double high = 1.0;
        bool fitPending = true;

        // what was drawn last: the plot, its values, the keys (to pick them) and the legend's rows
        ImRect plot;
        TimelineMath::ValueAxis values;
        struct Point
        {
            Types::KeyframeablePropertyType type{};
            std::uint32_t index = 0;
            std::size_t key = 0;
            std::int32_t id = 0;
            std::uint32_t tick = 0;
            float value = 0.0f;
            ImVec2 position;
        };
        std::vector<Point> points;
        std::vector<std::pair<std::string, ImRect>> legend;

        // What the mouse does from a press until it is let go
        enum class Gesture
        {
            None,
            PressPoint,  // a click on a key, or a move once it drags
            Move,
            PressEmpty,  // a click on the background, or a box once it drags
            Box,
            Pan,
        };
        struct Drag
        {
            Gesture gesture = Gesture::None;
            ImGuiMouseButton button = ImGuiMouseButton_Left;
            ImVec2 press;
            double pressTick = 0.0;
            double pressValue = 0.0;
            // the key pressed, and which of its values
            Types::KeyframeablePropertyType type{};
            std::uint32_t index = 0;
            std::int32_t id = 0;
            // a click on one of several selected keys selects only it, unless it is dragged
            bool collapse = false;
            // the keys of its property that move, and their value when the move started; what time snaps to
            std::vector<std::pair<std::int32_t, float>> moved;
            std::vector<std::int64_t> targets;
            std::vector<TimelineMath::Span> spans;
            // the box adds to what was selected before it
            bool additive = false;
            std::set<Item> before;
            // panning: what showed when it started
            double low = 0.0;
            double high = 0.0;
            TimelineMath::View view;
        };
        Drag drag;

        // the key a double-click types a value of: the property, the value's index and the key's id
        std::optional<std::tuple<Types::KeyframeablePropertyType, std::uint32_t, std::int32_t>> typed;
        bool openTyped = false;
        constexpr auto TYPED_POPUP = "##graphTyped";
        constexpr auto GRAPH_MENU = "##graphMenu";

        const Types::KeyframeableProperty& PropertyOf(Types::KeyframeablePropertyType type)
        {
            return KeyframeManager::Get().GetProperty(type);
        }

        // "Campath X", "Filmtweak Brightness", and a value with a name of its own before its property's, which the
        // legend's narrow column may cut off: "G · Sun Light Color", "Shadows · Filmtweak Temperature Ranges"
        std::string GetName(const Curve& curve)
        {
            const auto& property = PropertyOf(curve.type);
            auto owned = Components::Lights::GetName(curve.type);
            if (!owned)
                owned = Components::Props::GetName(curve.type);
            auto name = owned.value_or(std::string(property.name));
            // (without the icon before it)
            if (const auto space = name.find(' ');
                space != std::string::npos && static_cast<unsigned char>(name[0]) > 127)
                name = name.substr(space + 1);
            if (curve.type == Types::KeyframeablePropertyType::CampathCamera)
                name = "Campath";
            if (property.valueType == Types::KeyframeValueType::FloatingPoint)
                return name;
            if (property.valueType == Types::KeyframeValueType::Vector3 && !property.valueNames[curve.index].empty())
                return std::format("{} \xC2\xB7 {}", property.valueNames[curve.index], name);
            return std::format("{} {}", name, Types::KeyframeValue::GetValueName(property, curve.index));
        }

        // A value's colour: the campath's position red, green and blue, its angles orange, purple and cyan, its field
        // of view yellow; a property's three values red, green and blue; one value white. Paler for the second
        // property shown and on.
        ImU32 GetColor(const Curve& curve)
        {
            static constexpr std::array<ImU32, 7> COLORS = {IM_COL32(235, 90, 90, 255),   IM_COL32(110, 200, 90, 255),
                                                            IM_COL32(90, 150, 245, 255),  IM_COL32(240, 165, 50, 255),
                                                            IM_COL32(195, 115, 235, 255), IM_COL32(70, 205, 205, 255),
                                                            IM_COL32(230, 220, 90, 255)};
            const auto valueType = PropertyOf(curve.type).valueType;
            auto color = valueType == Types::KeyframeValueType::FloatingPoint ? IM_COL32(225, 225, 225, 255)
                                                                              : COLORS[curve.index % COLORS.size()];
            std::vector<Types::KeyframeablePropertyType> order;
            for (const auto& each : curves)
            {
                if (std::ranges::find(order, each.type) == order.end())
                    order.push_back(each.type);
            }
            const auto paler = std::min<std::ptrdiff_t>(std::ranges::find(order, curve.type) - order.begin(), 2);
            if (paler > 0)
            {
                auto mixed = ImGui::ColorConvertU32ToFloat4(color);
                const auto amount = 0.3f * paler;
                mixed = ImVec4(mixed.x + (1 - mixed.x) * amount, mixed.y + (1 - mixed.y) * amount,
                               mixed.z + (1 - mixed.z) * amount, 1.0f);
                color = ImGui::ColorConvertFloat4ToU32(mixed);
            }
            return color;
        }

        // a value dragged or typed as a key of the property can have it: a switch's on or off, a list's item
        float ValueOf(Types::KeyframeablePropertyType type, double value)
        {
            return KeyframeLogic::IsStepped(type) ? KeyframeLogic::GetStepValue(type, static_cast<float>(value))
                                                  : static_cast<float>(value);
        }

        // the values of the keys of a property that the graph goes through (angles unwrapped)
        std::vector<float> GetKeyValues(const std::vector<Types::Keyframe>& keys, Types::KeyframeValueType valueType,
                                        std::uint32_t index)
        {
            return KeyframeLogic::GetValues(keys, index, KeyframeLogic::IsAngle(valueType, index));
        }

        // Fits the values shown to the curves where they show and the keys there; all of the keys when none show
        void Fit(const TimelineMath::View& view)
        {
            double lowest = std::numeric_limits<double>::max();
            double highest = std::numeric_limits<double>::lowest();
            const auto include = [&](double value) {
                lowest = std::min(lowest, value);
                highest = std::max(highest, value);
            };
            for (const auto& curve : curves)
            {
                const auto& property = PropertyOf(curve.type);
                const auto& keys = KeyframeManager::Get().GetKeyframes(property);
                if (!curve.shown || keys.empty())
                    continue;
                const auto keyValues = GetKeyValues(keys, property.valueType, curve.index);
                for (std::size_t i = 0; i < keys.size(); ++i)
                {
                    if (static_cast<std::int64_t>(keys[i].tick) >= view.start &&
                        static_cast<std::int64_t>(keys[i].tick) <= view.end)
                        include(keyValues[i]);
                }
                for (int i = 0; i <= 100; ++i)
                    include(KeyframeLogic::Evaluate(keys, keyValues, view.start + (view.end - view.start) * i / 100.0));
            }
            if (lowest > highest)
            {
                std::tie(low, high) = std::pair(0.0, 1.0);
                return;
            }
            const auto minSpan = std::max(0.1, std::max(std::abs(lowest), std::abs(highest)) * 0.05);
            std::tie(low, high) = TimelineMath::FitValues(lowest, highest, FIT_MARGIN, minSpan);
        }

        // the key drawn nearest the mouse within reach
        std::optional<std::size_t> FindPoint(ImVec2 mouse)
        {
            std::optional<std::size_t> nearest;
            float best = PICK_PIXELS;
            for (std::size_t i = 0; i < points.size(); ++i)
            {
                const auto distance = std::hypot(points[i].position.x - mouse.x, points[i].position.y - mouse.y);
                if (distance <= best)
                {
                    best = distance;
                    nearest = i;
                }
            }
            return nearest;
        }

        Types::Keyframe* FindKey(Types::KeyframeablePropertyType type, std::int32_t id)
        {
            auto& keys = KeyframeManager::Get().GetKeyframes(PropertyOf(type));
            const auto it = std::ranges::find(keys, id, &Types::Keyframe::id);
            return it != keys.end() ? &*it : nullptr;
        }

        void EndDrag()
        {
            if (drag.gesture == Gesture::Move)
            {
                TimelineSelection::EndMove();
                auto& keyframeManager = KeyframeManager::Get();
                for (const auto& [id, value] : drag.moved)
                {
                    if (const auto key = FindKey(drag.type, id);
                        key && keyframeManager.IsKeyframeValueBeingModified(*key))
                        keyframeManager.EndModifyingKeyframeValue(PropertyOf(drag.type), *key);
                }
                Components::UndoHistory::Get().EndGroup();
            }
            drag = {};
        }

        void DrawHeader(const Frame& frame)
        {
            const auto& style = ImGui::GetStyle();
            const auto right = frame.axis.x0 - 6;
            const auto top = frame.rect.Min.y + 2;
            ImGui::SetCursorScreenPos(ImVec2(frame.headerX, top + style.FramePadding.y));
            ImGui::TextUnformatted(ICON_FA_CHART_LINE " Graph");

            // Fit and close, at its right
            const auto buttonSize = ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
            ImGui::SetCursorScreenPos(ImVec2(right - buttonSize.x * 2 - style.ItemSpacing.x, top));
            if (ImGui::Button(ICON_FA_ARROWS_UP_DOWN "##fit", buttonSize))
                fitPending = true;
            ImGui::SetItemTooltip("Fit the values shown (the wheel zooms them, the middle button moves them)");
            legend.emplace_back("fit", ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_XMARK "##close", buttonSize))
                Close();
            ImGui::SetItemTooltip("Close the graph (a track's graph button opens it again)");
            legend.emplace_back("close", ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));

            // a row for each value: its colour (a click shows or hides it), its name and its value at the playhead,
            // which typing keys there
            const auto listTop = top + buttonSize.y + style.ItemSpacing.y;
            ImGui::SetCursorScreenPos(ImVec2(frame.headerX, listTop));
            ImGui::BeginChild(
                "##legend", ImVec2(std::max(right - frame.headerX, 1.0f), std::max(frame.rect.Max.y - listTop, 1.0f)));
            const auto tick = Components::Playback::GetTimelineTick();
            auto& keyframeManager = KeyframeManager::Get();
            for (std::size_t i = 0; i < curves.size(); ++i)
            {
                // (a copy: a swatch's click changes it)
                const auto curve = curves[i];
                const auto& property = PropertyOf(curve.type);
                const auto& keys = keyframeManager.GetKeyframes(property);
                const auto name = GetName(curve);
                ImGui::PushID(static_cast<int>(i));
                const auto rowMin = ImGui::GetCursorScreenPos();
                const auto swatch = ImGui::GetFrameHeight() * 0.6f;
                ImGui::SetCursorScreenPos(rowMin + ImVec2(0, (ImGui::GetFrameHeight() - swatch) / 2));
                if (ImGui::InvisibleButton("##swatch", ImVec2(swatch, swatch)))
                    SetShown(curve.type, curve.index, !curve.shown);
                const auto swatchRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                ImGui::SetItemTooltip(curve.shown ? "Hide it" : "Show it");
                const auto color = GetColor(curve);
                auto drawList = ImGui::GetWindowDrawList();
                if (curve.shown)
                    drawList->AddRectFilled(swatchRect.Min, swatchRect.Max, color, 2.0f);
                else
                    drawList->AddRect(swatchRect.Min, swatchRect.Max, color, 2.0f, 0, 1.5f);
                legend.emplace_back(std::format("{}.{}", KeyframeManager::GetTypeName(curve.type), curve.index), swatchRect);

                // the value at the playhead, at the right
                const auto valueWidth = ImGui::GetFontSize() * 4.5f;
                const auto valueX = ImGui::GetWindowContentRegionMax().x + ImGui::GetWindowPos().x - valueWidth;
                ImGui::SetCursorScreenPos(ImVec2(rowMin.x + swatch + style.ItemSpacing.x, rowMin.y));
                ImGui::AlignTextToFramePadding();
                ImGui::PushClipRect(rowMin, ImVec2(valueX - 4, rowMin.y + ImGui::GetFrameHeight()), true);
                ImGui::TextColored(
                    ImGui::ColorConvertU32ToFloat4(curve.shown ? ImGui::GetColorU32(ImGuiCol_Text)
                                                               : ImGui::GetColorU32(ImGuiCol_TextDisabled)),
                    "%s", name.c_str());
                ImGui::PopClipRect();
                // (all of it, which the column may cut off)
                ImGui::SetItemTooltip("%s", name.c_str());
                if (curve.type == Types::KeyframeablePropertyType::CampathCamera && keyframeManager.IsConstantSpeed())
                    ImGui::SetItemTooltip(
                        "The campath goes at constant speed: the graph shows its shape by the "
                        "nodes' times, the camera passes the nodes when it gets there");
                ImGui::SetCursorScreenPos(ImVec2(valueX, rowMin.y));
                ImGui::SetNextItemWidth(valueWidth);
                if (keys.empty())
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("no keys");
                }
                else
                {
                    auto value = keyframeManager.Interpolate(property, tick);
                    auto edited = value.GetByIndex(curve.index);
                    const auto speed = static_cast<float>(std::max(values.ValuesPer(1.0f), 1e-4));
                    if (ImGui::DragFloat("##value", &edited, speed, 0.0f, 0.0f, "%.2f"))
                    {
                        value.SetByIndex(curve.index, ValueOf(curve.type, edited));
                        ImGuiEx::Keyframeable::KeyValueHere(property, value, name.c_str());
                    }
                    ImGui::SetItemTooltip("%s at the playhead; drag or double-click to change it, which keys it there",
                                          name.c_str());
                    ImGuiEx::Keyframeable::EndEditWhenIdle();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }

        void DrawPlot(const Frame& frame, std::optional<std::size_t> hovered)
        {
            auto drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(plot.Min, plot.Max, ImGui::GetColorU32(ImGuiCol_FrameBg),
                                    ImGui::GetStyle().FrameRounding);
            drawList->PushClipRect(plot.Min, plot.Max, true);

            // lines of values, labelled at the left, and of the ruler's times
            const auto step = TimelineMath::GetValueStep(values.ValuesPer(1.0f), VALUE_LINE_PIXELS);
            const auto labelSize = ImGui::GetFontSize() * 0.75f;
            int lines = 0;
            for (auto value = std::ceil(values.min / step) * step; value <= values.max && lines < 200;
                 value += step, ++lines)
            {
                const auto y = std::floor(values.ToY(value));
                const bool zero = std::abs(value) < step * 1e-6;
                drawList->AddLine(ImVec2(plot.Min.x, y), ImVec2(plot.Max.x, y),
                                  zero ? IM_COL32(255, 255, 255, 45) : IM_COL32(255, 255, 255, 16));
                // (above its line, where that isn't cut off)
                if (y - labelSize - 1 >= plot.Min.y)
                    drawList->AddText(ImGui::GetFont(), labelSize, ImVec2(plot.Min.x + 3, y - labelSize - 1),
                                      ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                      TimelineMath::FormatValue(value, step).c_str());
            }
            const auto spacing = TimelineMath::GetRulerSpacing(frame.axis.TicksPer(1.0f),
                                                               ImGui::CalcTextSize("0:00.00").x + ImGui::GetFontSize());
            TimelineMath::ForEachLine(static_cast<std::uint32_t>(std::max(frame.axis.view.start, 0)),
                                      static_cast<std::uint32_t>(std::max(frame.axis.view.end, 0)) + 1, spacing.major,
                                      [&](std::uint32_t tick) {
                                          const auto x = std::floor(frame.axis.ToX(tick));
                                          drawList->AddLine(ImVec2(x, plot.Min.y), ImVec2(x, plot.Max.y),
                                                            IM_COL32(255, 255, 255, 14));
                                      });

            // the capture range's ends, the frozen tick and the playhead
            const auto line = [&](double tick, ImU32 color, float thickness) {
                const auto x = std::floor(frame.axis.ToX(tick));
                drawList->AddLine(ImVec2(x, plot.Min.y), ImVec2(x, plot.Max.y), color, thickness);
            };
            const auto& capture = Components::CaptureManager::Get().GetCaptureSettings();
            line(capture.startTick, CAPTURE_COLOR, 1.0f);
            line(capture.endTick, CAPTURE_COLOR, 1.0f);
            if (const auto frozen = Components::Playback::GetFrozenTick())
                line(*frozen, FROZEN_COLOR, 1.0f);
            line(Components::Playback::GetTimelineTick(), Theme::Playhead(), 2.0f);

            // the curves, by the keys' own times (the campath's shape even at constant speed), every other pixel
            auto& keyframeManager = KeyframeManager::Get();
            for (const auto& curve : curves)
            {
                const auto& property = PropertyOf(curve.type);
                const auto& keys = keyframeManager.GetKeyframes(property);
                if (!curve.shown || keys.empty())
                    continue;
                const auto keyValues = GetKeyValues(keys, property.valueType, curve.index);
                std::vector<ImVec2> polyline;
                for (auto x = plot.Min.x; x <= plot.Max.x + 2; x += 2.0f)
                {
                    const auto value = KeyframeLogic::Evaluate(keys, keyValues, frame.axis.ToTick(x));
                    polyline.emplace_back(x, values.ToY(value));
                }
                drawList->AddPolyline(polyline.data(), static_cast<int>(polyline.size()), GetColor(curve), 0, 2.0f);
            }

            // and their keys
            for (std::size_t i = 0; i < points.size(); ++i)
            {
                const auto& point = points[i];
                const Curve* curve = nullptr;
                for (const auto& each : curves)
                {
                    if (each.type == point.type && each.index == point.index)
                        curve = &each;
                }
                const bool selected = TimelineSelection::IsSelected(TimelineSelection::KeyItem(point.type, point.id));
                const auto fill = selected       ? ImGui::GetColorU32(ImGuiCol_CheckMark)
                                  : hovered == i ? IM_COL32_WHITE
                                                 : (curve ? GetColor(*curve) : IM_COL32_WHITE);
                const auto& center = point.position;
                const auto radius = POINT_RADIUS;
                const ImVec2 top(center.x, center.y - radius), right(center.x + radius, center.y),
                    bottom(center.x, center.y + radius), left(center.x - radius, center.y);
                drawList->AddQuadFilled(top, right, bottom, left, fill);
                drawList->AddQuad(top, right, bottom, left, selected ? IM_COL32_WHITE : IM_COL32(0, 0, 0, 160), 1.0f);
            }

            if (drag.gesture == Gesture::Box)
            {
                const auto mouse = ImGui::GetMousePos();
                const ImRect box(ImMin(drag.press, mouse), ImMax(drag.press, mouse));
                drawList->AddRectFilled(box.Min, box.Max, Theme::SelectionFill());
                drawList->AddRect(box.Min, box.Max, Theme::SelectionEdge());
            }
            drawList->PopClipRect();
        }

        // Where each key of the values shown is drawn
        void PlacePoints(const Frame& frame)
        {
            points.clear();
            auto& keyframeManager = KeyframeManager::Get();
            for (const auto& curve : curves)
            {
                if (!curve.shown)
                    continue;
                const auto& property = PropertyOf(curve.type);
                const auto& keys = keyframeManager.GetKeyframes(property);
                const auto keyValues = GetKeyValues(keys, property.valueType, curve.index);
                for (std::size_t k = 0; k < keys.size(); ++k)
                {
                    const ImVec2 position(std::floor(frame.axis.ToX(keys[k].tick)),
                                          std::floor(values.ToY(keyValues[k])));
                    if (position.x < plot.Min.x - POINT_RADIUS || position.x > plot.Max.x + POINT_RADIUS ||
                        position.y < plot.Min.y - POINT_RADIUS || position.y > plot.Max.y + POINT_RADIUS)
                        continue;
                    points.push_back({curve.type, curve.index, k, keys[k].id, keys[k].tick, keyValues[k], position});
                }
            }
        }

        // A double-click on the background: a key of the value whose curve is nearest, with the value there
        void AddKeyAt(const Frame& frame, ImVec2 mouse)
        {
            const auto tick = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(std::llround(frame.axis.ToTick(mouse.x)), 0, frame.endTick));
            const Curve* nearest = nullptr;
            float best = std::numeric_limits<float>::max();
            auto& keyframeManager = KeyframeManager::Get();
            for (const auto& curve : curves)
            {
                if (!curve.shown)
                    continue;
                const auto& property = PropertyOf(curve.type);
                const auto& keys = keyframeManager.GetKeyframes(property);
                const auto value =
                    keys.empty() ? 0.0f
                                 : KeyframeLogic::Evaluate(keys, GetKeyValues(keys, property.valueType, curve.index),
                                                           static_cast<double>(tick));
                const auto distance = std::abs(values.ToY(value) - mouse.y);
                if (distance < best)
                {
                    best = distance;
                    nearest = &curve;
                }
            }
            if (!nearest)
                return;
            const auto& property = PropertyOf(nearest->type);
            auto& keys = keyframeManager.GetKeyframes(property);
            const bool campath = property.type == Types::KeyframeablePropertyType::CampathCamera;
            if (std::ranges::find(keys, tick, &Types::Keyframe::tick) != keys.end())
            {
                Toasts::Show(
                    std::format("There's already a {} at {}", campath ? "node" : "key", Toasts::FormatTime(tick)));
                return;
            }
            auto value = keyframeManager.GetValueAt(property, tick);
            value.SetByIndex(nearest->index, ValueOf(property.type, values.ToValue(mouse.y)));
            const Types::Keyframe key(property, tick, value);
            keyframeManager.AddKeyframe(property, key);
            keyframeManager.SortAndSaveKeyframes(keys);
            const auto item = TimelineSelection::KeyItem(property.type, key.id);
            TimelineSelection::Set(std::span(&item, 1));
            Toasts::Show(std::format("{} added at {}", campath ? "Node" : "Key", Toasts::FormatTime(tick)), true);
        }

        void Press(const Frame& frame, std::optional<std::size_t> hovered)
        {
            auto& io = ImGui::GetIO();
            drag = {};
            drag.press = io.MousePos;
            drag.pressTick = frame.axis.ToTick(io.MousePos.x);
            drag.pressValue = values.ToValue(io.MousePos.y);
            if (hovered)
            {
                const auto& point = points[*hovered];
                drag.gesture = Gesture::PressPoint;
                drag.type = point.type;
                drag.index = point.index;
                drag.id = point.id;
                const auto item = TimelineSelection::KeyItem(point.type, point.id);
                if (io.KeyMods == ImGuiMod_Ctrl)
                    TimelineSelection::Toggle(item);
                else if (io.KeyMods == ImGuiMod_Shift)
                    TimelineSelection::Add(std::span(&item, 1));
                else if (!TimelineSelection::IsSelected(item))
                    TimelineSelection::Set(std::span(&item, 1));
                else
                    drag.collapse = TimelineSelection::Get().size() > 1;
                TimelineSelection::SetAnchor(item);
                return;
            }
            if (io.KeyAlt)
            {
                drag.gesture = Gesture::Pan;
                drag.low = low;
                drag.high = high;
                drag.view = frame.axis.view;
                return;
            }
            drag.gesture = Gesture::PressEmpty;
            drag.additive = io.KeyCtrl || io.KeyShift;
            drag.before = TimelineSelection::Get();
        }

        void BeginMove(const Frame& frame)
        {
            const auto item = TimelineSelection::KeyItem(drag.type, drag.id);
            if (!TimelineSelection::IsSelected(item))
                TimelineSelection::Set(std::span(&item, 1));
            const std::vector<Item> selected(TimelineSelection::Get().begin(), TimelineSelection::Get().end());
            Components::UndoHistory::Get().BeginGroup("move " + TimelineSelection::Describe(selected));
            drag.gesture = Gesture::Move;
            drag.targets = frame.getSnapTargets ? frame.getSnapTargets() : std::vector<std::int64_t>();
            TimelineSelection::BeginMove();
            drag.spans = TimelineSelection::GetMovingSpans();
            auto& keyframeManager = KeyframeManager::Get();
            for (auto& key : keyframeManager.GetKeyframes(PropertyOf(drag.type)))
            {
                if (!TimelineSelection::IsSelected(TimelineSelection::KeyItem(drag.type, key.id)))
                    continue;
                keyframeManager.BeginModifyingKeyframeValue(key);
                drag.moved.emplace_back(key.id, key.value.GetByIndex(drag.index));
            }
        }

        void UpdateDrag(const Frame& frame)
        {
            auto& io = ImGui::GetIO();
            const auto mouse = io.MousePos;
            const auto moved =
                std::abs(mouse.x - drag.press.x) > DRAG_PIXELS || std::abs(mouse.y - drag.press.y) > DRAG_PIXELS;
            switch (drag.gesture)
            {
                case Gesture::PressPoint:
                    if (!moved)
                        break;
                    BeginMove(frame);
                    [[fallthrough]];
                case Gesture::Move:
                {
                    // Shift keeps it to time or value, whichever it was dragged more along
                    const bool alongTime = std::abs(mouse.x - drag.press.x) >= std::abs(mouse.y - drag.press.y);
                    const bool timeMoves = !io.KeyShift || alongTime;
                    const bool valueMoves = !io.KeyShift || !alongTime;
                    const auto raw = timeMoves ? std::llround(frame.axis.ToTick(mouse.x) - drag.pressTick) : 0ll;
                    const auto move = TimelineMath::SnapMove(
                        drag.spans, raw,
                        io.KeyAlt ? std::span<const std::int64_t>() : std::span<const std::int64_t>(drag.targets),
                        frame.snapTolerance, 0, frame.endTick);
                    TimelineSelection::UpdateMove(move.delta);
                    const auto valueDelta = valueMoves ? values.ToValue(mouse.y) - drag.pressValue : 0.0;
                    auto& keyframeManager = KeyframeManager::Get();
                    auto& keys = keyframeManager.GetKeyframes(PropertyOf(drag.type));
                    std::optional<float> pressedValue;
                    for (const auto& [id, value] : drag.moved)
                    {
                        if (const auto key = FindKey(drag.type, id))
                        {
                            key->value.SetByIndex(drag.index, ValueOf(drag.type, value + valueDelta));
                            if (id == drag.id)
                                pressedValue = key->value.GetByIndex(drag.index);
                        }
                    }
                    keyframeManager.SortAndSaveKeyframes(keys);
                    ImGui::SetTooltip(
                        "%s%.3f s%s", move.delta < 0 ? "-" : "+", std::abs(move.delta) / 1000.0,
                        pressedValue ? std::format(", {:.2f} ({:+.2f})", *pressedValue, valueDelta).c_str() : "");
                    break;
                }
                case Gesture::PressEmpty:
                    if (moved)
                    {
                        drag.gesture = Gesture::Box;
                        UpdateDrag(frame);
                    }
                    break;
                case Gesture::Box:
                {
                    const ImRect box(ImMin(drag.press, mouse), ImMax(drag.press, mouse));
                    std::set<Item> selection = drag.additive ? drag.before : std::set<Item>();
                    for (const auto& point : points)
                    {
                        if (box.Contains(point.position))
                            selection.insert(TimelineSelection::KeyItem(point.type, point.id));
                    }
                    TimelineSelection::Set(std::vector<Item>(selection.begin(), selection.end()));
                    break;
                }
                case Gesture::Pan:
                {
                    frame.setView(TimelineMath::Pan(drag.view, -frame.axis.TicksPer(mouse.x - drag.press.x),
                                                    static_cast<std::int32_t>(frame.endTick)));
                    const auto delta = values.ValuesPer(mouse.y - drag.press.y);
                    low = drag.low + delta;
                    high = drag.high + delta;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    break;
                }
                case Gesture::None:
                    break;
            }
        }

        void Release()
        {
            switch (drag.gesture)
            {
                case Gesture::PressPoint:
                    if (drag.collapse)
                    {
                        const auto item = TimelineSelection::KeyItem(drag.type, drag.id);
                        TimelineSelection::Set(std::span(&item, 1));
                    }
                    break;
                case Gesture::PressEmpty:
                    if (!drag.additive)
                        TimelineSelection::Clear();
                    break;
                default:
                    break;
            }
            EndDrag();
        }

        void HandleInput(const Frame& frame, std::optional<std::size_t> hovered, bool plotHovered)
        {
            auto& io = ImGui::GetIO();
            const auto mouse = io.MousePos;

            // the wheel zooms the values at the mouse; with Ctrl it zooms the time, with Shift it pans it
            if (plotHovered && io.MouseWheel != 0.0f && !ImGui::IsAnyItemActive())
            {
                const auto& view = frame.axis.view;
                if (io.KeyCtrl)
                    frame.setView(TimelineMath::Zoom(view, io.MouseWheel, (mouse.x - frame.axis.x0) / frame.axis.width,
                                                     MIN_SPAN, static_cast<std::int32_t>(frame.endTick)));
                else if (io.KeyShift)
                    frame.setView(TimelineMath::Pan(view, -io.MouseWheel * 0.1 * (view.end - view.start),
                                                    static_cast<std::int32_t>(frame.endTick)));
                else
                    std::tie(low, high) = TimelineMath::ZoomValues(low, high, io.MouseWheel, values.ToValue(mouse.y));
            }

            if (hovered && drag.gesture == Gesture::None && !ImGui::IsAnyMouseDown())
            {
                const auto& point = points[*hovered];
                const auto key = FindKey(point.type, point.id);
                const auto& property = PropertyOf(point.type);
                const bool campath = point.type == Types::KeyframeablePropertyType::CampathCamera;
                std::string passing;
                if (campath && KeyframeManager::Get().IsConstantSpeed())
                    passing = std::format("\nAt constant speed the camera passes it at {}",
                                          TimelineMath::FormatTime(
                                              std::llround(KeyframeManager::Get().GetCampathPassingTick(point.tick))));
                ImGui::SetTooltip(
                    "%s of the %s at %s: %.2f, %s%s\nDrag to move it in time and value (Shift: only one, "
                    "Alt: without snapping), double-click to type its value, right-click for more",
                    std::string(Types::KeyframeValue::GetValueName(property, point.index)).c_str(),
                    campath ? "node" : "key", TimelineMath::FormatTime(point.tick).c_str(), point.value,
                    key ? std::string(magic_enum::enum_name(key->interpolation)).c_str() : "", passing.c_str());
            }

            if (plotHovered && drag.gesture == Gesture::None)
            {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
                {
                    drag = {};
                    drag.gesture = Gesture::Pan;
                    drag.button = ImGuiMouseButton_Middle;
                    drag.press = mouse;
                    drag.low = low;
                    drag.high = high;
                    drag.view = frame.axis.view;
                }
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    Press(frame, hovered);
                }
                else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                {
                    if (hovered)
                    {
                        const auto& point = points[*hovered];
                        const auto item = TimelineSelection::KeyItem(point.type, point.id);
                        if (!TimelineSelection::IsSelected(item))
                            TimelineSelection::Set(std::span(&item, 1));
                        TimelineSelection::SetAnchor(item);
                        frame.openItemMenu(point.tick);
                    }
                    else
                    {
                        ImGui::OpenPopup(GRAPH_MENU);
                    }
                }
            }

            if (plotHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                // (the press of a double-click's second click picked what is under it already)
                drag = {};
                if (hovered)
                {
                    const auto& point = points[*hovered];
                    typed = std::tuple(point.type, point.index, point.id);
                    openTyped = true;
                }
                else
                {
                    AddKeyAt(frame, mouse);
                }
            }

            if (drag.gesture != Gesture::None && ImGui::IsMouseDown(drag.button))
                UpdateDrag(frame);
            if (drag.gesture != Gesture::None && !ImGui::IsMouseDown(drag.button))
                Release();
        }

        void DrawPopups()
        {
            auto& keyframeManager = KeyframeManager::Get();
            if (openTyped)
            {
                openTyped = false;
                ImGui::OpenPopup(TYPED_POPUP);
            }
            if (ImGui::BeginPopup(TYPED_POPUP))
            {
                const auto key = typed ? FindKey(std::get<0>(*typed), std::get<2>(*typed)) : nullptr;
                if (!key)
                {
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    const auto& [type, index, id] = *typed;
                    const auto& property = PropertyOf(type);
                    ImGui::TextDisabled("%s at %s", GetName({type, index}).c_str(),
                                        TimelineMath::FormatTime(key->tick).c_str());
                    auto edited = key->value.GetByIndex(index);
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
                    if (ImGui::IsWindowAppearing())
                        ImGui::SetKeyboardFocusHere();
                    if (ImGui::DragFloat("##value", &edited, static_cast<float>(std::max(values.ValuesPer(1.0f), 1e-4)),
                                         0.0f, 0.0f, "%.3f"))
                    {
                        if (!keyframeManager.IsKeyframeValueBeingModified(*key))
                            keyframeManager.BeginModifyingKeyframeValue(*key);
                        key->value.SetByIndex(index, ValueOf(type, edited));
                        keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
                    }
                    else if (!ImGui::IsItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                             keyframeManager.IsKeyframeValueBeingModified(*key))
                    {
                        keyframeManager.EndModifyingKeyframeValue(property, *key);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
                        ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            else if (typed)
            {
                // closed while typing: what was typed is one step
                if (const auto key = FindKey(std::get<0>(*typed), std::get<2>(*typed));
                    key && keyframeManager.IsKeyframeValueBeingModified(*key))
                    keyframeManager.EndModifyingKeyframeValue(PropertyOf(std::get<0>(*typed)), *key);
                typed.reset();
            }

            if (ImGui::BeginPopup(GRAPH_MENU))
            {
                if (ImGui::MenuItem(ICON_FA_ARROWS_UP_DOWN " Fit the values"))
                    fitPending = true;
                ImGui::Separator();
                for (const auto curve : curves)
                {
                    if (ImGui::MenuItem(GetName(curve).c_str(), nullptr, curve.shown))
                        SetShown(curve.type, curve.index, !curve.shown);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_XMARK " Close the graph"))
                    Close();
                ImGui::EndPopup();
            }
        }
    }  // namespace

    bool IsOpen()
    {
        return !curves.empty();
    }

    bool Shows(Types::KeyframeablePropertyType type)
    {
        return std::ranges::any_of(curves, [type](const Curve& curve) { return curve.type == type; });
    }

    void Toggle(Types::KeyframeablePropertyType type)
    {
        if (Shows(type))
        {
            if (drag.type == type)
                EndDrag();
            std::erase_if(curves, [type](const Curve& curve) { return curve.type == type; });
        }
        else
        {
            const auto& property = PropertyOf(type);
            for (std::int32_t i = 0; i < property.GetValueCount(); ++i)
            {
                // (a value with no name of three that have names isn't one: a light's aim is its pitch and yaw)
                if (property.valueType == Types::KeyframeValueType::Vector3 && property.valueNames[i].empty() &&
                    !property.valueNames[0].empty())
                    continue;
                curves.push_back({type, static_cast<std::uint32_t>(i),
                                  type != Types::KeyframeablePropertyType::CampathCamera || i < 3});
            }
        }
        fitPending = true;
    }

    void Close()
    {
        EndDrag();
        curves.clear();
    }

    void Refit()
    {
        fitPending = true;
    }

    void Clear()
    {
        Close();
        typed.reset();
        openTyped = false;
        points.clear();
        legend.clear();
    }

    void Draw(const Frame& frame)
    {
        legend.clear();
        if (curves.empty() || frame.rect.GetHeight() < 20.0f || frame.axis.width < 20.0f)
        {
            points.clear();
            return;
        }

        ImGui::SetCursorScreenPos(frame.rect.Min);
        ImGui::BeginChild("##graph", frame.rect.GetSize(), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        plot =
            ImRect(ImVec2(frame.axis.x0, frame.rect.Min.y), ImVec2(frame.axis.x0 + frame.axis.width, frame.rect.Max.y));
        if (fitPending)
        {
            Fit(frame.axis.view);
            fitPending = false;
        }
        // (a few pixels free above and below, for the keys at the ends)
        values = {low, high, plot.Min.y + 6, std::max(plot.GetHeight() - 12, 1.0f)};

        PlacePoints(frame);
        const auto mouse = ImGui::GetMousePos();
        const bool plotHovered = ImGui::IsWindowHovered() && plot.Contains(mouse);
        const auto hovered = plotHovered && drag.gesture == Gesture::None ? FindPoint(mouse) : std::nullopt;

        DrawPlot(frame, hovered);
        DrawHeader(frame);
        // (the swatches and Close can take values out)
        if (!curves.empty())
            HandleInput(frame, hovered, plotHovered);
        DrawPopups();
        if (frame.drawMenus)
            frame.drawMenus();
        ImGui::EndChild();
    }

    bool IsDragging()
    {
        return drag.gesture != Gesture::None;
    }

    const std::vector<Curve>& GetCurves()
    {
        return curves;
    }

    void SetShown(Types::KeyframeablePropertyType type, std::uint32_t index, bool shown)
    {
        for (auto& curve : curves)
        {
            if (curve.type == type && curve.index == index)
                curve.shown = shown;
        }
        fitPending = true;
    }

    ImRect GetPlot()
    {
        return plot;
    }

    TimelineMath::ValueAxis GetValueAxis()
    {
        return values;
    }

    std::optional<ImVec2> GetKeyPoint(Types::KeyframeablePropertyType type, std::uint32_t index, std::size_t key)
    {
        for (const auto& point : points)
        {
            if (point.type == type && point.index == index && point.key == key)
                return point.position;
        }
        return std::nullopt;
    }

    std::vector<std::pair<std::string, ImRect>> GetLegend()
    {
        return legend;
    }
}  // namespace IWXMVM::UI::TimelineGraph
