#include "StdInclude.hpp"
#include "Layout.hpp"

#include "Mod.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Components/CaptureManager.hpp"
#include "UI/UIManager.hpp"
#include "UI/Components/TimelineGraph.hpp"

namespace IWXMVM::UI::Layout
{
    namespace
    {
        ImRect view, panel, timeline;

        // a splitter being dragged: the size it had and where the mouse was when the drag started
        struct Drag
        {
            float size = 0.0f;
            float mouse = 0.0f;
        };
        std::optional<Drag> drag;

        std::vector<std::pair<std::string, ImRect>> splitters;

        // the layout's windows, behind every other one (the panel's tabs are windows of their own)
        constexpr std::array<std::string_view, 13> LAYOUT_WINDOWS = {
            "##splitter.timeline",
            "##splitter.panel",
            "Timeline",
            "Demos",
            "Kills",
            "Camera",
            "Visuals",
            "Actors",
            "Players",
            "Capture",
            "PrimaryTabs",
            "GameView",
            "Background",
        };

        bool IsTimelineShown()
        {
            return Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
        }

        void SetSizes(const LayoutMath::Sizes& sizes)
        {
            auto& preferences = PreferencesConfiguration::Get();
            preferences.layoutPanel = sizes.panel;
            preferences.layoutTimeline = sizes.timeline;
            preferences.layoutTimelineWithGraph = sizes.timelineWithGraph;
        }

        // A splitter on an edge between two of the layout's windows, as a window of its own in front of them (so it
        // gets the mouse rather than them): the drag sets size to what it was plus how far the mouse went along
        // (back, for a part on the far side of the edge), a double-click puts the default back. Returns whether it
        // was let go (or reset), when the sizes are saved.
        bool DrawSplitter(const char* name, ImRect rect, bool vertical, float size,
                          const std::function<void(float)>& setSize, const std::function<void()>& reset)
        {
            splitters.emplace_back(name + 11, rect);  // (without "##splitter.")
            ImGui::SetNextWindowPos(rect.Min);
            ImGui::SetNextWindowSize(rect.GetSize());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            constexpr ImGuiWindowFlags flags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
            bool released = false;
            if (ImGui::Begin(name, nullptr, flags))
            {
                ImGui::BeginDisabled(Components::CaptureManager::Get().IsCapturing());
                ImGui::InvisibleButton("##drag", rect.GetSize());
                const bool hovered = ImGui::IsItemHovered();
                const bool active = ImGui::IsItemActive();
                const auto mouse = vertical ? ImGui::GetMousePos().x : ImGui::GetMousePos().y;
                if (ImGui::IsItemActivated())
                    drag = Drag{size, mouse};
                if (active && drag)
                    setSize(drag->size - (mouse - drag->mouse));
                if (ImGui::IsItemDeactivated())
                {
                    drag.reset();
                    released = true;
                }
                // (the second click starts a drag too, which would put the size back while it is held)
                if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    drag.reset();
                    reset();
                    released = true;
                }
                if (hovered || active)
                {
                    ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
                    const auto middle = vertical ? std::floor(rect.GetCenter().x) : std::floor(rect.GetCenter().y);
                    const auto from = vertical ? ImVec2(middle, rect.Min.y) : ImVec2(rect.Min.x, middle);
                    const auto to = vertical ? ImVec2(middle, rect.Max.y) : ImVec2(rect.Max.x, middle);
                    ImGui::GetWindowDrawList()->AddLine(
                        from, to, ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered),
                        2.0f);
                }
                if (!active && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay))
                    ImGui::SetTooltip("Drag to resize, double-click for the default size");
                ImGui::EndDisabled();
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            return released;
        }
    }  // namespace

    LayoutMath::Sizes GetSizes()
    {
        const auto& preferences = PreferencesConfiguration::Get();
        return {preferences.layoutPanel, preferences.layoutTimeline, preferences.layoutTimelineWithGraph};
    }

    void Update()
    {
        const auto display = ImGui::GetIO().DisplaySize;
        const auto top = UIManager::Get().GetUIComponent(Component::MenuBar)->GetSize().y;
        const auto layout = LayoutMath::Fit(display.x, std::max(display.y - top, 0.0f), ImGui::GetFontSize(),
                                            GetSizes(), IsTimelineShown(), TimelineGraph::IsOpen());
        view = ImRect(0, top, layout.viewWidth, top + layout.viewHeight);
        panel = ImRect(layout.viewWidth, top, display.x, top + layout.viewHeight);
        timeline = ImRect(0, view.Max.y, display.x, view.Max.y + layout.timelineHeight);
    }

    ImRect GetView()
    {
        return view;
    }

    ImRect GetPanel()
    {
        return panel;
    }

    ImRect GetTimeline()
    {
        return timeline;
    }

    void DrawSplitters()
    {
        splitters.clear();
        const auto display = ImGui::GetIO().DisplaySize;
        const auto fontSize = ImGui::GetFontSize();
        const auto half = std::max(std::floor(fontSize * 0.2f), 3.0f);
        const auto save = [] { Configuration::Get().Write(true); };

        const auto panelWidth = panel.GetWidth();
        if (DrawSplitter(
                "##splitter.panel", ImRect(view.Max.x - half, view.Min.y, view.Max.x + half, view.Max.y), true,
                panelWidth,
                [&](float pixels) { SetSizes(LayoutMath::SetPanel(GetSizes(), pixels, display.x, fontSize)); },
                [] {
                    auto sizes = GetSizes();
                    sizes.panel = LayoutMath::DEFAULT_PANEL;
                    SetSizes(sizes);
                }))
            save();

        if (IsTimelineShown())
        {
            const bool graph = TimelineGraph::IsOpen();
            const auto height = display.y - view.Min.y;
            if (DrawSplitter(
                    "##splitter.timeline", ImRect(0, timeline.Min.y - half, display.x, timeline.Min.y + half), false,
                    timeline.GetHeight(),
                    [&](float pixels) {
                        SetSizes(LayoutMath::SetTimeline(GetSizes(), pixels, height, fontSize, graph));
                    },
                    [graph] {
                        auto sizes = GetSizes();
                        (graph ? sizes.timelineWithGraph : sizes.timeline) =
                            graph ? LayoutMath::DEFAULT_TIMELINE_WITH_GRAPH : LayoutMath::DEFAULT_TIMELINE;
                        SetSizes(sizes);
                    }))
                save();
        }

        // ImGui puts a window made with ImGuiWindowFlags_NoBringToFrontOnFocus behind all the others, so the
        // background, which covers the screen, would end up in front of those made before it, and the splitters
        // behind the windows they lie on: from the front, the splitters, the layout's windows, then the background
        for (const auto name : LAYOUT_WINDOWS)
        {
            if (const auto window = ImGui::FindWindowByName(name.data()))
                ImGui::BringWindowToDisplayBack(window);
        }
    }

    void Apply(Preset preset)
    {
        switch (preset)
        {
            case Preset::Default:
                SetSizes({});
                break;
            case Preset::Editing:
                SetSizes({32.0f, 22.0f, 30.0f});
                break;
            case Preset::Watching:
                SetSizes({LayoutMath::MIN_PANEL, LayoutMath::MIN_TIMELINE, LayoutMath::DEFAULT_TIMELINE_WITH_GRAPH});
                break;
        }
        Configuration::Get().Write(true);
    }

    void SetScale(float scale)
    {
        PreferencesConfiguration::Get().uiScale = LayoutMath::ValidScale(scale);
        Configuration::Get().Write(true);
    }

    std::optional<ImRect> GetSplitter(std::string_view name)
    {
        for (const auto& [splitter, rect] : splitters)
        {
            if (splitter == name)
                return rect;
        }
        return std::nullopt;
    }
}  // namespace IWXMVM::UI::Layout
