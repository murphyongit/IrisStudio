#include "StdInclude.hpp"
#include "ActorsMenu.hpp"

#include "UI/Theme.hpp"

#include "Mod.hpp"
#include "Resources.hpp"
#include "Events.hpp"
#include "Components/Actors.hpp"
#include "Components/PlayerLookLogic.hpp"
#include "Components/CameraManager.hpp"
#include "Components/KillFinder.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/BoneCamera.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "Components/LightLogic.hpp"
#include "Components/Lights.hpp"
#include "Components/LiveActors.hpp"
#include "Components/LiveGame.hpp"
#include "Components/Loadouts.hpp"
#include "Components/Playback.hpp"
#include "Components/PropLogic.hpp"
#include "Components/Props.hpp"
#include "Components/PoseSignatures.hpp"
#include "Components/ReloadProfiles.hpp"
#include "Components/Rewinding.hpp"
#include "Components/ShotLogic.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Graphics/Graphics.hpp"
#include "Input.hpp"
#include "UI/ActorActions.hpp"
#include "UI/GaitColors.hpp"
#include "UI/UIManager.hpp"
#include "UI/Toasts.hpp"
#include "UI/Components/ReloadStrip.hpp"
#include "UI/Components/Timeline.hpp"
#include "UI/ImGuiEx/ImGuiExtensions.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"

namespace IWXMVM::UI
{
    ImVec4 MisfitColor()
    {
        return Theme::Misfit();
    }

    using Catalog = Types::ActorCatalog;
    using ModelKind = Catalog::ModelKind;

    namespace
    {
        // every section of the panel opened when it is drawn next (see ResetActorPanel)
        bool openSections = false;
    }  // namespace

    // the players of the demo by client number, from the kill finder's scan
    std::vector<std::pair<std::int32_t, std::string>> GetPlayers();

    void ActorsMenu::Initialize()
    {
        using Commands::Bit;
        using Commands::Context;

        // a keyed light or prop put somewhere on the game's thread is keyed here (Components::Lights::MoveTo,
        // Components::Props::MoveTo)
        Events::RegisterListener(EventType::OnFrame, [] {
            for (const auto& move : Components::Lights::TakeKeyedMoves())
                ActorActions::KeyLightPlace(move.actor, move.position, move.aim, move.step);
            for (const auto& move : Components::Props::TakeKeyedMoves())
                ActorActions::KeyPropPlace(move.actor, move.position, move.turn, move.step);
        });

        // A live game's bots, with the editor hidden too: keys instead of opening menus
        const auto notLive = []() -> std::optional<std::string> {
            if (!Mod::GetGameInterface()->IsHostingLiveGame())
                return "Only in a game of your own";
            return std::nullopt;
        };
        Commands::Register({
            .id = "live.addActor",
            .label = "Add a bot where you look",
            .category = "Live game",
            .defaults = {ImGuiKey_F5},
            .contexts = Bit(Context::Capturing) | Bit(Context::Global),
            .unavailable = notLive,
            .run = [] { Components::LiveActors::AddNew(); },
            .keywords = "bot",
        });
        Commands::Register({
            .id = "live.removeActor",
            .label = "Remove the bot you look at, else the newest",
            .category = "Live game",
            .defaults = {ImGuiKey_F6},
            .contexts = Bit(Context::Capturing) | Bit(Context::Global),
            .unavailable = notLive,
            .run = [] { Components::LiveActors::RequestRemoveLooked(); },
            .keywords = "bot kick",
        });
        // (next to F5 and F6)
        Commands::Register({
            .id = "live.clearBodies",
            .label = "Clear the bodies",
            .category = "Live game",
            .defaults = {ImGuiKey_F7},
            .contexts = Bit(Context::Capturing) | Bit(Context::Global),
            .unavailable = notLive,
            .run = [] { Components::LiveGame::RequestClearBodies(); },
            .keywords = "corpses",
        });

        // While Click to place is on: Enter or Escape stop it, Backspace removes the point placed last (the selected
        // one)
        Commands::Register({
            .id = "placing.finish",
            .label = "Stop placing path points",
            .category = "Actors",
            .defaults = {ImGuiKey_Enter, ImGuiKey_Escape},
            .contexts = Bit(Context::Placing),
            .run = [] { GFX::GraphicsManager::Get().SetPlacingPathPoints(false); },
        });
        // 1, 2, 3: the next stretch's gait; placing comes before the cameras
        // these keys pick otherwise
        for (const auto& [id, label, key, gait] :
             {std::tuple("placing.walk", "Next stretch: Walk", ImGuiKey_1, Types::ActorGait::Walk),
              std::tuple("placing.run", "Next stretch: Run", ImGuiKey_2, Types::ActorGait::Run),
              std::tuple("placing.sprint", "Next stretch: Sprint", ImGuiKey_3, Types::ActorGait::Sprint)})
            Commands::Register({
                .id = id,
                .label = label,
                .category = "Actors",
                .defaults = {key},
                .contexts = Bit(Context::Placing),
                .run = [gait] { GFX::GraphicsManager::Get().placingGait = std::pair(gait, false); },
            });
        Commands::Register({
            .id = "placing.removeLast",
            .label = "Remove the path point placed last",
            .category = "Actors",
            .defaults = {ImGuiKey_Backspace},
            .contexts = Bit(Context::Placing),
            .unavailable =
                []() -> std::optional<std::string> {
                const auto selected = Components::Actors::GetSelected();
                const auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt;
                const auto point = Components::Actors::GetSelectedWaypoint();
                if (!actor || !point || *point >= actor->path.size())
                    return "No point is selected";
                if (actor->path.size() <= 1)
                    return "A path keeps at least one point";
                return std::nullopt;
            },
            .run =
                [] {
                    auto actor = *Components::Actors::GetActor(*Components::Actors::GetSelected());
                    const auto point = *Components::Actors::GetSelectedWaypoint();
                    ActorLogic::RemoveWaypoint(actor, point);
                    Components::Actors::Update(actor, false, "delete path point of " + actor.name);
                    Components::Actors::SelectWaypoint(point > 0 ? std::optional(point - 1) : std::nullopt);
                },
        });
    }

    // what an actor is made of: the body first, then a head and what is worn, each optional
    struct Parts
    {
        std::string body, head;
        std::vector<std::string> worn;

        std::vector<std::string> ToModels() const
        {
            std::vector<std::string> models{body};
            if (!head.empty())
                models.push_back(head);
            for (const auto& model : worn)
                if (!model.empty())
                    models.push_back(model);
            return models;
        }
    };

    ModelKind GetKind(const Catalog& catalog, std::string_view model)
    {
        if (const auto found = catalog.FindModel(model))
            return found->kind;
        return model.find("head") != std::string_view::npos ? ModelKind::Head : ModelKind::Gear;
    }

    Parts GetParts(const Catalog& catalog, const Types::Actor& actor)
    {
        Parts parts;
        for (std::size_t i = 0; i < actor.models.size(); ++i)
        {
            const auto& model = actor.models[i];
            if (i == 0)
                parts.body = model;
            else if (GetKind(catalog, model) == ModelKind::Head && parts.head.empty())
                parts.head = model;
            else
                parts.worn.push_back(model);
        }
        return parts;
    }

    bool MatchesFilter(std::string_view text, std::string_view filter)
    {
        if (filter.empty())
            return true;
        auto lower = [](std::string_view s) {
            std::string out(s);
            std::ranges::transform(out, out.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        };
        return lower(text).find(lower(filter)) != std::string::npos;
    }

    void FilterField(const char* id, std::string& filter)
    {
        if (ImGui::IsWindowAppearing())
        {
            filter.clear();
            ImGui::SetKeyboardFocusHere();
        }
        char buffer[128];
        strncpy_s(buffer, filter.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint(id, ICON_FA_MAGNIFYING_GLASS " Search", buffer, sizeof(buffer)))
            filter = buffer;
    }

    // how many bones models have together, as the catalog knows them
    std::uint32_t CountBones(const Catalog& catalog, std::initializer_list<std::string_view> models)
    {
        std::uint32_t bones = 0;
        for (const auto model : models)
            if (const auto found = catalog.FindModel(model))
                bones += found->bones;
        return bones;
    }

    std::uint32_t CountBones(const Catalog& catalog, const std::vector<std::string>& models)
    {
        std::uint32_t bones = 0;
        for (const auto& model : models)
            if (const auto found = catalog.FindModel(model))
                bones += found->bones;
        return bones;
    }

    // A combo of the models of some kinds, grouped by where they come from, with a search field and, if noneLabel is
    // given, a first entry for none. A model the character would have more bones with than the game can draw (as
    // bonesWith counts them) can't be chosen; what header draws comes first. Returns whether the choice changed.
    bool ModelCombo(const char* id, const Catalog& catalog, std::string& choice, std::initializer_list<ModelKind> kinds,
                    const char* noneLabel, std::string& filter,
                    const std::function<std::uint32_t(const Catalog::Model&)>& bonesWith = {},
                    const std::function<void()>& header = {})
    {
        const auto preview = choice.empty() ? std::string(noneLabel ? noneLabel : "")
                                            : Components::Actors::DescribeModel(choice) +
                                                  (catalog.FindModel(choice) ? "" : " (not loaded)");
        bool changed = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool open = ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge);
        // (the harness clicks it by its id)
        if (!open)
            ImGuiEx::Keyframeable::Record(std::format("actors.{}", std::string_view(id).substr(2)));
        if (open)
        {
            FilterField("##modelFilter", filter);
            if (header)
                header();
            if (noneLabel && filter.empty() && ImGui::Selectable(noneLabel, choice.empty()))
            {
                choice.clear();
                changed = true;
            }

            // Multiplayer first, then the pack
            std::vector<const Catalog::Model*> models;
            for (const auto& model : catalog.models)
            {
                const auto description = Components::Actors::DescribeModel(model.name);
                if (std::ranges::find(kinds, model.kind) != kinds.end() &&
                    (MatchesFilter(description, filter) || MatchesFilter(model.name, filter)))
                    models.push_back(&model);
            }
            std::ranges::stable_sort(models, {},
                                     [](const Catalog::Model* model) { return model->source != "Multiplayer"; });

            std::string source;
            for (const auto model : models)
            {
                if (model->source != source)
                {
                    source = model->source;
                    ImGui::SeparatorText(source.c_str());
                }
                const auto description = Components::Actors::DescribeModel(model->name);
                const auto bones = bonesWith ? bonesWith(*model) : 0;
                const bool tooMany = catalog.maxBones != 0 && bones > catalog.maxBones;
                ImGui::BeginDisabled(tooMany && choice != model->name);
                if (ImGui::Selectable(std::format("{}##{}", description, model->name).c_str(), choice == model->name))
                {
                    choice = model->name;
                    changed = true;
                }
                ImGui::EndDisabled();
                if (tooMany && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip(
                        "%s\nToo many bones with the rest of the character: %u, of the %u the game can "
                        "draw",
                        model->name.c_str(), bones, catalog.maxBones);
                else if (!tooMany)
                    ImGui::SetItemTooltip("%s", model->name.c_str());
            }
            if (models.empty())
                ImGui::TextDisabled("Nothing found");
            ImGui::EndCombo();
        }
        return changed;
    }

    // goes to an actor's tick (a clip's start, an event's), as the keyframe arrows do
    void JumpTo(std::uint32_t tick)
    {
        Components::Playback::SeekTimeline(Components::Playback::ActorToTimelineTick(tick));
    }

    std::string FormatActorTime(std::uint32_t tick)
    {
        return std::format("{:.2f} s", tick / 1000.0);
    }

    void EndActorEditStepWhenIdle()
    {
        if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            Components::Actors::EndEditStep();
    }

    namespace
    {
    // a tooltip with the key of the command that does the same, as it is bound now: "Copies the clip (Ctrl+D)"
    std::string WithKey(std::string_view text, std::string_view command)
    {
        const auto key = Commands::GetShortcut(command);
        return key.empty() ? std::string(text) : std::format("{} ({})", text, key);
    }

    // A label left of the control after it; the controls start at the same x, after the longest label
    void Label(const char* text)
    {
        const auto width = std::min(std::max({ImGui::CalcTextSize("Characters").x, ImGui::CalcTextSize("Walk speed").x,
                                              ImGui::CalcTextSize("Attachments").x}) +
                                        ImGui::GetFontSize() * 0.75f,
                                    ImGui::GetContentRegionAvail().x * 0.4f);
        // (SameLine counts from the window's edge, not from where its contents start)
        const auto start = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(start + width);
    }

    // Whether a section shows the options used less, after the ones used most: its Advanced, on its header (kept per
    // section in config.json, see PanelLogic::ReadAdvancedSections; off at first)
    bool IsAdvanced(PanelLogic::ActorPage section)
    {
        return PanelLogic::ReadAdvancedSections(PreferencesConfiguration::Get().actorsAdvancedOpen).contains(section);
    }

    // turns a section's Advanced on or off, or every section's with all
    void SetAdvanced(PanelLogic::ActorPage section, bool on, bool all)
    {
        using PanelLogic::ActorPage;
        auto& names = PreferencesConfiguration::Get().actorsAdvancedOpen;
        auto sections = PanelLogic::ReadAdvancedSections(names);
        for (const auto each : {ActorPage::Look, ActorPage::Movement, ActorPage::Animation, ActorPage::Events})
        {
            if (!all && each != section)
                continue;
            if (on)
                sections.insert(each);
            else
                sections.erase(each);
        }
        names = PanelLogic::WriteAdvancedSections(sections);
        Configuration::Get().Write(true);
    }

    // Marks the rows drawn while it lives as advanced ones, with a line at their left
    struct AdvancedRows
    {
        AdvancedRows()
        {
            ImGui::BeginGroup();
        }
        ~AdvancedRows()
        {
            ImGui::EndGroup();
            const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            if (max.y > min.y)
                ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x - 5.0f, min.y), ImVec2(min.x - 5.0f, max.y),
                                                    ImGui::GetColorU32(ImGuiCol_CheckMark, 0.7f), 2.0f);
        }
    };

    struct RowButton
    {
        const char* icon;
        const char* text;
        const char* tooltip;
        bool disabled = false;
        bool active = false;           // drawn pressed, as the mode it is in
        const char* record = nullptr;  // the name the harness clicks it by (ImGuiEx::Keyframeable::Record)
    };

    // Buttons sharing a row; each shows its icon and text when all of them fit, else its icon with the text in its
    // tooltip. Returns which one was clicked.
    std::optional<std::size_t> ButtonRow(const char* id, std::span<const RowButton> buttons)
    {
        const auto& style = ImGui::GetStyle();
        const auto count = static_cast<float>(buttons.size());
        const auto width = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * (count - 1)) / count;
        const bool textFits = std::ranges::all_of(buttons, [&](const RowButton& button) {
            return ImGui::CalcTextSize(std::format("{} {}", button.icon, button.text).c_str()).x +
                       style.FramePadding.x * 2 <=
                   width;
        });

        std::optional<std::size_t> clicked;
        ImGui::PushID(id);
        std::size_t i = 0;
        for (const auto& button : buttons)
        {
            if (i > 0)
                ImGui::SameLine();
            ImGui::BeginDisabled(button.disabled);
            if (button.active)
                ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
            const auto label = textFits ? std::format("{} {}##{}", button.icon, button.text, i)
                                        : std::format("{}##{}", button.icon, i);
            if (ImGui::Button(label.c_str(), ImVec2(width, 0)))
                clicked = i;
            if (button.record)
                ImGuiEx::Keyframeable::Record(button.record);
            if (button.active)
                ImGui::PopStyleColor();
            ImGui::EndDisabled();
            if (textFits)
                ImGui::SetItemTooltip("%s", button.tooltip);
            else
                ImGui::SetItemTooltip("%s: %s", button.text, button.tooltip);
            ++i;
        }
        ImGui::PopID();
        return clicked;
    }
    std::optional<std::size_t> ButtonRow(const char* id, std::initializer_list<RowButton> buttons)
    {
        return ButtonRow(id, std::span<const RowButton>(buttons.begin(), buttons.size()));
    }

    // the piece of a walk clip shown: the one with the stretch from the selected
    // point, else its first
    const ActorLogic::WalkPiece* FindShownPiece(const std::vector<ActorLogic::WalkPiece>& pieces, std::uint32_t clip,
                                                std::optional<std::size_t> point)
    {
        const ActorLogic::WalkPiece* found = nullptr;
        for (const auto& piece : pieces)
        {
            if (piece.clip != clip)
                continue;
            if (!found)
                found = &piece;
            if (point && *point >= piece.first && *point <= piece.last)
                return &piece;
        }
        return found;
    }

    // a point of a path as the panel names it
    std::string PointName(std::size_t point)
    {
        return point == 0 ? std::string("start") : std::format("point {}", point);
    }

    // A time on the timeline, shown the same everywhere: a button that jumps there, the time in seconds to drag or
    // type (the tick in its tooltip), and a button that moves it to the playhead. Returns whether it changed.
    bool WhenControl(const char* id, std::uint32_t& tick)
    {
        const ImVec2 size(ImGui::GetFrameHeight(), 0);
        bool changed = false;
        ImGui::PushID(id);
        if (ImGui::Button(ICON_FA_FORWARD_STEP, size))
            JumpTo(tick);
        ImGui::SetItemTooltip("Jump there");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - size.x - ImGui::GetStyle().ItemSpacing.x);
        float seconds = tick / 1000.0f;
        if (ImGui::DragFloat("##time", &seconds, 0.01f, 0.0f, 86400.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
        {
            tick = static_cast<std::uint32_t>(std::lround(seconds * 1000.0f));
            changed = true;
        }
        ImGui::SetItemTooltip("Tick %u; drag it, or Ctrl+click to type", tick);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ARROWS_TO_DOT, size))
        {
            tick = Components::Playback::GetActorTick();
            changed = true;
        }
        ImGui::SetItemTooltip("To the playhead");
        ImGui::PopID();
        return changed;
    }
    }  // namespace

    ImVec2 ProjectPreview(const glm::vec3& position, float yaw)
    {
        // seen from the side, turned by yaw; x forward, y left, z up
        const float side = -position.x * std::sin(yaw) + position.y * std::cos(yaw);
        return ImVec2(-side, -position.z);
    }

    // The skeleton of a model in an animation, playing over and over, framed to fit all of it; dragging turns it
    void DrawAnimationPreview(const std::string& animation, const std::string& model, ImVec2 size)
    {
        static std::string previewKey;
        static double previewStart = 0;
        static float previewYaw = 0.6f;
        static std::optional<std::pair<ImVec2, ImVec2>> previewBounds;
        static float previewBoundsYaw = 0;

        if (!ImGui::BeginChild("##actorAnimationPreview", size, true, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::EndChild();
            return;
        }
        const auto area = ImGui::GetContentRegionAvail();
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##actorPreviewDrag", ImVec2(std::max(area.x, 1.0f), std::max(area.y, 1.0f)));
        if (ImGui::IsItemActive())
            previewYaw += ImGui::GetIO().MouseDelta.x * 0.01f;
        ImGui::SetCursorScreenPos(origin);

        auto& game = *Mod::GetGameInterface();
        const auto first =
            animation.empty() || model.empty() ? std::nullopt : game.GetAnimationPoseByName(animation, model, 0.0f);
        if (!first)
        {
            ImGui::TextDisabled(animation.empty() ? "Hover an animation\nto preview it" : "No preview");
            ImGui::EndChild();
            return;
        }

        const auto key = animation + "|" + model;
        if (previewKey != key)
        {
            previewKey = key;
            previewStart = ImGui::GetTime();
            previewBounds.reset();
        }
        if (!previewBounds || previewBoundsYaw != previewYaw)
        {
            ImVec2 min(0, 0), max(0, 0);
            for (int i = 0; i <= 8; ++i)
            {
                if (const auto pose = game.GetAnimationPoseByName(animation, model, i / 8.0f))
                {
                    for (const auto& position : pose->positions)
                    {
                        const auto point = ProjectPreview(position, previewYaw);
                        min = ImVec2(std::min(min.x, point.x), std::min(min.y, point.y));
                        max = ImVec2(std::max(max.x, point.x), std::max(max.y, point.y));
                    }
                }
            }
            previewBounds = std::pair(min, max);
            previewBoundsYaw = previewYaw;
        }

        // plays, holds the last frame for a moment and starts again
        constexpr double HOLD = 0.5;
        const auto duration = std::max(first->duration, 0.01f);
        const double elapsed = std::fmod(ImGui::GetTime() - previewStart, duration + HOLD);
        const auto pose = game.GetAnimationPoseByName(animation, model, static_cast<float>(elapsed / duration));
        if (!pose)
        {
            ImGui::EndChild();
            return;
        }

        const auto [min, max] = *previewBounds;
        const auto textHeight = ImGui::GetTextLineHeightWithSpacing();
        const float scale =
            std::min(area.x / std::max(max.x - min.x, 1.0f), (area.y - textHeight) / std::max(max.y - min.y, 1.0f)) *
            0.9f;
        const ImVec2 center(origin.x + area.x / 2, origin.y + (area.y - textHeight) / 2);
        const ImVec2 middle((min.x + max.x) / 2, (min.y + max.y) / 2);
        const auto toScreen = [&](const glm::vec3& position) {
            const auto point = ProjectPreview(position, previewYaw);
            return ImVec2(center.x + (point.x - middle.x) * scale, center.y + (point.y - middle.y) * scale);
        };

        auto drawList = ImGui::GetWindowDrawList();
        const auto groundY = toScreen(glm::vec3(0)).y;
        drawList->AddLine(ImVec2(origin.x, groundY), ImVec2(origin.x + area.x, groundY),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled));
        const auto color = ImGui::GetColorU32(ImGuiCol_Text);
        for (std::size_t bone = 0; bone < pose->positions.size(); ++bone)
        {
            // the bones hanging from the model's origin would draw lines from the floor
            const auto parent = pose->parents[bone];
            if (parent < 0 || pose->parents[parent] < 0)
                continue;
            drawList->AddLine(toScreen(pose->positions[parent]), toScreen(pose->positions[bone]), color, 2.0f);
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + area.y - textHeight));
        ImGui::TextDisabled("%s, %.1f s", animation.c_str(), first->duration);
        ImGui::EndChild();
    }

    // an animation as lists show it: a killstreak's motion by its name ("Fly"), others by theirs
    std::string ShownName(const std::string& animation)
    {
        return Components::KillstreakLogic::IsMotion(animation)
                   ? std::format("{} ({})", Components::KillstreakLogic::GetMotionLabel(animation), animation)
                   : animation;
    }

    // a text with underscores for its spaces, as the harness names controls
    std::string Underscored(std::string text)
    {
        std::ranges::replace(text, ' ', '_');
        return text;
    }

    using Listed = AnimationCatalogLogic::Listed;

    // how an animation stands and moves as it starts or ends, in words: "crouching, still"; empty when nothing is known
    std::string DescribeState(const Types::AnimationState& state)
    {
        constexpr std::array POSTURES = {"", "standing", "crouching", "prone", "on its back"};
        constexpr std::array MOVEMENTS = {"", "still", "walking", "running", "sprinting"};
        const std::string posture = POSTURES[static_cast<std::size_t>(state.posture)];
        const std::string movement = MOVEMENTS[static_cast<std::size_t>(state.movement)];
        return posture.empty() || movement.empty() ? posture + movement : posture + ", " + movement;
    }

    // What an animation is, for its tooltip and the browser's preview: where it is from, what it moves and for which
    // rig, how it starts and ends, whether it is loaded or loads from the animation pack as it is used, why it can't
    // be played if neither the game nor the pack has it, and why it doesn't fit the models given (if it doesn't)
    std::string DescribeAnimation(const Listed& listed, const Components::Actors::AnimationList& list,
                                  const std::vector<std::string>* models = nullptr)
    {
        constexpr std::array KINDS = {"full body", "upper body", "face", "other"};
        constexpr std::array FAMILIES = {"MP players' rig",    "SP rig (AI, zombies)", "human", "dog",
                                         "first person arms", "prop or vehicle"};
        std::string text = std::format("{}, from {}", AnimationCatalogLogic::GetCategories()[listed.category],
                                       list.headings[listed.heading]);
        if (Components::KillstreakLogic::IsMotion(listed.name))
            text += std::format("\n{}: {}", Components::KillstreakLogic::GetMotionLabel(listed.name),
                                Components::KillstreakLogic::DescribeMotion(listed.name));
        const auto entry = listed.entry;
        if (entry)
        {
            if (entry->AssetName() != entry->name)
                text += std::format("\nA copy of {} that differs from the game's", entry->AssetName());
            text += std::format("\n{:.1f} s{}, {} for the {}", entry->Duration(), entry->loop ? ", loops" : "",
                                KINDS[static_cast<std::size_t>(entry->kind)],
                                FAMILIES[static_cast<std::size_t>(entry->family)]);
            const auto from = DescribeState(entry->from);
            const auto to = DescribeState(entry->to);
            if (entry->from == entry->to)
                text += from.empty() ? "" : "\n" + from;
            else
                text += std::format("\n{}{}to {}", from.empty() ? "" : "From ", from.empty() ? "" : from + " ",
                                    to.empty() ? "?" : to);
            if (std::abs(entry->turn) >= 1.0f)
                text += std::format(", turns {:.0f} degrees {}", std::abs(entry->turn), entry->turn > 0 ? "left" : "right");
        }
        if (!listed.loaded)
            text += std::format(
                "\nNot loaded: {}. The game has multiplayer's animations, and the campaign's common "
                "ones once a campaign character is loaded; this one is listed so you can find it.",
                list.game && !list.game->animationStoreError.empty() ? list.game->animationStoreError
                                                                     : "the animation pack can't be loaded from");
        else if (listed.loaded->stored)
            text += Components::Actors::IsStoreLoaded(listed.name)
                        ? "\nLoaded from the animation pack"
                        : "\nIn the animation pack: it loads as it is used, in a moment";
        if (models)
            if (const auto misfit = Components::Actors::DescribeMisfit(*models, listed.name); !misfit.empty())
                text += "\n" ICON_FA_BAN " Doesn't fit the actor: " + misfit;
        return text;
    }

    AnimationBrowserState browserState;
    // the animation the browser was opened from (a clip's, a look's, a death's), whose kind it opens on:
    // empty for none
    std::string browserLikeThis;

    AnimationBrowserState GetAnimationBrowserState()
    {
        return browserState;
    }

    // A window of every animation the game has (Components::Actors::GetAnimationList), by where they are from, sorted
    // into categories, with favorites, a search, a filter of those that fit the actor's models (F3, on unless turned
    // off: then those that don't are marked, saying why; while the models load, nothing until it is known what fits
    // them) and a preview on its body; of the face's alone, those animations only. Those the animation store loads
    // from the pack as they are used are marked so (as the character picker marks the characters that load as they
    // are chosen); without the pack, those the game hasn't loaded are greyed and can't be chosen. Returns whether one
    // was chosen (a double-click or Use).
    bool AnimationBrowser(const char* popupId, const Catalog& catalog, const std::vector<std::string>& models,
                          std::string& choice, std::string& filter, std::string& category, bool facesOnly)
    {
        bool chosen = false;
        // A popup sizes itself to what it holds, so the parts get sizes of their own; sized from what is left, they
        // shrank a little every frame
        const auto fontSize = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopup(popupId))
            return false;

        static std::string hovered;
        static std::string highlighted;
        // the filters: where from (a heading, empty for anywhere), only those that fit the actor's models (shared with
        // the view's menu), only those the game has loaded
        static std::string from;
        // (only those that fit, always; every one, loaded or not)
        const bool fitting = Components::Actors::IsFitFilterOn();
        constexpr bool loadedOnly = false;
        if (ImGui::IsWindowAppearing())
            highlighted = choice;

        const auto list = Components::Actors::GetAnimationList();
        static const Components::Actors::AnimationList none;
        const auto& listed = list ? *list : none;
        // which of the pack's the store has loaded, as of this frame
        const auto storeLoaded = Components::Actors::GetStoreLoaded();
        static const Components::Actors::StoreLoaded noneLoaded;
        const auto inMemory = [&](const Listed& animation) {
            return animation.loaded && Components::Actors::IsAnimationLoaded(
                                           *animation.loaded, storeLoaded ? storeLoaded.get() : &noneLoaded);
        };
        const auto body = models.empty() ? std::string() : models.front();
        // (the faces of the Face row's browser are any face's)
        const auto fits = !facesOnly ? Components::Actors::GetFits(models) : nullptr;
        // what fits isn't known until the game has loaded the models: while they load, none rather than all for a
        // moment
        const bool waiting = fitting && !facesOnly && !fits && Components::Actors::AreModelsLoading(models);
        const auto misfits = [&](const Listed& animation) {
            // (a killstreak's motion fits the killstreaks of its class, by their models)
            if (animation.index == SIZE_MAX && Components::KillstreakLogic::IsMotion(animation.name))
                return !facesOnly && !Components::Actors::Fits(models, animation.name).value_or(true);
            return fits && animation.index < fits->size() && !(*fits)[animation.index];
        };

        const auto height = fontSize * 26;
        const auto categoryWidth = fontSize * 12;
        const auto previewWidth = fontSize * 16;
        const auto listWidth = fontSize * 26;
        const auto& categories = AnimationCatalogLogic::GetCategories();
        // opened from an animation: on those like it, the nearest first (the same gait's by speed, deaths, idles)
        static std::string likeThis;
        if (ImGui::IsWindowAppearing())
        {
            likeThis = std::exchange(browserLikeThis, {});
            // (those like it, not those a search left before)
            if (!likeThis.empty())
            {
                category = "Like this";
                filter.clear();
            }
        }
        const auto* reference = likeThis.empty() ? nullptr : Components::Actors::GetAnimationCatalog().Find(likeThis);
        const auto speedOf = [](const Types::AnimationEntry& entry) {
            return glm::length(glm::vec2(entry.motion)) / std::max(entry.Duration(), 0.01f);
        };
        const bool referenceMoves = reference && reference->loop && speedOf(*reference) >= ActorLogic::IDLE_SPEED;
        const auto likeKind = !reference         ? std::string()
                              : reference->death ? std::string("deaths")
                              : referenceMoves
                                  ? std::string(reference->from.movement == Types::AnimationMovement::Sprint ? "sprints"
                                                : reference->from.movement == Types::AnimationMovement::Run  ? "runs"
                                                                                                             : "walks")
                              : reference->loop ? std::string("idles")
                                                : std::string("the same kind");
        const auto isLike = [&](const Listed& animation) {
            const auto* entry = animation.entry;
            if (!reference || !entry)
                return false;
            if (reference->death)
                return entry->death;
            if (referenceMoves)
                return entry->loop && !entry->death && entry->from.movement == reference->from.movement &&
                       entry->from.posture == reference->from.posture && speedOf(*entry) >= ActorLogic::IDLE_SPEED &&
                       std::abs(glm::degrees(std::atan2(entry->motion.y, entry->motion.x)) -
                                glm::degrees(std::atan2(reference->motion.y, reference->motion.x))) <= 15.0f;
            if (reference->loop)
                return entry->loop && !entry->death && entry->from.posture == reference->from.posture &&
                       speedOf(*entry) < ActorLogic::IDLE_SPEED && entry->kind == reference->kind;
            return entry->from == reference->from && entry->to == reference->to && entry->kind == reference->kind;
        };
        if (category == "Like this" && !reference)
            category = "All";
        if (category != "All" && category != "Favorites" && category != "Like this" &&
            (facesOnly || std::ranges::find(categories, category) == categories.end()))
            category = "All";

        // what is shown, and how many of each category would be with the other filters
        std::vector<std::string> words;
        {
            std::string word;
            for (const auto c : filter + " ")
            {
                if (c != ' ')
                    word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                else if (!word.empty())
                    words.push_back(std::exchange(word, {}));
            }
        }
        std::vector<std::size_t> counts(categories.size(), 0);
        std::size_t all = 0, favorites = 0, alike = 0;
        std::vector<const Listed*> shown;
        for (const auto& animation : listed.animations)
        {
            if ((facesOnly && animation.kind != Types::AnimationKind::Face) || (loadedOnly && !inMemory(animation)) ||
                (!from.empty() && listed.headings[animation.heading] != from) || waiting ||
                (fitting && misfits(animation)))
                continue;
            if (!std::ranges::all_of(words, [&](const std::string& word) {
                    return animation.lowerName.find(word) != std::string::npos ||
                           animation.lowerWhere.find(word) != std::string::npos;
                }))
                continue;
            const bool favorite = Components::Actors::IsFavoriteAnimation(animation.name);
            const bool like = isLike(animation);
            ++all;
            favorites += favorite ? 1 : 0;
            alike += like ? 1 : 0;
            ++counts[animation.category];
            if (category == "All" || (category == "Favorites" && favorite) || (category == "Like this" && like) ||
                (animation.category < categories.size() && categories[animation.category] == category))
                shown.push_back(&animation);
        }
        // those like it: the nearest first (in speed, for those that go)
        if (category == "Like this" && reference)
            std::ranges::stable_sort(shown, {}, [&](const Listed* animation) {
                return animation->name == likeThis          ? -1.0f
                       : referenceMoves && animation->entry ? std::abs(speedOf(*animation->entry) - speedOf(*reference))
                                                            : 0.0f;
            });

        // categories
        if (ImGui::BeginChild("##animationCategories", ImVec2(categoryWidth, height), true))
        {
            auto entry = [&](std::string_view name, std::string_view label, std::size_t count) {
                if (ImGui::Selectable(std::format("{} ({})##{}", label, count, name).c_str(), category == name))
                    category = name;
                ImGuiEx::Keyframeable::Record("browser.category." + std::string(name));
            };
            if (reference)
                entry("Like this", ICON_FA_WAND_MAGIC_SPARKLES " Like this: " + likeKind, alike);
            entry("All", facesOnly ? "Faces" : "All", all);
            entry("Favorites", ICON_FA_STAR " Favorites", favorites);
            if (!facesOnly)
            {
                ImGui::Separator();
                for (std::size_t i = 0; i < categories.size(); ++i)
                    entry(categories[i], categories[i], counts[i]);
            }
        }
        ImGui::EndChild();

        // the list
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(listWidth);
        {
            char buffer[128];
            strncpy_s(buffer, filter.c_str(), _TRUNCATE);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            if (ImGui::InputTextWithHint("##animationSearch", ICON_FA_MAGNIFYING_GLASS " Search", buffer,
                                         sizeof(buffer)))
            {
                // (a search looks through every one, as before Like this)
                if (filter.empty() && category == "Like this")
                    category = "All";
                filter = buffer;
            }
            ImGuiEx::Keyframeable::Record("browser.search");
        }
        // where from, and the filters
        ImGui::SetNextItemWidth(fontSize * 11);
        if (ImGui::BeginCombo("##animationFrom", from.empty() ? "All sources" : from.c_str(),
                              ImGuiComboFlags_HeightLarge))
        {
            if (ImGui::Selectable("All sources", from.empty()))
                from.clear();
            ImGuiEx::Keyframeable::Record("browser.from.anywhere");
            for (const auto& heading : listed.headings)
            {
                if (ImGui::Selectable(heading.c_str(), from == heading))
                    from = heading;
                ImGuiEx::Keyframeable::Record("browser.from." + Underscored(heading));
            }
            ImGui::EndCombo();
        }
        ImGuiEx::Keyframeable::Record("browser.from");
        ImGui::SetItemTooltip("Multiplayer's, the campaign's common ones and those of each level, Zombies'...");

        // the rows: a heading wherever they come from somewhere else, then the animations from there
        struct Row
        {
            const Listed* animation = nullptr;
            std::size_t heading = 0;
        };
        std::vector<Row> rows;
        rows.reserve(shown.size() + 32);
        for (const auto animation : shown)
        {
            // (those like it in their order, without where each is from)
            if (category != "Like this" &&
                (rows.empty() ||
                 (rows.back().animation ? rows.back().animation->heading : rows.back().heading) != animation->heading))
                rows.push_back({nullptr, animation->heading});
            rows.push_back({animation, animation->heading});
        }

        hovered.clear();
        const Listed* highlightedListed = nullptr;
        if (ImGui::BeginChild("##animationList",
                              ImVec2(listWidth, height - 2 * ImGui::GetFrameHeightWithSpacing()), true) &&
            ImGui::BeginTable("##animationTable", 3, ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("##favorite", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##duration", ImGuiTableColumnFlags_WidthFixed, fontSize * 4.5f);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto& row = rows[i];
                    ImGui::TableNextRow();
                    if (!row.animation)
                    {
                        // one line as tall as the animations' (the clipper takes every row to be as tall as the
                        // first), with a line after it
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("%s", listed.headings[row.heading].c_str());
                        const auto end = ImGui::GetItemRectMax();
                        const auto y = (ImGui::GetItemRectMin().y + end.y) / 2;
                        ImGui::GetWindowDrawList()->AddLine(ImVec2(end.x + fontSize * 0.5f, y),
                                                            ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), y),
                                                            ImGui::GetColorU32(ImGuiCol_Separator));
                        continue;
                    }
                    const auto& animation = *row.animation;
                    const std::string name(animation.name);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(name.c_str());
                    const bool favorite = Components::Actors::IsFavoriteAnimation(name);
                    ImGui::PushStyleColor(ImGuiCol_Text, favorite ? Theme::Favorite()
                                                                  : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::SmallButton(ICON_FA_STAR))
                        Components::Actors::SetFavoriteAnimation(name, !favorite);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    // one that loads from the animation pack as it is used: with the character picker's icon; one
                    // neither the game nor the pack has, greyed too; one that doesn't fit (shown with the filter off),
                    // marked
                    const bool loadsWhenUsed = !inMemory(animation);
                    const bool misfit = misfits(animation);
                    if (!animation.loaded)
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    else if (misfit)
                        ImGui::PushStyleColor(ImGuiCol_Text, MisfitColor());
                    if (ImGui::Selectable(std::format("{}{}{}##row", misfit ? ICON_FA_BAN " " : "", ShownName(name),
                                                      loadsWhenUsed ? "  " ICON_FA_DOWNLOAD : "")
                                              .c_str(),
                                          highlighted == name, ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        highlighted = name;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && animation.loaded)
                        {
                            choice = name;
                            chosen = true;
                        }
                    }
                    if (!animation.loaded || misfit)
                        ImGui::PopStyleColor();
                    ImGuiEx::Keyframeable::Record("browser." + name);
                    // dragged onto an actor's lane on the timeline, a clip of it there (see Timeline)
                    if (animation.loaded && ImGui::BeginDragDropSource())
                    {
                        ImGui::SetDragDropPayload(ANIMATION_PAYLOAD, name.c_str(), name.size() + 1);
                        ImGui::TextUnformatted(name.c_str());
                        ImGui::TextDisabled("Drop it on an actor's lane on the timeline");
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::IsItemHovered())
                    {
                        hovered = name;
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(fontSize * 24);
                        ImGui::TextUnformatted(name.c_str());
                        ImGui::TextDisabled("%s", DescribeAnimation(animation, listed, &models).c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    ImGui::TableSetColumnIndex(2);
                    const auto duration = animation.entry ? animation.entry->Duration() : animation.loaded->duration;
                    const bool loop = animation.entry ? animation.entry->loop : animation.loaded->loop;
                    ImGui::TextDisabled("%.1f s%s", duration, loop ? " " ICON_FA_REPEAT : "");
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
            if (waiting)
                ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  Loading its models...");
            else if (shown.empty())
                ImGui::TextDisabled(fits && fitting && !loadedOnly && from.empty() && filter.empty() &&
                                            category == "All"
                                        ? "Nothing fits its models"
                                        : "Nothing found");
        }
        ImGui::EndChild();
        ImGui::EndGroup();
        for (const auto& animation : listed.animations)
            if (animation.name == highlighted)
                highlightedListed = &animation;

        // the preview, and what the animation is
        ImGui::SameLine();
        ImGui::BeginGroup();
        const auto& previewed = hovered.empty() ? highlighted : hovered;
        const Listed* previewedListed = nullptr;
        if (previewed == highlighted)
            previewedListed = highlightedListed;
        else
            for (const auto& animation : listed.animations)
                if (animation.name == previewed)
                    previewedListed = &animation;
        const auto infoHeight = ImGui::GetTextLineHeightWithSpacing() * 6;
        DrawAnimationPreview(previewed, body, ImVec2(previewWidth, height - infoHeight - ImGui::GetStyle().ItemSpacing.y));
        if (ImGui::BeginChild("##animationInfo", ImVec2(previewWidth, infoHeight), true))
        {
            ImGui::PushTextWrapPos(0.0f);
            if (previewedListed)
                ImGui::TextDisabled("%s", DescribeAnimation(*previewedListed, listed, &models).c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        ImGui::EndGroup();

        const bool usable = highlightedListed && highlightedListed->loaded;
        ImGui::BeginDisabled(!usable);
        if (ImGui::Button("Apply", ImVec2(fontSize * 6, 0)))
        {
            choice = highlighted;
            chosen = true;
        }
        ImGui::EndDisabled();
        if (highlightedListed && !highlightedListed->loaded)
            ImGui::SetItemTooltip("Neither the game nor the animation pack has it, so it can't be played");
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fontSize * 6, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu animations · double-click to apply · drag onto the timeline", shown.size());
        browserState = {.shown = shown.size(),
                        .listed = listed.animations.size(),
                        .fitting = fitting,
                        .fitsKnown = fits != nullptr,
                        .waiting = waiting,
                        .frame = ImGui::GetFrameCount()};
        ImGui::SetItemTooltip("Every animation of the game is listed (resources/animations.json); what each is comes "
                              "from the game's tables, its bones and its name");

        if (chosen)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return chosen;
    }

    bool DrawThenItems(const Types::Actor& actor, std::uint32_t clipId, const std::string& record)
    {
        const auto clip = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id);
        if (clip == actor.clips.end())
            return false;
        ImGui::TextDisabled("After %s", ShownName(clip->animation).c_str());
        const auto candidates = Components::Actors::GetChainCandidates(actor, clipId);
        if (candidates.empty())
        {
            ImGui::TextDisabled("No transitions");
            return false;
        }
        const auto catalog = Components::Actors::GetCatalog();
        bool chosen = false;
        const auto item = [&](const TransitionLogic::Candidate& candidate, std::size_t i) {
            // (cover arrivals and exits need a cover's wall: never put in on their own)
            const auto& name = candidate.animation;
            const bool cover = name.find("arrive") != std::string::npos || name.find("exit") != std::string::npos ||
                               name.find("trans_in") != std::string::npos ||
                               name.find("trans_out") != std::string::npos || name.find("corner") != std::string::npos ||
                               name.find("cover") != std::string::npos || name.find("pillar") != std::string::npos;
            const auto distance = candidate.distance ? std::format("{:.0f} u", *candidate.distance) : std::string();
            if (ImGui::MenuItem(
                    std::format("{}{}  ({})##then{}", candidate.label, cover ? ", at cover" : "", name, i).c_str(),
                    distance.c_str()))
            {
                ActorActions::PlayAfter(actor.id, clipId, name);
                chosen = true;
            }
            ImGuiEx::Keyframeable::Record(record + "." + name);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            {
                const auto animation = catalog ? catalog->FindAnimation(name) : nullptr;
                const auto pose =
                    candidate.distance
                        ? std::format("\nIts first pose is {:.1f} units from the clip's last{}", *candidate.distance,
                                      *candidate.distance <= TransitionLogic::CLEAN_JOIN ? ": a smooth join" : "")
                        : std::string();
                ImGui::SetTooltip("%s, %.2f s: plays after the clip, the clips after it making room%s", name.c_str(),
                                  animation ? animation->duration : 0.0f, pose.c_str());
            }
        };
        // its style's own first, the game's others under More
        bool others = false;
        for (std::size_t i = 0; i < candidates.size(); ++i)
            if (candidates[i].table)
                item(candidates[i], i);
            else
                others = true;
        if (!others)
            return chosen;
        if (std::ranges::any_of(candidates, &TransitionLogic::Candidate::table))
            ImGui::Separator();
        if (ImGui::BeginMenu("More"))
        {
            for (std::size_t i = 0; i < candidates.size(); ++i)
                if (!candidates[i].table)
                    item(candidates[i], i);
            ImGui::EndMenu();
        }
        else
            ImGuiEx::Keyframeable::Record(record + ".more");
        return chosen;
    }

    // a content pack by the name shown: "Vorkuta (Campaign)"
    std::string DescribePack(const Catalog& catalog, const std::string& name)
    {
        if (name.empty())
            return "Multiplayer only";
        const auto found = catalog.FindPack(name);
        return found ? std::format("{} ({})", found->displayName, found->group) : name;
    }

    // The content packs to load more characters from (campaign levels, Zombies maps), at the top of the body's combo
    // (its end scrolls away): one for the scene at a time, in place of the one before; the multiplayer characters of
    // the map are always there
    void ActorsMenu::RenderPackMenu(const Catalog& catalog)
    {
        const bool open = ImGui::BeginMenu(ICON_FA_BOX_OPEN " More characters...");
        if (!open)
        {
            ImGuiEx::Keyframeable::Record("actors.morecharacters");
            ImGui::Separator();
            return;
        }
        const auto pack = Components::Actors::GetPack();
        ImGui::TextDisabled("The characters of a campaign level or Zombies map,\nand the campaign's animations");
        if (ImGui::MenuItem("Multiplayer only", nullptr, pack.empty()))
            Components::Actors::SetPack({});
        ImGuiEx::Keyframeable::Record("actors.pack.none");
        ImGui::SetItemTooltip("The map's own characters alone");
        std::string group;
        for (const auto& entry : catalog.packs)
        {
            if (entry.group != group)
            {
                group = entry.group;
                ImGui::SeparatorText(group.c_str());
            }
            const bool loading = catalog.loadingPack == entry.name;
            if (ImGui::MenuItem(std::format("{}##{}", entry.displayName, entry.name).c_str(),
                                loading ? "loading" : nullptr, pack == entry.name))
                Components::Actors::SetPack(entry.name);
            ImGuiEx::Keyframeable::Record("actors.pack." + entry.name);
            ImGui::SetItemTooltip("Loads its characters in place of the pack loaded before; it takes a few seconds");
        }
        ImGui::EndMenu();
        ImGui::Separator();
    }

    // what the scene's content pack is doing: loading, loaded or failing
    void ActorsMenu::RenderPackState(const Catalog& catalog)
    {
        const auto pack = Components::Actors::GetPack();
        if (!catalog.loadingPack.empty())
            ImGui::TextDisabled(ICON_FA_BOX_OPEN "  Loading the characters of %s...",
                                DescribePack(catalog, catalog.loadingPack).c_str());
        else if (!catalog.packError.empty())
            ImGui::TextColored(Theme::Misfit(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                               catalog.packError.c_str());
        else if (!catalog.loadedPack.empty())
        {
            ImGui::TextDisabled(ICON_FA_BOX "  With the characters of %s",
                                DescribePack(catalog, catalog.loadedPack).c_str());
            ImGui::SetItemTooltip(
                "The game uses %d MB of memory with them. More characters... in Body loads "
                "others.",
                catalog.packMemoryMegabytes);
        }
        else if (!pack.empty())
            ImGui::TextDisabled(ICON_FA_BOX "  %s not loaded yet", DescribePack(catalog, pack).c_str());
    }

    // on the ground where the camera looks, facing the camera: where it stands, its key at the timeline's tick, or
    // a point of its path
    bool EffectBrowser(const char* popupId, const Catalog& catalog, std::string& choice, std::string& filter,
                       std::string& category, const char* use)
    {
        bool chosen = false;
        const auto fontSize = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopup(popupId))
            return false;
        static std::string highlighted;
        if (ImGui::IsWindowAppearing())
            highlighted = choice;
        const auto height = fontSize * 22;
        const auto categoryWidth = fontSize * 11;
        const auto listWidth = fontSize * 30;
        const auto& categories = ActorLogic::GetEffectCategories();
        const auto recent = Components::Actors::GetRecentEffects();
        if (category != "All" && category != "Favorites" && category != "Recent" &&
            std::ranges::find(categories, category) == categories.end())
            category = "All";

        // what is shown, and how many of each category would be with the search
        std::vector<std::string> words;
        {
            std::string word;
            for (const auto c : filter + " ")
            {
                if (c != ' ')
                    word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                else if (!word.empty())
                    words.push_back(std::exchange(word, {}));
            }
        }
        std::map<std::string_view, std::size_t> counts;
        std::size_t all = 0, favorites = 0, recents = 0;
        std::vector<const std::string*> shown;
        for (const auto& name : catalog.effects)
        {
            if (!std::ranges::all_of(words, [&](const std::string& word) { return MatchesFilter(name, word); }))
                continue;
            const auto in = ActorLogic::GetEffectCategory(name);
            const bool favorite = Components::Actors::IsFavoriteEffect(name);
            const bool isRecent = std::ranges::find(recent, name) != recent.end();
            ++all;
            favorites += favorite ? 1 : 0;
            recents += isRecent ? 1 : 0;
            ++counts[in];
            if (category == "All" || (category == "Favorites" && favorite) || (category == "Recent" && isRecent) ||
                in == category)
                shown.push_back(&name);
        }
        // the recent ones, the latest first
        if (category == "Recent")
            std::ranges::stable_sort(shown, {}, [&](const std::string* name) {
                return std::ranges::find(recent, *name) - recent.begin();
            });

        if (ImGui::BeginChild("##effectCategories", ImVec2(categoryWidth, height), true))
        {
            auto entry = [&](std::string_view name, std::string_view label, std::size_t count) {
                if (ImGui::Selectable(std::format("{} ({})##{}", label, count, name).c_str(), category == name))
                    category = name;
                ImGuiEx::Keyframeable::Record("effects.category." + Underscored(std::string(name)));
            };
            entry("All", "All", all);
            entry("Favorites", ICON_FA_STAR " Favorites", favorites);
            entry("Recent", ICON_FA_CLOCK_ROTATE_LEFT " Recent", recents);
            ImGui::Separator();
            for (const auto name : categories)
                entry(name, name, counts[name]);
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(listWidth);
        {
            char buffer[128];
            strncpy_s(buffer, filter.c_str(), _TRUNCATE);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            if (ImGui::InputTextWithHint("##effectSearch", ICON_FA_MAGNIFYING_GLASS " Search: fire, smoke, explosion",
                                         buffer, sizeof(buffer)))
                filter = buffer;
            ImGuiEx::Keyframeable::Record("effects.search");
        }
        if (ImGui::BeginChild("##effectList", ImVec2(listWidth, height - 2 * ImGui::GetFrameHeightWithSpacing()),
                              true) &&
            ImGui::BeginTable("##effectTable", 4, ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("##favorite", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##folder", ImGuiTableColumnFlags_WidthFixed, fontSize * 8.0f);
            ImGui::TableSetupColumn("##plays", ImGuiTableColumnFlags_WidthFixed, fontSize * 3.2f);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto& name = *shown[i];
                    const auto info = catalog.effectInfo.find(name);
                    const auto plays = info != catalog.effectInfo.end() ? info->second : Catalog::EffectInfo{};
                    const auto slash = name.find_last_of('/');
                    ImGui::TableNextRow();
                    ImGui::PushID(name.c_str());
                    ImGui::TableSetColumnIndex(0);
                    const bool favorite = Components::Actors::IsFavoriteEffect(name);
                    ImGui::PushStyleColor(ImGuiCol_Text, favorite ? Theme::Favorite()
                                                                  : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::SmallButton(ICON_FA_STAR))
                        Components::Actors::SetFavoriteEffect(name, !favorite);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::Selectable(std::format("{}##row", name.substr(slash + 1)).c_str(), highlighted == name,
                                          ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        highlighted = name;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            choice = name;
                            chosen = true;
                        }
                    }
                    ImGuiEx::Keyframeable::Record("effects." + name);
                    ImGui::SetItemTooltip("%s\n%s", name.c_str(), ActorLogic::DescribeEffect(plays).c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextDisabled("%s", slash == std::string::npos ? "" : name.substr(0, slash).c_str());
                    ImGui::TableSetColumnIndex(3);
                    if (ActorLogic::EffectLoops(plays))
                        ImGui::TextColored(Theme::Note(), "Loops");
                    else
                        ImGui::TextDisabled("Once");
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
            if (shown.empty())
                ImGui::TextDisabled("None here: try another word, or All");
        }
        ImGui::EndChild();

        ImGui::BeginDisabled(highlighted.empty());
        if (ImGui::Button(use))
        {
            choice = highlighted;
            chosen = true;
        }
        ImGui::EndDisabled();
        ImGuiEx::Keyframeable::Record("effects.use");
        ImGui::SameLine();
        ImGui::TextDisabled("Double-click places it; " ICON_FA_STAR " keeps one in Favorites");
        ImGui::EndGroup();
        if (chosen)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return chosen;
    }

    bool PropBrowser(const char* popupId, const Catalog& catalog, std::string& choice, std::string& filter,
                     std::string& group, const char* use, const PropPreviewRay& preview, ImVec2 keepClear)
    {
        namespace PropLogic = Components::PropLogic;
        bool chosen = false;
        const auto fontSize = ImGui::GetFontSize();
        // (beside where the model shows, not over it)
        const auto viewport = ImGui::GetMainViewport();
        const bool right = keepClear.x < viewport->GetCenter().x;
        ImGui::SetNextWindowPos(
            ImVec2(right ? viewport->Pos.x + viewport->Size.x - fontSize : viewport->Pos.x + fontSize,
                   viewport->GetCenter().y),
            ImGuiCond_Appearing, ImVec2(right ? 1.0f : 0.0f, 0.5f));
        if (!ImGui::BeginPopup(popupId))
            return false;
        static std::string highlighted;
        if (ImGui::IsWindowAppearing())
            highlighted = choice;
        const auto height = fontSize * 22;
        const auto groupWidth = fontSize * 10;
        const auto listWidth = fontSize * 22;
        const auto current = [&] {
            const auto found = std::ranges::find_if(
                PropLogic::ALL_GROUPS, [&](PropLogic::Group each) { return PropLogic::GetGroupKey(each) == group; });
            return found != PropLogic::ALL_GROUPS.end() ? *found : PropLogic::Group::Map;
        }();

        ImGui::SetNextItemWidth(groupWidth + listWidth + ImGui::GetStyle().ItemSpacing.x);
        {
            char buffer[128];
            strncpy_s(buffer, filter.c_str(), _TRUNCATE);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            if (ImGui::InputTextWithHint("##propSearch", ICON_FA_MAGNIFYING_GLASS " Search", buffer, sizeof(buffer)))
                filter = buffer;
            ImGuiEx::Keyframeable::Record("props.search");
        }
        const bool searching = filter.find_first_not_of(' ') != std::string::npos;
        const auto counts = PropLogic::CountGroups(catalog.props, catalog.mapProps);
        if (ImGui::BeginChild("##propGroups", ImVec2(groupWidth, height), true))
        {
            for (const auto each : PropLogic::ALL_GROUPS)
            {
                const auto key = PropLogic::GetGroupKey(each);
                if (ImGui::Selectable(std::format("{} ({})##{}", PropLogic::GetGroupName(each),
                                                  counts[static_cast<std::size_t>(each)], key)
                                          .c_str(),
                                      !searching && each == current))
                {
                    group = key;
                    filter.clear();
                }
                ImGuiEx::Keyframeable::Record(std::format("props.group.{}", key));
                if (each == PropLogic::Group::Map)
                    ImGui::Separator();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();
        const auto shown = PropLogic::GetShown(catalog.props, catalog.mapProps, current, filter);
        std::optional<std::string> hovered;
        if (ImGui::BeginChild("##propList", ImVec2(listWidth, height), true))
        {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(shown.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto& name = shown[i];
                    if (ImGui::Selectable(std::format("{}##prop", name).c_str(), highlighted == name,
                                          ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        highlighted = name;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            choice = name;
                            chosen = true;
                        }
                    }
                    ImGuiEx::Keyframeable::Record("props." + name);
                    if (ImGui::IsItemHovered())
                        hovered = name;
                }
            }
            if (shown.empty())
                ImGui::TextDisabled("None here: try another word");
        }
        ImGui::EndChild();
        // the row under the mouse stands where it would go till the mouse leaves it
        if (hovered)
            Components::Props::SetPreview({*hovered, preview.origin, preview.direction, preview.yaw});
        if (!chosen && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        {
            const auto kept = hovered.value_or(highlighted);
            if (!kept.empty())
            {
                choice = kept;
                chosen = true;
            }
        }

        ImGui::BeginDisabled(highlighted.empty());
        if (ImGui::Button(use))
        {
            choice = highlighted;
            chosen = true;
        }
        ImGui::EndDisabled();
        ImGuiEx::Keyframeable::Record("props.use");
        ImGui::SameLine();
        ImGui::TextDisabled("%zu models. Hover one to see it", shown.size());
        if (chosen)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return chosen;
    }

    void PlaceInFrontOfCamera(const Types::Actor& actor, std::optional<std::size_t> waypoint = std::nullopt)
    {
        const auto& camera = Components::CameraManager::Get().GetActiveCamera();
        const auto yaw = std::remainder(camera->GetRotation().y + 180.0f, 360.0f);
        Components::Actors::RequestPlacement(actor.id, camera->GetPosition(), camera->GetForwardVector(), waypoint,
                                             yaw);
    }

    void ActorsMenu::RenderList()
    {
        const auto catalog = Components::Actors::GetCatalog();
        const auto actors = Components::Actors::GetActors();
        const auto selected = Components::Actors::GetSelected();
        const auto addHere = [](Types::ActorKind kind) {
            const auto& camera = Components::CameraManager::Get().GetActiveCamera();
            ActorActions::AddActor(camera->GetPosition(), camera->GetForwardVector(), kind);
        };
        const auto addLight = [](Types::LightKind kind) {
            const auto& camera = Components::CameraManager::Get().GetActiveCamera();
            ActorActions::AddLight(kind, camera->GetPosition(), camera->GetForwardVector());
        };
        const auto lightMenu = [&] {
            if (ImGui::MenuItem("Point"))
                addLight(Types::LightKind::Point);
            ImGuiEx::Keyframeable::Record("actors.add.light.point");
            ImGui::SetItemTooltip("Lights all around it, just off what the camera looks at, from the playhead");
            if (ImGui::MenuItem("Spot"))
                addLight(Types::LightKind::Spot);
            ImGuiEx::Keyframeable::Record("actors.add.light.spot");
            ImGui::SetItemTooltip("A cone from the camera onto what it looks at, from the playhead");
        };
        // the killstreaks and a dog, where the camera looks
        const auto killstreakMenu = [&] {
            for (const auto& info : Components::KillstreakLogic::GetAll())
            {
                const auto unavailable = ActorActions::GetKillstreakUnavailableReason(info.killstreak);
                if (ImGui::MenuItem(std::string(info.name).c_str(), nullptr, false, !unavailable))
                {
                    const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                    ActorActions::AddKillstreak(info.killstreak, camera->GetPosition(), camera->GetForwardVector());
                }
                ImGuiEx::Keyframeable::Record("actors.add.killstreak." +
                                              ActorActions::GetKillstreakKey(info.killstreak));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", unavailable ? unavailable->c_str()
                                                        : ActorActions::GetKillstreakTooltip(info.killstreak).c_str());
            }
            // (no model: an actor of its own)
            const auto mortar = ActorActions::GetMortarUnavailableReason();
            if (ImGui::MenuItem(std::string(Components::KillstreakLogic::MORTAR_NAME).c_str(), nullptr, false, !mortar))
            {
                const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                ActorActions::AddMortar(camera->GetPosition(), camera->GetForwardVector());
            }
            ImGuiEx::Keyframeable::Record("actors.add.killstreak.mortarteam");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("%s", mortar
                                            ? mortar->c_str()
                                            : "Three points on what the camera looks at, its shells falling on them");
            // (an actor of its own kind, listed where a player gets them)
            const auto dog = ActorActions::GetAddUnavailableReason(Types::ActorKind::Dog);
            if (ImGui::MenuItem("Dog", nullptr, false, !dog))
                addHere(Types::ActorKind::Dog);
            ImGuiEx::Keyframeable::Record("actors.add.dog");
            ImGui::SetItemTooltip("%s", dog ? dog->c_str() : "The map's german shepherd, where the camera looks");
        };
        // Add: an actor, a dog or a killstreak where the camera looks, at once
        constexpr auto ADD_POPUP = "##addActor";
        const auto addMenu = [&] {
            const auto add = ActorActions::GetAddUnavailableReason();
            if (ImGui::MenuItem(ICON_FA_USER_PLUS " Add Actor", nullptr, false, !add))
                addHere(Types::ActorKind::Human);
            ImGuiEx::Keyframeable::Record("actors.add.actor");
            ImGui::SetItemTooltip("%s", add ? add->c_str() : "A person of the map, where the camera looks");
            if (ImGui::BeginMenu(ICON_FA_HELICOPTER " Add Killstreak"))
            {
                killstreakMenu();
                ImGui::EndMenu();
            }
            else
                ImGuiEx::Keyframeable::Record("actors.add.killstreak");
            if (ImGui::MenuItem(ICON_FA_WAND_MAGIC_SPARKLES " Add Effect..."))
            {
                openEffectBrowser = true;
                effectBrowserAdds = true;
                effectBrowsed.clear();
            }
            ImGuiEx::Keyframeable::Record("actors.add.effect");
            ImGui::SetItemTooltip("One of the map's effects, on what the camera looks at, from the playhead");
            // (a model of the map on its own)
            if (ImGui::MenuItem(ICON_FA_CUBE " Add Prop..."))
                AddProp();
            ImGuiEx::Keyframeable::Record("actors.add.prop");
            ImGui::SetItemTooltip("One of the map's models, on what the camera looks at, from the playhead");
            if (ImGui::BeginMenu(ICON_FA_LIGHTBULB " Add Light"))
            {
                lightMenu();
                ImGui::EndMenu();
            }
            else
                ImGuiEx::Keyframeable::Record("actors.add.light");
        };

        // with none: Add Actor across the page, what else can be added in an even row under it, then the other way
        if (actors.empty())
        {
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            const auto add = ActorActions::GetAddUnavailableReason();
            ImGui::BeginDisabled(add.has_value());
            if (ImGui::Button(ICON_FA_USER_PLUS "  Add Actor", ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.3f)))
                addHere(Types::ActorKind::Human);
            ImGui::EndDisabled();
            ImGui::PopStyleColor();
            // (as Add's Add Actor, for the harness)
            ImGuiEx::Keyframeable::Record("actors.add.actor");
            if (add)
                ImGui::SetItemTooltip("%s", add->c_str());

            const auto spacing = ImGui::GetStyle().ItemSpacing.x;
            const auto third = ImVec2(std::floor((ImGui::GetContentRegionAvail().x - spacing * 3.0f) / 4.0f), 0.0f);
            if (ImGui::Button("Killstreak " ICON_FA_CARET_DOWN, third))
                ImGui::OpenPopup("##addKillstreak");
            ImGuiEx::Keyframeable::Record("actors.add.killstreak");
            ImGui::SetItemTooltip("A killstreak or a dog, where the camera looks");
            if (ImGui::BeginPopup("##addKillstreak"))
            {
                killstreakMenu();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            // (an effect on its own, as Add's Add Effect...)
            if (ImGui::Button("Effect...", third))
            {
                openEffectBrowser = true;
                effectBrowserAdds = true;
                effectBrowsed.clear();
            }
            ImGuiEx::Keyframeable::Record("actors.add.effect");
            ImGui::SetItemTooltip("One of the map's effects, on what the camera looks at, from the playhead");
            ImGui::SameLine();
            // (a prop on its own, as Add's Add Prop...)
            if (ImGui::Button("Prop...", third))
                AddProp();
            ImGuiEx::Keyframeable::Record("actors.add.prop");
            ImGui::SetItemTooltip("One of the map's models, on what the camera looks at, from the playhead");
            ImGui::SameLine();
            // (a light on its own, as Add's Add Light)
            if (ImGui::Button("Light " ICON_FA_CARET_DOWN, third))
                ImGui::OpenPopup("##addLight");
            ImGuiEx::Keyframeable::Record("actors.add.light");
            if (ImGui::BeginPopup("##addLight"))
            {
                lightMenu();
                ImGui::EndPopup();
            }

            const auto hint = "or right-click the ground in the view";
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(hint).x) / 2.0f);
            ImGui::TextDisabled("%s", hint);
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
            return;
        }

        // the row on top: Add (each section has its own Advanced, on its header)
        if (ImGui::Button(ICON_FA_USER_PLUS " Add " ICON_FA_CARET_DOWN))
            ImGui::OpenPopup(ADD_POPUP);
        ImGuiEx::Keyframeable::Record("actors.add");
        ImGui::SetItemTooltip("Adds an actor, a dog or a killstreak where the camera looks; or right-click the ground");
        if (ImGui::BeginPopup(ADD_POPUP))
        {
            addMenu();
            ImGui::EndPopup();
        }

        std::optional<std::uint32_t> hovered;
        const auto spacing = ImGui::GetStyle().ItemSpacing.x;
        // the actors, then the effects in the scene under a heading of their own, its
        // props under theirs, then its lights
        std::vector<const Types::Actor*> listed;
        for (const auto& actor : actors)
            if (!actor.effect && !actor.light && !actor.placedProp)
                listed.push_back(&actor);
        const auto firstEffect = listed.size();
        for (const auto& actor : actors)
            if (actor.effect)
                listed.push_back(&actor);
        const auto firstProp = listed.size();
        for (const auto& actor : actors)
            if (actor.placedProp)
                listed.push_back(&actor);
        const auto firstLight = listed.size();
        for (const auto& actor : actors)
            if (actor.light)
                listed.push_back(&actor);
        const auto rows =
            std::clamp<std::size_t>(actors.size() + (firstEffect < firstProp ? 1 : 0) +
                                        (firstProp < firstLight ? 1 : 0) + (firstLight < listed.size() ? 1 : 0),
                                    2, 5);
        if (ImGui::BeginChild("##actorList", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * rows + 8), true))
        {
            for (std::size_t row = 0; row < listed.size(); ++row)
            {
                const auto& actor = *listed[row];
                if (row == firstEffect && firstEffect < firstProp)
                    ImGui::TextDisabled(ICON_FA_WAND_MAGIC_SPARKLES " Effects");
                if (row == firstProp && firstProp < firstLight)
                    ImGui::TextDisabled(ICON_FA_CUBE " Props");
                if (row == firstLight)
                    ImGui::TextDisabled(ICON_FA_LIGHTBULB " Lights");
                ImGui::PushID(static_cast<int>(actor.id));
                // (an effect the map hasn't: it doesn't play)
                const bool missing = actor.effect ? !catalog->effectInfo.contains(actor.effect->name)
                                     : actor.placedProp
                                         ? !std::ranges::binary_search(catalog->props, actor.placedProp->model)
                                         : std::ranges::any_of(actor.models, [&](const std::string& model) {
                                               return !catalog->FindModel(model);
                                           });
                const bool leftOut = !missing && Components::Actors::GetLeftOut(actor.id);
                const auto eyeWidth = ImGui::GetFrameHeight();
                // with character zones, a character that isn't loaded is loading
                const auto icon =
                    missing && !actor.effect && !actor.placedProp && Components::Actors::HasCharacterZones()
                        ? ICON_FA_HOURGLASS_HALF " "
                    : missing || leftOut ? ICON_FA_TRIANGLE_EXCLAMATION " "
                    : actor.effect       ? ICON_FA_WAND_MAGIC_SPARKLES " "
                    : actor.placedProp   ? ICON_FA_CUBE " "
                    : actor.mortar       ? ICON_FA_BOMB " "
                                         : "";
                const auto width = ImGui::GetContentRegionAvail().x - eyeWidth - spacing;
                if (renaming == actor.id)
                {
                    // typing its name in its place: Enter or a click elsewhere keeps it, Escape doesn't
                    if (renameFocus)
                        ImGui::SetKeyboardFocusHere();
                    renameFocus = false;
                    char buffer[64];
                    strncpy_s(buffer, renameBuffer.c_str(), _TRUNCATE);
                    ImGui::SetNextItemWidth(width);
                    if (ImGui::InputText("##rename", buffer, sizeof(buffer), ImGuiInputTextFlags_AutoSelectAll))
                        renameBuffer = buffer;
                    if (ImGui::IsItemDeactivated())
                    {
                        if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && !renameBuffer.empty() &&
                            renameBuffer != actor.name)
                        {
                            auto renamed = actor;
                            renamed.name = renameBuffer;
                            Components::Actors::Update(renamed, false, "rename " + actor.name);
                        }
                        renaming.reset();
                    }
                }
                else
                {
                    // (a light's bulb goes in its own colour over the room left for it)
                    const auto bulbRoom =
                        actor.light ? std::string(static_cast<std::size_t>(std::ceil(
                                                      ImGui::CalcTextSize(ICON_FA_LIGHTBULB " ").x /
                                                      ImGui::CalcTextSize(" ").x)),
                                                  ' ')
                                    : std::string();
                    if (ImGui::Selectable(std::format("{}{}{}", bulbRoom, icon, actor.name).c_str(),
                                          selected == actor.id,
                                          ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                                          ImVec2(width, 0)))
                    {
                        Components::Actors::Select(actor.id);
                        page = Page::Look;
                        // back from a campath node to the actor
                        GFX::GraphicsManager::Get().FocusActor();
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            renaming = actor.id;
                            renameBuffer = actor.name;
                            renameFocus = true;
                        }
                    }
                    ImGuiEx::Keyframeable::Record(std::format("actors.list.{}", actor.id));
                    if (ImGui::IsItemHovered())
                        hovered = actor.id;
                    if (actor.light)
                    {
                        // (bright enough to see, however dark the light is)
                        const auto color =
                            actor.light->color /
                            std::max({actor.light->color.r, actor.light->color.g, actor.light->color.b, 0.01f});
                        ImGui::GetWindowDrawList()->AddText(ImGui::GetItemRectMin(),
                                                            ImGui::GetColorU32(ImVec4(color.r, color.g, color.b, 1.0f)),
                                                            ICON_FA_LIGHTBULB);
                    }
                    // an effect: whether it loops, at the row's right
                    if (actor.effect && !missing)
                    {
                        const auto info = catalog->effectInfo.find(actor.effect->name)->second;
                        const auto plays = ActorLogic::PlaysLooping(*actor.effect, info) ? "loops" : "once";
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(ImGui::GetItemRectMax().x - ImGui::CalcTextSize(plays).x - spacing,
                                   ImGui::GetItemRectMin().y),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), plays);
                    }
                    if (actor.effect && missing)
                        ImGui::SetItemTooltip("This map hasn't %s: it doesn't play", actor.effect->name.c_str());
                    else if (actor.placedProp && missing)
                        ImGui::SetItemTooltip("This map hasn't %s: it isn't drawn", actor.placedProp->model.c_str());
                    else if (missing)
                        ImGui::SetItemTooltip(
                            Components::Actors::HasCharacterZones()
                                ? "Its character is loading"
                                : "Its character isn't loaded yet: it loads with its level's characters");
                    else if (leftOut)
                        ImGui::SetItemTooltip("Part of it is hidden: too many bones (see Look)");
                    // right-click: what can be done to it
                    if (ImGui::BeginPopupContextItem("##actorMenu"))
                    {
                        RenderActorMenu(actor);
                        ImGui::EndPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(actor.hidden ? ICON_FA_EYE_SLASH : ICON_FA_EYE))
                {
                    auto changed = actor;
                    changed.hidden = !changed.hidden;
                    Components::Actors::Update(changed);
                }
                ImGui::SetItemTooltip(actor.hidden ? "Show" : "Hide");
                ImGui::PopID();
            }
            // right-click below the names: add one
            if (ImGui::BeginPopupContextWindow("##listAdd",
                                               ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
            {
                addMenu();
                ImGui::EndPopup();
            }
            // F2 renames the selected one, with the mouse over the list; Delete and Ctrl+D act on the actor there
            if (ImGui::IsWindowHovered())
            {
                page = Page::Look;
                if (selected && !renaming && ImGui::IsKeyPressed(ImGuiKey_F2, false))
                    if (const auto actor = Components::Actors::GetActor(*selected))
                    {
                        renaming = actor->id;
                        renameBuffer = actor->name;
                        renameFocus = true;
                    }
            }
        }
        ImGui::EndChild();
        // the name under the mouse lights its actor up in the view
        GFX::GraphicsManager::Get().SetListHoveredActor(hovered);
    }

    // What the right-click menu of an actor's name does
    void ActorsMenu::RenderActorMenu(const Types::Actor& actor)
    {
        if (ImGui::MenuItem(ICON_FA_PEN " Rename", "F2"))
        {
            renaming = actor.id;
            renameBuffer = actor.name;
            renameFocus = true;
        }
        ImGuiEx::Keyframeable::Record("actors.list.rename");
        if (ImGui::MenuItem(ICON_FA_CLONE " Duplicate", Commands::GetShortcut("edit.duplicate").c_str()))
            ActorActions::Duplicate(actor.id);
        ImGuiEx::Keyframeable::Record("actors.list.duplicate");
        if (ImGui::MenuItem(ICON_FA_TRASH " Delete", Commands::GetShortcut("edit.delete").c_str()))
            ActorActions::Delete(actor.id);
        ImGuiEx::Keyframeable::Record("actors.list.delete");
        ImGui::Separator();
        if (ImGui::MenuItem(actor.hidden ? ICON_FA_EYE " Show" : ICON_FA_EYE_SLASH " Hide"))
        {
            auto changed = actor;
            changed.hidden = !changed.hidden;
            Components::Actors::Update(changed);
        }
        ImGuiEx::Keyframeable::Record("actors.list.hide");
        if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Go to it"))
        {
            // the free camera a few steps in front of it, looking at it
            Components::Actors::Select(actor.id);
            const auto placement = Components::Actors::EvaluatePlacement(actor, Components::Playback::GetActorTick());
            Commands::Run("camera.free");
            auto& camera = Components::CameraManager::Get().GetActiveCamera();
            const auto yaw = glm::radians(placement.yaw);
            camera->GetPosition() =
                placement.position + glm::vec3(std::cos(yaw), std::sin(yaw), 0.0f) * 150.0f + glm::vec3(0, 0, 60);
            camera->GetRotation() = glm::vec3(12.0f, std::remainder(placement.yaw + 180.0f, 360.0f), 0.0f);
        }
        ImGuiEx::Keyframeable::Record("actors.list.goto");
        if (Components::Actors::GetActorKind(actor) == Types::ActorKind::Human &&
            ImGui::BeginMenu(ICON_FA_USER_PEN " Copy look to player"))
        {
            const auto players = GetPlayers();
            if (players.empty())
                ImGui::TextDisabled("Finding players...");
            for (const auto& [client, name] : players)
                if (ImGui::MenuItem(std::format("{}##{}", name, client).c_str()))
                {
                    const auto error = Components::Actors::CopyLookToPlayer(actor, client);
                    Toasts::Show(error.empty() ? std::format("{} looks like {} now", name, actor.name) : error);
                }
            ImGui::EndMenu();
        }
    }

    std::vector<std::pair<std::int32_t, std::string>> GetPlayers();

    // what the game left out of the actor, as it has more bones than the game can draw (e.g. from a scene, a preset
    // or with the gun in its hand)
    void RenderLeftOut(const Types::Actor& actor, const Catalog& catalog)
    {
        const auto leftOut = Components::Actors::GetLeftOut(actor.id);
        if (!leftOut)
            return;
        std::string names;
        for (const auto& model : leftOut->models)
            names += (names.empty() ? "" : ", ") + Components::Actors::DescribeModel(model);
        if (leftOut->gun)
            names += names.empty() ? "the gun" : " and the gun";
        if (leftOut->stowed)
            names += names.empty() ? "the stowed gun" : " and the stowed gun";
        // (a campaign body playing the players' animations has the 5 bones of the spine rig they play on)
        const auto character = Components::Actors::GetCharacter(actor, catalog);
        const bool rig = character && !character->level.empty() &&
                         std::ranges::any_of(actor.clips, [&](const Types::ActorClip& clip) {
                             const auto animation = catalog.FindAnimation(clip.animation);
                             return animation && animation->family == Types::AnimationFamily::MpPlayer;
                         });
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Misfit());
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  Too many bones: %u of %u%s. Hidden: %s", leftOut->bones,
                           catalog.maxBones, rig ? " (5 are the spine)" : "", names.c_str());
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(
            "The game draws an actor's body, head, gear and gun as one model of at most %u bones%s. Choose a smaller "
            "head, no gear, or no head with a full body",
            catalog.maxBones,
            rig ? "; 5 of them are the spine the players' animations play on a campaign body with" : "");
    }

    namespace
    {
        // A killstreak's effects: a row each, on until turned off, as the game
        // runs them (the rotors, lights and exhausts, its dust and treads)
        void RenderKillstreakEffects(Types::Actor& actor, const Components::KillstreakLogic::Info& info)
        {
            namespace Killstreaks = Components::KillstreakLogic;
            const auto rows = Killstreaks::GetEffectRows(info);
            if (rows.empty())
                return;
            ImGui::SeparatorText("Effects");
            for (const auto row : rows)
            {
                const auto key = std::string(Killstreaks::GetEffectRowKey(row));
                const auto start = ImGui::GetCursorPosX();
                bool on = Killstreaks::IsEffectRowOn(actor, row);
                if (ImGui::Checkbox(std::format("{}##effect{}", Killstreaks::GetEffectRowName(row), key).c_str(), &on))
                    Killstreaks::SetEffectRowOn(actor, row, on);
                ImGuiEx::Keyframeable::Record("actors.effect." + key);
                ImGui::SetItemTooltip("%s, as the game shows it; off, it doesn't run",
                                      std::string(Killstreaks::DescribeEffectRow(row)).c_str());
                ImGui::SameLine(start + ImGui::GetFontSize() * 8.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", std::string(Killstreaks::DescribeEffectRow(row)).c_str());
                ImGui::PopTextWrapPos();
            }
        }

        // Damaged: a helicopter's engine smoking, light or heavy, from a time on
        void RenderDamaged(Types::Actor& actor, const Components::KillstreakLogic::Info& info)
        {
            if (!Components::KillstreakLogic::CanBeDamaged(info))
                return;
            Label("Damaged");
            constexpr std::array<const char*, 3> LEVELS{"Off", "Light", "Heavy"};
            auto level = static_cast<int>(actor.damage);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##actorDamage", &level, LEVELS.data(), static_cast<int>(LEVELS.size())))
            {
                // (from the playhead, when it starts)
                if (actor.damage == Types::ActorDamage::None)
                    actor.damagedFrom = Components::Playback::GetActorTick();
                actor.damage = static_cast<Types::ActorDamage>(level);
            }
            ImGuiEx::Keyframeable::Record("actors.damage");
            ImGui::SetItemTooltip("Smoke from its engine, as the game shows a damaged helicopter, light or heavy");
            if (actor.damage == Types::ActorDamage::None)
                return;
            Label("From");
            WhenControl("##damagedFrom", actor.damagedFrom);
            ImGuiEx::Keyframeable::Record("actors.damage.from");
        }

        // A killstreak's Spin out: the spin it spins up to and the speed it flies
        // on at, each the killstreak's own (Auto, said in the value) until changed; Auto gives it back
        void RenderSpinOut(const Types::Actor& actor, Types::ActorClip& clip)
        {
            namespace Killstreaks = Components::KillstreakLogic;
            const auto killstreak = Killstreaks::FindByModels(actor.models);
            if (!killstreak || clip.animation != Killstreaks::MOTION_SPINOUT)
                return;
            const auto& info = Killstreaks::Get(*killstreak);
            // own: the clip's value (0 for Auto); shown in the row's unit (scale of it: units a second per mph)
            const auto row = [&](const char* label, const std::string& id, float& own, float automatic, float least,
                                 float most, float scale, const char* unit, const std::string& tooltip) {
                Label(label);
                const auto& style = ImGui::GetStyle();
                const auto autoWidth = ImGui::CalcTextSize("Auto").x + style.FramePadding.x * 2;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - autoWidth - style.ItemSpacing.x);
                auto shown = (own > 0.0f ? own : automatic) / scale;
                const auto format = std::format("%.0f {}{}", unit, own > 0.0f ? "" : " · Auto");
                if (ImGui::SliderFloat(("##" + id).c_str(), &shown, least, most, format.c_str(),
                                       ImGuiSliderFlags_AlwaysClamp))
                    own = std::max(shown * scale, 0.01f);
                ImGuiEx::Keyframeable::Record("actors.clip." + id);
                ImGui::SetItemTooltip("%s", tooltip.c_str());
                ImGui::SameLine();
                ImGui::BeginDisabled(own <= 0.0f);
                if (ImGui::Button(("Auto##" + id).c_str(), ImVec2(autoWidth, 0)))
                    own = 0.0f;
                ImGui::EndDisabled();
                ImGuiEx::Keyframeable::Record("actors.clip." + id + ".auto");
                ImGui::SetItemTooltip("The %s's own: %.0f %s", std::string(info.name).c_str(), automatic / scale, unit);
            };
            row("Spin", "spin", clip.spinRate, info.spinRate, 30.0f, 360.0f, 1.0f, "°/s",
                std::format("How fast it spins once it has spun up, over 3 s (the game: {:.0f}-{:.0f} degrees a second)",
                            Killstreaks::GAME_SPIN_LEAST, Killstreaks::GAME_SPIN_MOST));
            row("Speed", "crashspeed", clip.crashSpeed, info.crashSpeed, 0.0f, 100.0f, Killstreaks::FromMph(1.0f), "mph",
                std::format("How fast it flies on as it spins (the game: {:.0f}-{:.0f} mph)",
                            Killstreaks::GAME_CRASH_LEAST / Killstreaks::FromMph(1.0f),
                            Killstreaks::GAME_CRASH_MOST / Killstreaks::FromMph(1.0f)));
        }
    }  // namespace

    void ActorsMenu::RenderLook(Types::Actor& actor, const Catalog& catalog)
    {
        // a charred twin is shown as its character, burnt
        auto current = Components::Actors::GetCharacter(actor, catalog);
        if (current && !current->burntOf.empty())
            if (const auto base = catalog.FindCharacter(current->burntOf))
                current = base;
        Label("Character");
        RenderCharacterPicker(actor, catalog, current);
        if (current)
            RenderCharacterLook(actor, catalog, *current);
        RenderLeftOut(actor, catalog);
        if (Components::Actors::HasCharacterZones())
            RenderZoneState(actor, catalog);
        else
        {
            RenderPackState(catalog);
            RenderZoneSetup();
        }

        // a dog has no face of a person's and holds nothing (what its clips do is the dogs' own), nor a killstreak
        const bool person = Components::Actors::GetActorKind(actor) == Types::ActorKind::Human;
        if (person)
        {
            RenderFacePaint(actor, catalog);
            RenderWeapon(actor);
            RenderStowed(actor);
        }
        const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models);
        if (killstreak)
            RenderKillstreakEffects(actor, Components::KillstreakLogic::Get(*killstreak));

        // less used: its face, its parts one by one, models on its bones, copies of it, presets
        if (!IsAdvanced(Page::Look))
            return;
        AdvancedRows rows;
        if (killstreak)
            RenderDamaged(actor, Components::KillstreakLogic::Get(*killstreak));
        if (person)
        {
            // its face, moving as the game's players' faces do (see ActorLogic::GetFace)
            Label("Face");
            bool face = actor.face == Types::ActorFace::Auto;
            if (ImGui::Checkbox("Auto##actorFace", &face))
                actor.face = face ? Types::ActorFace::Auto : Types::ActorFace::Off;
            ImGuiEx::Keyframeable::Record("actors.face");
            ImGui::SetItemTooltip(
                "Moves as the game's players' faces do: idling, alert while it aims or shoots, a grimace at each shot, "
                "wincing when hit, dying with a death. The Face row under its clips plays over it, also when off");
        }
        RenderParts(actor, catalog);
        RenderProps(actor, catalog);
        RenderGroup(actor);
        ImGui::SeparatorText("Presets");
        RenderPresets(Components::Actors::PresetKind::Look, actor);
        if (person)
            RenderCopyLook(actor);
    }

    // what loading the actor's character zones is doing: loading, or failing
    void ActorsMenu::RenderZoneState(const Types::Actor& actor, const Catalog& catalog)
    {
        if (catalog.HasModels(actor.models))
            return;
        const auto zones = Components::Actors::GetActorZones(actor, catalog);
        const bool loading = std::ranges::any_of(zones, [&](const std::string& zone) {
            return std::ranges::find(catalog.loadedZones, zone) == catalog.loadedZones.end();
        });
        // (the error is of the zone that failed last, named first)
        const bool failed = std::ranges::any_of(
            zones, [&](const std::string& zone) { return catalog.zoneError.starts_with(zone + ":"); });
        if (failed)
            ImGui::TextColored(Theme::Misfit(), ICON_FA_TRIANGLE_EXCLAMATION "  %s",
                               catalog.zoneError.c_str());
        else if (loading)
            ImGui::TextDisabled(ICON_FA_HOURGLASS_HALF "  Loading the character...");
        else
            ImGui::TextDisabled(ICON_FA_TRIANGLE_EXCLAMATION "  Some models can't load");
    }

    // Without the character zones, a line that says how to set them up, with the steps in a popup
    void ActorsMenu::RenderZoneSetup()
    {
        constexpr auto SETUP_POPUP = "##zoneSetup";
        if (ImGui::SmallButton(ICON_FA_USERS " Set up all characters..."))
            ImGui::OpenPopup(SETUP_POPUP);
        ImGuiEx::Keyframeable::Record("actors.zonesetup");
        ImGui::SetItemTooltip("Builds a small zone for each character of the campaign and Zombies, once");
        if (ImGui::BeginPopup(SETUP_POPUP))
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
            ImGui::TextUnformatted(
                "The characters of the campaign and Zombies load one by one, each in about a second, from a small "
                "zone of their own. Those zones are built from your game's files once, with OpenAssetTools' Linker:");
            ImGui::BulletText("run tools/t5-characters/setup.ps1 -Game <your Black Ops folder>");
            ImGui::BulletText("it downloads OpenAssetTools (3.8 MB, GPL-3) and takes a few minutes");
            ImGui::TextWrapped("It writes them to %s.", Components::Actors::GetCharacterZonesFolder().string().c_str());
            ImGui::TextUnformatted("Until then, a character of a level loads with all of that level's characters.");
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Check again"))
            {
                Components::Actors::RescanCharacterZones();
                ImGui::CloseCurrentPopup();
            }
            ImGuiEx::Keyframeable::Record("actors.zonesetup.check");
            ImGui::EndPopup();
        }
    }

    // How many bones a character has as the game makes it, as far as the catalog knows them (its own counts, else
    // those of its loaded models)
    std::uint32_t CountCharacterBones(const Catalog& catalog, const Types::Character& character)
    {
        std::uint32_t bones = 0;
        for (const auto& model : character.GetModels())
        {
            if (const auto known = character.bones.find(model); known != character.bones.end())
                bones += known->second;
            else if (const auto loaded = catalog.FindModel(model))
                bones += loaded->bones;
        }
        return bones;
    }

    // (A character of a level whose models aren't loaded loads with it, its level's pack without character zones. The
    // charred twins are its characters' Burnt look.)
    void CharacterPicker(const char* id, const Catalog& catalog, const Types::Character* current,
                         const std::string& preview, const char* keep, std::string& filter, const std::string& record,
                         const std::string& rowRecord, bool peopleOnly, const char* tooltip,
                         const std::function<void(const Types::Character*)>& choose)
    {
        const auto describe = [&](const Types::Character& character) {
            return Components::CharacterLogic::Describe(character, catalog.levels);
        };
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool open = ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLargest);
        if (!open)
        {
            ImGuiEx::Keyframeable::Record(record);
            ImGui::SetItemTooltip("%s", tooltip);
            return;
        }
        FilterField("##characterFilter", filter);
        ImGuiEx::Keyframeable::Record(rowRecord + ".search");
        if (keep)
        {
            if (ImGui::Selectable(keep, !current))
            {
                choose(nullptr);
                ImGui::CloseCurrentPopup();
            }
            ImGuiEx::Keyframeable::Record(rowRecord + ".keep");
        }

        const auto matches = [&](const Types::Character& character) {
            if (peopleOnly && character.kind != Types::Character::Kind::Human &&
                character.kind != Types::Character::Kind::Zombie)
                return false;
            return character.burntOf.empty() && (MatchesFilter(describe(character), filter) ||
                                                 MatchesFilter(character.id, filter));
        };
        const auto row = [&](const Types::Character& character, const char* section) {
            ImGui::PushID(section);
            ImGui::PushID(character.id.c_str());
            const bool favorite = Components::Actors::IsFavoriteCharacter(character.id);
            ImGui::PushStyleColor(ImGuiCol_Text, favorite ? Theme::Favorite()
                                                          : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::SmallButton(ICON_FA_STAR))
                Components::Actors::SetFavoriteCharacter(character.id, !favorite);
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip(favorite ? "Takes it out of the favourites" : "Makes it a favourite");
            ImGui::SameLine();
            const bool loaded = catalog.HasModels(character.GetModels());
            const bool zones = Components::Actors::HasCharacterZones();
            // with the character zones set up, a character without one can't load (the setup was left unfinished)
            const bool unavailable = zones && !loaded && !character.level.empty() &&
                                     !Components::Actors::IsCharacterZoneAvailable(character.zone);
            const auto bones = CountCharacterBones(catalog, character);
            const auto label = std::format("{}{}", character.name,
                                           loaded        ? ""
                                           : unavailable ? "  " ICON_FA_TRIANGLE_EXCLAMATION
                                                         : "  " ICON_FA_DOWNLOAD);
            // (its images aren't there yet, and the game ends on an image it can't load)
            const bool noTextures = unavailable && Components::Actors::IsCharacterZoneMissingTextures(character.zone);
            ImGui::BeginDisabled(noTextures);
            if (ImGui::Selectable(label.c_str(), current && current->id == character.id))
            {
                choose(&character);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGuiEx::Keyframeable::Record(rowRecord + "." + character.id);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
            {
                std::string models;
                for (const auto& model : character.GetModels())
                    models += "\n  " + model;
                const auto state = loaded        ? ""
                                   : noTextures  ? "\nNeeds its textures: run Iris Studio's Setup"
                                   : unavailable ? "\nIts zone wasn't built: run tools/t5-characters/setup.ps1 again"
                                   : zones       ? "\nNot loaded: it loads when it is chosen (a second or so)"
                                                 : "\nNot loaded: its level's characters load when it is chosen";
                ImGui::SetTooltip("%s%s%s\n%u bones%s", describe(character).c_str(),
                                  character.loose ? "\nA model the level's scripts use" : "", models.c_str(), bones,
                                  state);
            }
            ImGui::PopID();
            ImGui::PopID();
        };

        std::vector<const Types::Character*> favorites;
        for (const auto& character : catalog.characters)
            if (Components::Actors::IsFavoriteCharacter(character.id) && matches(character))
                favorites.push_back(&character);
        if (!favorites.empty())
        {
            ImGui::SeparatorText(ICON_FA_STAR " Favorites");
            for (const auto character : favorites)
                row(*character, "favorites");
        }

        bool any = !favorites.empty();
        std::string section;
        for (const auto& character : catalog.characters)
        {
            if (!matches(character))
                continue;
            const auto level = std::ranges::find(catalog.levels, character.level, &Catalog::Pack::name);
            const auto heading = character.kind == Types::Character::Kind::Killstreak ? std::string("Killstreaks")
                                 : character.level.empty()                                ? std::string("This map")
                                 : level != catalog.levels.end()
                                     ? std::format("{} ({})", level->displayName, level->group)
                                     : character.level;
            if (heading != section)
            {
                section = heading;
                ImGui::SeparatorText(section.c_str());
            }
            row(character, "all");
            any = true;
        }
        if (!any)
            ImGui::TextDisabled("Nothing found");
        ImGui::EndCombo();
    }

    void ActorsMenu::RenderCharacterPicker(Types::Actor& actor, const Catalog& catalog, const Types::Character* current)
    {
        const auto unloaded = Components::Actors::HasCharacterZones() ? " (loading)" : " (not loaded)";
        const auto preview = current ? Components::CharacterLogic::Describe(*current, catalog.levels) +
                                           (catalog.HasModels(actor.models) ? "" : unloaded)
                             : actor.models.empty()
                                 ? std::string("None")
                                 : Components::Actors::DescribeModel(actor.models.front()) + " (no character)";
        CharacterPicker("##actorCharacter", catalog, current, preview, nullptr, characterFilter, "actors.character",
                        "character", false, "Who the actor is: the map's characters, the campaign's and Zombies'",
                        [&](const Types::Character* character) {
                            Components::Actors::ApplyCharacter(actor, *character, {}, catalog);
                        });
    }

    std::optional<std::int32_t> FacePaintRows(std::int32_t current, const char* keep,
                                              std::optional<std::int32_t>& hovered, const std::string& record)
    {
        std::optional<std::int32_t> chosen;
        const auto row = [&](std::int32_t number, const char* name) {
            if (ImGui::Selectable(std::format("{}##facePaint{}", name, number).c_str(), current == number))
                chosen = number;
            if (ImGui::IsItemHovered())
                hovered = number;
            ImGuiEx::Keyframeable::Record(std::format("{}.{}", record, number));
        };
        if (keep)
            row(-1, keep);
        for (std::size_t number = 0; number < Components::PlayerLookLogic::FACE_PAINTS.size(); ++number)
            row(static_cast<std::int32_t>(number), Components::PlayerLookLogic::FACE_PAINTS[number].data());
        return chosen;
    }

    // Its face paint, shown on it as the mouse goes down the list (a paint is a number: nothing loads)
    void ActorsMenu::RenderFacePaint(Types::Actor& actor, const Catalog& catalog)
    {
        const auto kept = facePaintPreviewWas ? *facePaintPreviewWas : actor;
        const bool possible = catalog.CanHaveFacePaint(kept.models);
        Label("Face paint");
        ImGui::BeginDisabled(!possible);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto name = Components::PlayerLookLogic::GetFacePaintName(kept.facePaint);
        std::optional<std::int32_t> hovered, chosen;
        if (ImGui::BeginCombo("##actorFacePaint", possible ? std::string(name).c_str() : "No face to paint",
                              ImGuiComboFlags_HeightLarge))
        {
            chosen = FacePaintRows(kept.facePaint, nullptr, hovered, "actors.facepaint");
            ImGui::EndCombo();
        }
        else
        {
            ImGuiEx::Keyframeable::Record("actors.facepaint");
        }
        ImGui::EndDisabled();

        if (chosen)
        {
            // kept from as it was before any preview
            if (facePaintPreviewWas)
                actor = *facePaintPreviewWas;
            facePaintPreviewWas.reset();
            actor.facePaint = *chosen;
            Components::Actors::Update(actor, false, "face paint");
        }
        else if (hovered && *hovered != kept.facePaint)
        {
            if (!facePaintPreviewWas)
                facePaintPreviewWas = actor;
            auto shown = *facePaintPreviewWas;
            shown.facePaint = *hovered;
            Components::Actors::Preview(shown);
        }
        else if (facePaintPreviewWas)
        {
            Components::Actors::Preview(*facePaintPreviewWas);
            actor = *facePaintPreviewWas;
            facePaintPreviewWas.reset();
        }
    }

    // what a wounded upper or lower body has shot off, from its name: c_vtn_nva1_body_g_rarmoff -> "Right arm off"
    std::string DescribeWound(std::string_view model)
    {
        static const std::array<std::pair<std::string_view, std::string_view>, 9> WOUNDS{{
            {"upclean", "Whole"},
            {"lowclean", "Whole"},
            {"rarmoff", "Right arm off"},
            {"larmoff", "Left arm off"},
            {"rlegoff", "Right leg off"},
            {"llegoff", "Left leg off"},
            {"legsoff", "Legs off"},
            {"behead", "Head off"},
            {"torso", "Arms off"},
        }};
        for (const auto& [suffix, text] : WOUNDS)
            if (model.ends_with(suffix))
                return std::string(text);
        return Components::Actors::DescribeModel(model);
    }

    // How the character looks: as in the game, burnt (its charred twin) or wounded (an upper and a lower body with
    // parts shot off), for the characters that have those
    void ActorsMenu::RenderCharacterLook(Types::Actor& actor, const Catalog& catalog, const Types::Character& character)
    {
        using Components::CharacterLogic::Variant;
        // a killstreak's: the friendly team's colours or the enemy's (a care package booby-trapped)
        if (character.kind == Types::Character::Kind::Killstreak)
        {
            if (character.bodies.size() < 2)
                return;
            const auto look = Components::CharacterLogic::GetLook(character, actor.models, nullptr);
            auto chosen = look;
            Label("Look");
            for (std::size_t i = 0; i < character.bodies.size(); ++i)
            {
                const std::string name(Components::KillstreakLogic::GetLookName(i));
                if (ImGui::RadioButton(name.c_str(), look.body == i))
                    chosen.body = i;
                std::string record = name;
                std::ranges::transform(record, record.begin(), [](char c) {
                    return c == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                });
                ImGuiEx::Keyframeable::Record("actors.look." + record);
                ImGui::SameLine();
            }
            ImGui::NewLine();
            if (chosen.body != look.body)
                Components::Actors::ApplyCharacter(actor, character, chosen, catalog);
            return;
        }
        const auto burnt = catalog.FindCharacter(character.burnt);
        const bool woundable = !character.woundedTorsos.empty();
        if (!burnt && !woundable)
            return;
        auto look = Components::CharacterLogic::GetLook(character, actor.models, burnt);
        auto chosen = look;

        Label("Look");
        const auto choice = [&](const char* text, Variant variant, const char* record, bool available) {
            ImGui::BeginDisabled(!available);
            if (ImGui::RadioButton(text, look.variant == variant))
                chosen.variant = variant;
            ImGui::EndDisabled();
            ImGuiEx::Keyframeable::Record(record);
            ImGui::SameLine();
        };
        choice("Default", Variant::Normal, "actors.look.normal", true);
        choice("Burnt", Variant::Burnt, "actors.look.burnt", burnt != nullptr);
        choice("Wounded", Variant::Wounded, "actors.look.wounded", woundable);
        ImGui::NewLine();

        if (look.variant == Variant::Wounded)
        {
            const auto woundCombo = [&](const char* id, const std::vector<std::string>& bodies, std::size_t& index) {
                if (bodies.empty())
                    return;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(id, DescribeWound(bodies[std::min(index, bodies.size() - 1)]).c_str()))
                {
                    for (std::size_t i = 0; i < bodies.size(); ++i)
                    {
                        if (ImGui::Selectable(std::format("{}##{}", DescribeWound(bodies[i]), i).c_str(), i == index))
                            index = i;
                        ImGui::SetItemTooltip("%s", bodies[i].c_str());
                    }
                    ImGui::EndCombo();
                }
            };
            Label("Upper body");
            woundCombo("##woundTorso", character.woundedTorsos, chosen.torso);
            Label("Lower body");
            woundCombo("##woundLegs", character.woundedLegs, chosen.legs);
        }

        if (chosen.variant != look.variant || chosen.torso != look.torso || chosen.legs != look.legs)
            Components::Actors::ApplyCharacter(actor, character, chosen, catalog);
    }

    // The actor's models one by one (under Advanced): its body, head and what it wears, any of the loaded models of
    // each kind. A full body has a head of its own, so the head chosen before goes (with it, the face's bones would
    // count twice, and the game can draw only so many).
    void ActorsMenu::RenderParts(Types::Actor& actor, const Catalog& catalog)
    {
        ImGui::SeparatorText("Parts");
        auto parts = GetParts(catalog, actor);
        bool changed = false;
        Label("Body");
        if (ModelCombo(
                "##actorBody", catalog, parts.body, {ModelKind::FullBody, ModelKind::Body}, nullptr, modelFilter,
                [&](const Catalog::Model& body) {
                    return CountBones(catalog, parts.worn) +
                           CountBones(catalog, {body.kind == ModelKind::FullBody ? "" : parts.head}) + body.bones;
                },
                [&] {
                    if (!Components::Actors::HasCharacterZones())
                        RenderPackMenu(catalog);
                }))
        {
            changed = true;
            if (GetKind(catalog, parts.body) == ModelKind::FullBody)
                parts.head.clear();
        }
        const bool fullBody =
            catalog.FindModel(parts.body) && catalog.FindModel(parts.body)->kind == ModelKind::FullBody;
        // (a dog's head and what it might wear are its body's, and a killstreak's)
        if (Components::Actors::GetActorKind(actor) != Types::ActorKind::Human)
        {
            if (changed && !parts.body.empty())
                actor.models = parts.ToModels();
            return;
        }
        Label("Head");
        changed |=
            ModelCombo("##actorHead", catalog, parts.head, {ModelKind::Head},
                       fullBody ? "None (the body has one)" : "None", modelFilter, [&](const Catalog::Model& head) {
                           return CountBones(catalog, {parts.body}) + CountBones(catalog, parts.worn) + head.bones;
                       });
        // a combo for each thing worn, and one more to add another (up to three)
        constexpr std::size_t MAX_WORN = 3;
        if (parts.worn.size() < MAX_WORN)
            parts.worn.emplace_back();
        for (std::size_t i = 0; i < parts.worn.size(); ++i)
        {
            Label("Gear");
            // (the first keeps the id the harness clicks it by)
            const auto id = i == 0 ? std::string("##actorGear") : std::format("##actorGear{}", i + 1);
            changed |= ModelCombo(id.c_str(), catalog, parts.worn[i], {ModelKind::Gear}, "Nothing", modelFilter,
                                  [&](const Catalog::Model& gear) {
                                      auto others = parts.worn;
                                      others.erase(others.begin() + static_cast<std::ptrdiff_t>(i));
                                      return CountBones(catalog, {parts.body, parts.head}) +
                                             CountBones(catalog, others) + gear.bones;
                                  });
        }
        if (changed && !parts.body.empty())
        {
            // (another person's body goes on as this one did, see ActorLogic::KeepMovementSet)
            const auto was = Components::Actors::GetMovementSet(actor);
            actor.models = parts.ToModels();
            ActorLogic::KeepMovementSet(actor, was, Components::Actors::GetMovementSet(actor));
        }
    }

    // Presets of a kind: a combo that applies one (right-click deletes it), and a button that saves the actor's
    void ActorsMenu::RenderPresets(Components::Actors::PresetKind kind, Types::Actor& actor)
    {
        const bool look = kind == Components::Actors::PresetKind::Look;
        const auto names = Components::Actors::GetPresetNames(kind);
        const auto& style = ImGui::GetStyle();
        const auto saveWidth = ImGui::CalcTextSize(ICON_FA_FLOPPY_DISK " Save").x + style.FramePadding.x * 2;
        ImGui::PushID(look ? "lookPresets" : "clipPresets");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - saveWidth - style.ItemSpacing.x);
        if (ImGui::BeginCombo("##presets", ICON_FA_BOOKMARK " Presets", ImGuiComboFlags_HeightLarge))
        {
            if (names.empty())
                ImGui::TextDisabled("No presets yet: save one with the button next to this");
            for (const auto& name : names)
            {
                // clips of animations that don't fit the actor, marked as the browser marks them
                std::string misfits;
                if (!look)
                    for (const auto& animation : Components::Actors::GetPresetAnimations(name))
                        if (const auto misfit = Components::Actors::DescribeMisfit(actor.models, animation);
                            !misfit.empty())
                            misfits += std::format("\n{}: {}", animation, misfit);
                if (!misfits.empty())
                    ImGui::PushStyleColor(ImGuiCol_Text, MisfitColor());
                if (ImGui::Selectable(std::format("{}{}", misfits.empty() ? "" : ICON_FA_BAN " ", name).c_str()))
                    Components::Actors::ApplyPreset(kind, name, actor, Components::Playback::GetActorTick());
                if (!misfits.empty())
                    ImGui::PopStyleColor();
                ImGui::SetItemTooltip(
                    "%s", misfits.empty() ? "Right-click to delete it"
                                          : ("Right-click to delete it. Doesn't fit the actor:" + misfits).c_str());
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                    Components::Actors::DeletePreset(kind, name);
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip(look ? "Gives the actor a character, weapon and props saved before"
                                   : "Adds clips saved before, from the playhead on");

        ImGui::SameLine();
        constexpr auto SAVE_POPUP = "##savePreset";
        ImGui::BeginDisabled(!look && actor.clips.empty());
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save", ImVec2(saveWidth, 0)))
        {
            presetName = actor.name;
            ImGui::OpenPopup(SAVE_POPUP);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(look ? "Saves the actor's character, weapon and props as a preset"
                                   : "Saves the actor's clips as a preset");
        if (ImGui::BeginPopup(SAVE_POPUP))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            char buffer[64];
            strncpy_s(buffer, presetName.c_str(), _TRUNCATE);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            const bool entered = ImGui::InputTextWithHint("##presetName", "Name", buffer, sizeof(buffer),
                                                          ImGuiInputTextFlags_EnterReturnsTrue);
            presetName = buffer;
            const bool exists = std::ranges::find(names, presetName) != names.end();
            ImGui::BeginDisabled(presetName.empty());
            if (ImGui::Button(exists ? "Replace" : "Save") || (entered && !presetName.empty()))
            {
                Components::Actors::SavePreset(kind, presetName, actor);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (exists)
                ImGui::SetItemTooltip("A preset of that name is replaced");
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Makes a player of the demo look like the actor, through the player loadouts
    void ActorsMenu::RenderCopyLook(const Types::Actor& actor)
    {
        constexpr auto COPY_POPUP = "##copyLook";
        if (ImGui::Button(ICON_FA_USER_PEN " Copy to player...", ImVec2(-FLT_MIN, 0)))
        {
            copyLookMessage.clear();
            ImGui::OpenPopup(COPY_POPUP);
        }
        ImGui::SetItemTooltip(
            "Gives a player of the demo its body, head and weapon, as the Players page's loadouts do");
        if (ImGui::BeginPopup(COPY_POPUP))
        {
            const auto players = GetPlayers();
            if (players.empty())
                ImGui::TextDisabled("Finding players...");
            for (const auto& [client, name] : players)
            {
                if (ImGui::Selectable(std::format("{}##{}", name, client).c_str()))
                {
                    const auto error = Components::Actors::CopyLookToPlayer(actor, client);
                    copyLookMessage = error.empty() ? std::format("{} looks like {} now", name, actor.name) : error;
                }
            }
            ImGui::EndPopup();
        }
        if (!copyLookMessage.empty())
            ImGui::TextDisabled("%s", copyLookMessage.c_str());
    }

    // Copies of the actor in rows next to and behind it, each starting a little later if wanted
    void ActorsMenu::RenderGroup(const Types::Actor& actor)
    {
        ImGui::SeparatorText("Group");
        Label("Actors");
        auto count = static_cast<int>(formation.count);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragInt("##groupCount", &count, 0.1f, 2, 32, "%d in all", ImGuiSliderFlags_AlwaysClamp))
            formation.count = static_cast<std::uint32_t>(count);
        Label("Per row");
        auto columns = static_cast<int>(formation.columns);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragInt("##groupColumns", &columns, 0.1f, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp))
            formation.columns = static_cast<std::uint32_t>(columns);
        Label("Spacing");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat2("##groupSpacing", &formation.spacing.x, 0.5f, 8.0f, 500.0f, "%.0f",
                          ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetItemTooltip("Between the actors of a row, and between rows; the rows go to its right and behind it");
        Label("Stagger");
        float stagger = formation.stagger / 1000.0f;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragFloat("##groupStagger", &stagger, 0.01f, 0.0f, 5.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
            formation.stagger = static_cast<std::uint32_t>(stagger * 1000.0f);
        ImGui::SetItemTooltip("How much later each copy's clips, events and keys are than the one before's");
        if (ImGui::Button(ICON_FA_PEOPLE_GROUP " Create group", ImVec2(-FLT_MIN, 0)))
            Components::Actors::AddFormation(actor.id, formation, Components::Playback::GetActorTick());
        ImGui::SetItemTooltip("Adds the copies as actors of their own; one undo takes them away");
    }

    // the bones models are put on most, by a name to show
    constexpr std::array<std::pair<const char*, const char*>, 12> PROP_TAGS{{
        {"tag_weapon_left", "Left hand"},
        {"tag_weapon_right", "Right hand"},
        {"tag_inhand", "Left hand (held)"},
        {"j_head", "Head"},
        {"tag_eye", "Eyes"},
        {"j_spine4", "Chest"},
        {"tag_stowed_back", "Back"},
        {"j_spinelower", "Belly"},
        {"j_mainroot", "Hips"},
        {"j_knee_le", "Left knee"},
        {"j_knee_ri", "Right knee"},
        {"tag_origin", "Feet (origin)"},
    }};

    // A combo of bones by the names above, with the bone's own name for one not among them
    bool TagCombo(const char* id, std::string& tag)
    {
        const auto known = std::ranges::find(PROP_TAGS, tag, [](const auto& entry) { return std::string_view(entry.first); });
        bool changed = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(id, known != PROP_TAGS.end() ? known->second : tag.c_str(), ImGuiComboFlags_HeightLarge))
        {
            for (const auto& [name, description] : PROP_TAGS)
            {
                if (ImGui::Selectable(std::format("{}##{}", description, name).c_str(), tag == name))
                {
                    tag = name;
                    changed = true;
                }
                ImGui::SetItemTooltip("%s", name);
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    // The models on the actor's bones: any model the game has loaded, moved and turned from the bone
    void ActorsMenu::RenderProps(Types::Actor& actor, const Catalog& catalog)
    {
        ImGui::SeparatorText("Props");
        if (ImGui::Button(ICON_FA_PLUS " Prop", ImVec2(-FLT_MIN, 0)))
            actor.props.push_back({});
        ImGuiEx::Keyframeable::Record("actors.look.addprop");
        ImGui::SetItemTooltip("Puts a model of the map, e.g. a radio or a bag, on a bone of the actor");

        for (std::size_t i = 0; i < actor.props.size(); ++i)
        {
            auto& prop = actor.props[i];
            ImGui::PushID(static_cast<int>(i));
            Label("Model");
            const auto removeWidth = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - removeWidth - ImGui::GetStyle().ItemSpacing.x);
            const auto missing = !prop.model.empty() && !std::ranges::binary_search(catalog.props, prop.model);
            if (ImGui::BeginCombo("##propModel",
                                  prop.model.empty() ? "Choose a model"
                                  : missing          ? (prop.model + " (not loaded)").c_str()
                                                     : prop.model.c_str(),
                                  ImGuiComboFlags_HeightLarge))
            {
                FilterField("##propFilter", propFilter);
                constexpr std::size_t MAX_LISTED = 400;
                std::size_t listed = 0, found = 0;
                for (const auto& model : catalog.props)
                {
                    if (!MatchesFilter(model, propFilter))
                        continue;
                    ++found;
                    if (listed++ < MAX_LISTED && ImGui::Selectable(model.c_str(), prop.model == model))
                        prop.model = model;
                }
                if (found > MAX_LISTED)
                    ImGui::TextDisabled("%zu more; search to find them", found - MAX_LISTED);
                if (found == 0)
                    ImGui::TextDisabled("Nothing found");
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH "##propRemove", ImVec2(removeWidth, 0)))
            {
                actor.props.erase(actor.props.begin() + static_cast<std::ptrdiff_t>(i));
                ImGui::PopID();
                break;
            }
            ImGui::SetItemTooltip("Takes the prop off");
            Label("On");
            TagCombo("##propTag", prop.tag);
            Label("Offset");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat3("##propOffset", &prop.offset.x, 0.1f, 0.0f, 0.0f, "%.1f");
            ImGui::SetItemTooltip("From the bone, along its axes");
            Label("Rotation");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat3("##propAngles", &prop.angles.x, 0.5f, 0.0f, 0.0f, "%.0f deg");
            ImGui::SetItemTooltip("Pitch, yaw and roll from the bone");
            ImGui::PopID();
            if (i + 1 < actor.props.size())
                ImGui::Separator();
        }
    }

    void ActorsMenu::RenderClips(Types::Actor& actor, const Catalog& catalog)
    {
        const auto tick = Components::Playback::GetActorTick();
        const auto selectedClip = Components::Actors::GetSelectedClip();
        auto clip = std::ranges::find(actor.clips, selectedClip.value_or(0), &Types::ActorClip::id);
        // or one of the Face row's
        const auto selectedFaceClip = Components::Actors::GetSelectedFaceClip();
        const auto faceClip = std::ranges::find(actor.faceClips, selectedFaceClip.value_or(0), &Types::ActorClip::id);
        const bool advanced = IsAdvanced(Page::Animation);

        // + Then: after the selected (or last) clip, its idle, a transition, another look of the walk, a recent
        // animation or one browsed; on the Face row, + Clip as before. Browse:
        // every animation that fits it
        constexpr auto BROWSE_POPUP = "##actorAnimationBrowse";
        constexpr auto ADD_THEN_POPUP = "##actorAddThen";
        const bool onFace = faceClip != actor.faceClips.end();
        const auto clicked = ButtonRow(
            "##clipButtons",
            {{ICON_FA_PLUS, onFace ? "Clip" : "Then " ICON_FA_CARET_DOWN,
              onFace ? "Adds one play of the selected or last clip's animation at the playhead (for a death, the idle)"
                     : "Adds after the selected clip (or the last): its idle, a transition, another look of the walk, "
                       "a recent animation or one browsed",
              false, false, "actors.clip.add"},
             {ICON_FA_FOLDER_OPEN, "Browse...",
              "Every animation that fits it: double-click one to play it from the playhead, or drag it onto "
              "its lane on the timeline",
              false, false, "actors.browse"}});
        if (clicked == 0 && faceClip != actor.faceClips.end())
        {
            // on the Face row: the selected one's animation, to the next or for three seconds
            Types::ActorClip added{.animation = faceClip->animation,
                                   .start = tick,
                                   .length = 3000,
                                   .loop = faceClip->loop,
                                   .blendIn = faceClip->blendIn};
            for (const auto& other : actor.faceClips)
                if (other.start > tick)
                    added.length = std::min(added.length, other.start - tick);
            Components::Actors::Update(actor);
            Components::Actors::SelectFaceClip(Components::Actors::AddFaceClip(actor.id, added));
            actor = *Components::Actors::GetActor(actor.id);
            return;
        }
        if (clicked == 0 && !onFace)
            ImGui::OpenPopup(ADD_THEN_POPUP);
        if (ImGui::BeginPopup(ADD_THEN_POPUP))
        {
            // (after a death it lies there: what is added goes after the clip before it, up to the death, which stays
            // where it is)
            auto after = clip != actor.clips.end() ? std::optional(clip->id)
                         : actor.clips.empty()     ? std::nullopt
                                                   : std::optional(actor.clips.back().id);
            std::optional<std::pair<std::uint32_t, std::uint32_t>> death;
            if (after)
                if (const auto at = std::ranges::find(actor.clips, *after, &Types::ActorClip::id);
                    at != actor.clips.begin() && !at->loop &&
                    ActorLogic::IsDeath(at->animation, Components::Actors::GetAnimationLookup()(at->animation)))
                {
                    death = std::pair(at->id, at->start);
                    after = std::prev(at)->id;
                }
            const auto* afterClip = after ? &*std::ranges::find(actor.clips, *after, &Types::ActorClip::id) : nullptr;
            ImGui::TextDisabled(
                "%s", afterClip
                          ? std::format("After {}", ShownName(Components::Actors::GetShownAnimation(actor, *afterClip)))
                                .c_str()
                          : "At the playhead");
            ImGui::Separator();
            std::optional<std::string> play;
            const auto idle = Components::Actors::GetIdleAnimation(actor);
            if (ImGui::MenuItem(std::format(ICON_FA_PERSON " Idle  ({})", idle).c_str()))
                play = idle;
            ImGuiEx::Keyframeable::Record("actors.then.idle");
            // a walk of a path: its look, not a clip that would fight the path
            const auto afterLocomotion =
                afterClip ? ActorLogic::FindLocomotion(actor, afterClip->animation) : std::nullopt;
            if (afterClip && actor.movement == Types::ActorMovement::Path && !actor.stretches.empty() &&
                !afterClip->picked && afterLocomotion && afterLocomotion->gait != ActorLogic::Gait::Idle)
            {
                if (ImGui::BeginMenu(ICON_FA_PERSON_RUNNING " Another look"))
                {
                    const auto& first = actor.stretches.front();
                    for (const auto& name : Components::Actors::GetLookCandidates(actor, first.gait, first.crouched, 8))
                        if (ImGui::MenuItem(ShownName(name).c_str()))
                        {
                            Components::Actors::SetLook(actor, std::nullopt, name);
                            Components::Actors::AddTriedLook(actor.id, name);
                        }
                    ImGui::EndMenu();
                }
                ImGuiEx::Keyframeable::Record("actors.then.look");
            }
            if (after)
            {
                if (ImGui::BeginMenu(ICON_FA_LINK " Then"))
                {
                    if (DrawThenItems(actor, *after, "actors.then.then"))
                    {
                        if (death)
                            ActorActions::KeepDeathAt(actor.id, death->first, death->second);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndMenu();
                }
                ImGuiEx::Keyframeable::Record("actors.then.then");
            }
            const auto recent = Components::Actors::GetRecentAnimations();
            if (!recent.empty())
                ImGui::SeparatorText("Recent");
            for (std::size_t i = 0; i < std::min<std::size_t>(recent.size(), 4); ++i)
            {
                if (ImGui::MenuItem(std::format("{}##recentThen{}", ShownName(recent[i]), i).c_str()))
                    play = recent[i];
                ImGuiEx::Keyframeable::Record("actors.then.recent." + recent[i]);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Browse..."))
            {
                browserLikeThis = afterClip ? Components::Actors::GetShownAnimation(actor, *afterClip) : std::string();
                browsed.clear();
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                ImGui::OpenPopup(BROWSE_POPUP);
            }
            else
            {
                ImGuiEx::Keyframeable::Record("actors.then.browse");
                ImGui::EndPopup();
            }
            if (play)
            {
                Components::Actors::Update(actor);
                if (after)
                    ActorActions::PlayAfter(actor.id, *after, *play);
                else
                    ActorActions::Play(actor.id, *play);
                if (death)
                    ActorActions::KeepDeathAt(actor.id, death->first, death->second);
                actor = *Components::Actors::GetActor(actor.id);
                return;
            }
        }
        if (clicked == 0 && onFace)
        {
            // (on the Face row, as before) one play of it, as picked, blending in as long as joins it cleanly
            const auto animation = ActorLogic::GetAddedClipAnimation(
                actor, clip != actor.clips.end() ? std::optional(clip->id) : std::nullopt,
                Components::Actors::GetIdleAnimation(actor), Components::Actors::GetAnimationLookup());
            const auto info = catalog.FindAnimation(animation);
            Types::ActorClip added{
                .animation = animation,
                .start = tick,
                .length =
                    info ? std::max(1u, static_cast<std::uint32_t>(std::lround(info->duration * 1000.0f))) : 3000u,
                .loop = info ? info->loop : true,
                .picked = true};
            for (const auto& other : actor.clips)
                if (other.start > tick)
                    added.length = std::min(added.length, other.start - tick);
            Components::Actors::Update(actor);
            const auto addedId = Components::Actors::AddClip(actor.id, added);
            Components::Actors::BlendCleanly(actor.id, addedId);
            Components::Actors::SelectClip(addedId);
            actor = *Components::Actors::GetActor(actor.id);
            return;
        }
        if (clicked == 1)
        {
            browsed.clear();
            ImGui::OpenPopup(BROWSE_POPUP);
        }
        if (AnimationBrowser(BROWSE_POPUP, catalog, actor.models, browsed, animationFilter, animationCategory))
        {
            Components::Actors::Update(actor);
            ActorActions::Play(actor.id, browsed);
            actor = *Components::Actors::GetActor(actor.id);
            return;
        }

        if (actor.clips.empty() && actor.faceClips.empty())
        {
            ImGui::TextDisabled("No clips.");
            if (advanced)
            {
                AdvancedRows rows;
                RenderPresets(Components::Actors::PresetKind::Clips, actor);
            }
            return;
        }

        // the clips, the transitions Auto transitions put in under the clip they lead into; a walk of the path as its
        // pieces, each named by its look
        std::optional<std::pair<std::uint32_t, bool>> edit;  // a clip to delete (or duplicate) from its menu
        const auto pieces = Components::Actors::GetWalkPieces(actor);
        const auto point = Components::Actors::GetSelectedWaypoint();
        std::size_t listed = actor.faceClips.size() + pieces.size();
        for (const auto& entry : actor.clips)
            if (std::ranges::none_of(pieces,
                                     [&](const ActorLogic::WalkPiece& piece) { return piece.clip == entry.id; }))
                ++listed;
        const auto rows = std::min<std::size_t>(listed, 6);
        if (ImGui::BeginChild("##actorClips", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * rows + 8), true))
        {
            for (const auto& entry : actor.clips)
            {
                if (std::ranges::any_of(pieces,
                                        [&](const ActorLogic::WalkPiece& piece) { return piece.clip == entry.id; }))
                {
                    const auto timing = Components::Actors::GetPathTiming(actor);
                    const auto* shown = FindShownPiece(pieces, entry.id, point);
                    bool firstOfClip = true;
                    for (std::size_t p = 0; p < pieces.size(); ++p)
                    {
                        const auto& piece = pieces[p];
                        if (piece.clip != entry.id)
                            continue;
                        const bool playing = piece.start <= tick && tick < piece.end;
                        const bool chained = entry.chain != Types::ActorChain::None;
                        const auto time = FormatActorTime(piece.start);
                        const auto timeWidth = ImGui::CalcTextSize(time.c_str()).x;
                        const auto start = ImGui::GetCursorPos();
                        const auto text =
                            std::format("{}{}" ICON_FA_ROUTE " {}##piece{}", chained ? "      " ICON_FA_LINK " " : "",
                                        playing ? ICON_FA_PLAY " " : "", ShownName(piece.animation), p);
                        if (ImGui::Selectable(text.c_str(), selectedClip == entry.id && shown == &piece,
                                              ImGuiSelectableFlags_AllowDoubleClick))
                        {
                            Components::Actors::SelectClip(entry.id);
                            Components::Actors::SelectWaypoint(piece.first);
                            seenWaypoint = piece.first;
                            page = Page::Animation;
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                JumpTo(piece.joined && timing.reached[piece.first] ? *timing.reached[piece.first]
                                                                                   : piece.start);
                        }
                        ImGuiEx::Keyframeable::Record(std::format("actors.piece.{}", p));
                        if (firstOfClip)
                            ImGuiEx::Keyframeable::Record(std::format("actors.clip.{}", entry.id));
                        const auto* stretch = ActorLogic::GetStretch(actor, piece.first);
                        ImGui::SetItemTooltip(
                            "%s", std::format(
                                      "{} "
                                      "\xc2\xb7"
                                      " {}{}\n{} " ICON_FA_ARROW_RIGHT " point {} "
                                      "\xc2\xb7"
                                      " from {}{}",
                                      piece.animation,
                                      stretch ? ActorActions::GetGaitName(actor, stretch->gait, stretch->crouched)
                                              : std::string("Walk"),
                                      stretch && !stretch->look.empty() ? ", its own look" : "",
                                      piece.first == 0 ? std::string("Start") : "Point " + std::to_string(piece.first),
                                      piece.last + 1, time, chained ? "\nAfter a turn or a wait" : "")
                                      .c_str());
                        if (firstOfClip && ImGui::BeginPopupContextItem(std::format("##clipMenu{}", entry.id).c_str()))
                        {
                            Components::Actors::SelectClip(entry.id);
                            if (ImGui::MenuItem(ICON_FA_FORWARD_STEP " Go to"))
                                JumpTo(entry.start);
                            if (ImGui::MenuItem(ICON_FA_TRASH " Delete", Commands::GetShortcut("edit.delete").c_str()))
                                edit = std::pair(entry.id, true);
                            ImGui::EndPopup();
                        }
                        firstOfClip = false;
                        const auto end = ImGui::GetCursorPos();
                        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - timeWidth, start.y));
                        ImGui::TextDisabled("%s", time.c_str());
                        ImGui::SetCursorPos(end);
                    }
                    continue;
                }
                const bool playing = entry.start <= tick && tick < entry.End();
                const auto missing = !catalog.FindAnimation(entry.animation);
                // (one that doesn't fit its models, chosen before its character changed)
                const auto misfit =
                    missing ? std::string() : Components::Actors::DescribeMisfit(actor.models, entry.animation);
                const bool chained = entry.chain != Types::ActorChain::None;
                const auto time = FormatActorTime(entry.start);
                const auto timeWidth = ImGui::CalcTextSize(time.c_str()).x;
                const auto start = ImGui::GetCursorPos();
                const auto shownName = Components::KillstreakLogic::IsMotion(entry.animation)
                                           ? std::string(Components::KillstreakLogic::GetMotionLabel(entry.animation))
                                           : ShownName(entry.animation);
                const auto text = std::format("{}{}{}{}##clip{}", chained ? "      " ICON_FA_LINK " " : "",
                                              playing ? ICON_FA_PLAY " " : "",
                                              missing           ? ICON_FA_TRIANGLE_EXCLAMATION " "
                                              : !misfit.empty() ? ICON_FA_BAN " "
                                                                : "",
                                              shownName, entry.id);
                ImGui::PushStyleColor(ImGuiCol_Text, !misfit.empty() ? MisfitColor()
                                                     : chained       ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                                                     : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                if (ImGui::Selectable(text.c_str(), selectedClip == entry.id, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    Components::Actors::SelectClip(entry.id);
                    page = Page::Animation;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        JumpTo(entry.start);
                }
                ImGui::PopStyleColor();
                ImGuiEx::Keyframeable::Record(std::format("actors.clip.{}", entry.id));
                ImGui::SetItemTooltip("%s\n%.2f s from %s%s%s%s", entry.animation.c_str(), entry.length / 1000.0f,
                                      time.c_str(), missing ? "; its animation isn't loaded" : "",
                                      misfit.empty() ? "" : ("\nDoesn't fit the actor: " + misfit).c_str(),
                                      chained ? "\nAuto transition" : "");
                if (ImGui::BeginPopupContextItem(std::format("##clipMenu{}", entry.id).c_str()))
                {
                    Components::Actors::SelectClip(entry.id);
                    if (ImGui::MenuItem(ICON_FA_FORWARD_STEP " Go to"))
                        JumpTo(entry.start);
                    if (ImGui::MenuItem(ICON_FA_CLONE " Duplicate", Commands::GetShortcut("edit.duplicate").c_str()))
                        edit = std::pair(entry.id, false);
                    if (ImGui::MenuItem(ICON_FA_TRASH " Delete", Commands::GetShortcut("edit.delete").c_str()))
                        edit = std::pair(entry.id, true);
                    if (!Components::KillstreakLogic::IsMotion(entry.animation) &&
                        ImGui::BeginMenu(ICON_FA_FORWARD " Then"))
                    {
                        Components::Actors::Update(actor);
                        DrawThenItems(*Components::Actors::GetActor(actor.id), entry.id, "actors.clipmenu.then");
                        ImGui::EndMenu();
                    }
                    ImGui::EndPopup();
                }
                // when it starts, at the right
                const auto end = ImGui::GetCursorPos();
                ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - timeWidth, start.y));
                ImGui::TextDisabled("%s", time.c_str());
                ImGui::SetCursorPos(end);
            }
            // the Face row's, after them
            for (const auto& entry : actor.faceClips)
            {
                const bool playing = entry.start <= tick && tick < entry.End();
                const auto time = FormatActorTime(entry.start);
                const auto timeWidth = ImGui::CalcTextSize(time.c_str()).x;
                const auto start = ImGui::GetCursorPos();
                if (ImGui::Selectable(std::format("{}" ICON_FA_FACE_SMILE " {}##faceClip{}",
                                                  playing ? ICON_FA_PLAY " " : "", entry.animation, entry.id)
                                          .c_str(),
                                      selectedFaceClip == entry.id, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    Components::Actors::SelectFaceClip(entry.id);
                    page = Page::Animation;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        JumpTo(entry.start);
                }
                ImGuiEx::Keyframeable::Record(std::format("actors.faceclip.{}", entry.id));
                ImGui::SetItemTooltip("%s on the Face row\n%.2f s from %s", entry.animation.c_str(),
                                      entry.length / 1000.0f, time.c_str());
                const auto end = ImGui::GetCursorPos();
                ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - timeWidth, start.y));
                ImGui::TextDisabled("%s", time.c_str());
                ImGui::SetCursorPos(end);
            }
        }
        ImGui::EndChild();
        if (edit)
        {
            page = Page::Animation;
            Components::Actors::SelectClip(edit->first);
            DeleteOrDuplicate(actor, edit->second);
            return;
        }

        if (faceClip != actor.faceClips.end())
        {
            RenderFaceClip(actor, *faceClip, catalog);
            return;
        }
        if (clip == actor.clips.end())
        {
            if (advanced)
            {
                AdvancedRows advancedRows;
                RenderPresets(Components::Actors::PresetKind::Clips, actor);
            }
            return;
        }

        // a walk of the path: the piece shown, and with Advanced what the legs play,
        // the transitions and presets
        if (const auto* piece = FindShownPiece(pieces, clip->id, point))
        {
            RenderWalkPiece(actor, clip->id, *piece);
            if (advanced)
            {
                AdvancedRows advancedRows;
                RenderLegs(actor);
                Label("Transitions");
                bool automatic = actor.autoTransitions;
                if (ImGui::Checkbox("Auto##actorAutoTransitions", &automatic))
                    Components::Actors::SetAutoTransitions(actor, automatic);
                ImGuiEx::Keyframeable::Record("actors.autotransitions");
                ImGui::SetItemTooltip(
                    "Joins its clips and its path with the game's transitions (starts, stops, turns, changes of "
                    "stance), put in again as they change. Off takes them out");
                ImGui::SeparatorText("Presets");
                RenderPresets(Components::Actors::PresetKind::Clips, actor);
            }
            return;
        }

        // (what the editor changes of a transition Auto transitions put in makes it the user's)
        const auto unedited = *clip;
        std::optional<std::uint32_t> rejoin;

        constexpr auto BROWSER_POPUP = "##actorAnimationBrowser";
        Label("Animation");
        const auto animation = catalog.FindAnimation(clip->animation);
        const auto misfit =
            animation ? Components::Actors::DescribeMisfit(actor.models, clip->animation) : std::string();
        // (a walk of a path with gaits plays its stretches' looks)
        const auto shown = Components::Actors::GetShownAnimation(actor, *clip);
        const bool pathWalk = shown != clip->animation ||
                              (actor.movement == Types::ActorMovement::Path && !actor.stretches.empty() &&
                               actor.locomotion == Types::ActorLocomotion::Auto && !clip->picked &&
                               (clip->chain == Types::ActorChain::None || clip->chain == Types::ActorChain::Walk) &&
                               ActorLogic::FindLocomotion(actor, clip->animation));
        if (ImGui::Button(
                std::format("{}{}{}{}##clipAnimation", misfit.empty() ? "" : ICON_FA_BAN " ", ShownName(shown),
                            animation ? "" : " (not loaded)", pathWalk ? "  (the path's look)" : "")
                    .c_str(),
                ImVec2(-FLT_MIN, 0)))
        {
            browserLikeThis = shown;
            ImGui::OpenPopup(BROWSER_POPUP);
        }
        ImGuiEx::Keyframeable::Record("actors.clip.animation");
        if (!misfit.empty())
            ImGui::SetItemTooltip("Doesn't fit the actor: %s. Click to choose another", misfit.c_str());
        else if (pathWalk)
            ImGui::SetItemTooltip(
                "The walk along its path plays each stretch's look: another one sets the look of its "
                "gait (of the selected point's stretch only, if one is selected)");
        auto chosenAnimation = pathWalk ? shown : clip->animation;
        if (AnimationBrowser(BROWSER_POPUP, catalog, actor.models, chosenAnimation, animationFilter, animationCategory))
        {
            if (pathWalk)
            {
                // the look of its gait, the clip staying the path's walk
                const auto point = Components::Actors::GetSelectedWaypoint();
                Components::Actors::SetLook(actor, point && *point + 1 < actor.path.size() ? point : std::nullopt,
                                            chosenAnimation);
                Components::Actors::AddTriedLook(actor.id, chosenAnimation);
                Components::Actors::AddRecentAnimation(chosenAnimation);
            }
            else
            {
                // as picked, with what suits it instead of what suited the one before (a death as every death is made,
                // dying as it runs on the path's end), blending in as long as joins it
                // cleanly to the clip before
                const auto id = clip->id;
                const auto animations = Components::Actors::GetAnimationLookup();
                ActorLogic::SwapAnimation(actor, id, chosenAnimation, animations);
                clip = std::ranges::find(actor.clips, id, &Types::ActorClip::id);
                if (clip == actor.clips.end())
                    return;
                if (ActorLogic::IsDeathClip(*clip, animations))
                {
                    Components::Actors::AnchorDeath(actor, id);
                    clip = std::ranges::find(actor.clips, id, &Types::ActorClip::id);
                }
                Components::Actors::BlendCleanly(actor.id, id);
                // (the view's menu plays it again)
                Components::Actors::AddRecentAnimation(chosenAnimation);
                rejoin = id;
            }
        }
        if (clip->chain != Types::ActorChain::None)
        {
            Label("");
            ImGui::TextDisabled(ICON_FA_LINK " Auto transition");
            ImGui::SetItemTooltip(
                "Put in between the clips around it: a change here makes it yours, else it is put in "
                "again as they change");
        }

        Label("Starts");
        WhenControl("##clipStart", clip->start);

        Label("Length");
        float length = clip->length / 1000.0f;
        const auto onceWidth = ImGui::CalcTextSize("Once").x + ImGui::GetStyle().FramePadding.x * 2;
        const auto loopWidth =
            ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Loop").x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - onceWidth - loopWidth -
                                ImGui::GetStyle().ItemSpacing.x * 2);
        if (ImGui::DragFloat("##clipLength", &length, 0.05f, 0.05f, 3600.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
            clip->length = std::max(1u, static_cast<std::uint32_t>(length * 1000.0f));
        ImGui::SameLine();
        ImGui::BeginDisabled(!animation || clip->speed <= 0.0f);
        if (ImGui::Button("Once", ImVec2(onceWidth, 0)) && animation)
            clip->length = std::max(1u, static_cast<std::uint32_t>(std::max(animation->duration - clip->offset, 0.05f) /
                                                                   clip->speed * 1000.0f));
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("As long as the animation plays once");
        ImGui::SameLine();
        ImGui::Checkbox("Loop##clipLoop", &clip->loop);
        ImGui::SetItemTooltip("Plays the animation again when it ends, else holds its last frame");
        RenderSpinOut(actor, *clip);

        // how long it takes over from the clip before it, and how cleanly the two join, as their poses say
        constexpr auto THEN_BEFORE_POPUP = "##actorThenBefore";
        const auto index = static_cast<std::size_t>(clip - actor.clips.begin());
        std::optional<float> distance;
        if (index > 0)
        {
            const auto poses = Components::PoseSignatures::GetLookup(actor);
            const auto before = poses(actor.clips[index - 1].animation), now = poses(clip->animation);
            if (before && now)
                distance = TransitionLogic::GetPoseDistance(before->last, now->first);
        }
        {
            Label("Blend");
            const auto& style = ImGui::GetStyle();
            const auto autoWidth = ImGui::CalcTextSize("Auto").x + style.FramePadding.x * 2;
            const bool smooth = distance && TransitionLogic::IsCleanJoin(*distance, clip->blendIn);
            const auto join = !distance ? "" : smooth ? "Smooth" : ICON_FA_TRIANGLE_EXCLAMATION " Pops";
            const auto joinWidth = distance ? ImGui::CalcTextSize(join).x + style.ItemSpacing.x : 0.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - autoWidth - joinWidth - style.ItemSpacing.x);
            float blendIn = clip->blendIn / 1000.0f;
            if (ImGui::DragFloat("##clipBlendIn", &blendIn, 0.01f, 0.0f, 5.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
                clip->blendIn = static_cast<std::uint32_t>(blendIn * 1000.0f);
            ImGuiEx::Keyframeable::Record("actors.clip.blend");
            ImGui::SetItemTooltip("How long it takes over from the clip before it; Auto works it out from their poses");
            ImGui::SameLine();
            ImGui::BeginDisabled(index == 0);
            if (ImGui::Button("Auto##clipBlendAuto", ImVec2(autoWidth, 0)))
            {
                // the blend that joins it cleanly, or once their poses are worked out (the first time they are asked)
                if (const auto clean = clip->start > 0 ? Components::Actors::GetCleanBlend(actor, clip->start - 1,
                                                                                           clip->animation)
                                                       : std::nullopt)
                    clip->blendIn = *clean;
                else
                    Components::Actors::BlendCleanly(actor.id, clip->id);
            }
            ImGui::EndDisabled();
            ImGuiEx::Keyframeable::Record("actors.clip.blend.auto");
            ImGui::SetItemTooltip("%s", index == 0 ? "The first clip has none before it to blend from"
                                                   : "Works out the blend that joins it cleanly to the clip before");
            if (distance)
            {
                ImGui::SameLine();
                if (smooth)
                    ImGui::TextDisabled("%s", join);
                else
                {
                    // (a click opens the clip before's Then: a transition between them)
                    ImGui::TextColored(MisfitColor(), "%s", join);
                    if (ImGui::IsItemHovered())
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsItemClicked())
                        ImGui::OpenPopup(THEN_BEFORE_POPUP);
                }
                ImGuiEx::Keyframeable::Record("actors.clip.join");
                ImGui::SetItemTooltip("Its first pose is %.1f units from the last of the clip before (hips, spine, head, "
                                      "knees, ankles and wrists), over a blend of %u ms%s",
                                      *distance, clip->blendIn,
                                      smooth ? "" : ". Click for Then on the clip before: a transition between them");
            }
            if (ImGui::BeginPopup(THEN_BEFORE_POPUP))
            {
                const auto beforeId = actor.clips[index - 1].id;
                Components::Actors::Update(actor);
                const bool chosen = DrawThenItems(*Components::Actors::GetActor(actor.id), beforeId, "actors.join.then");
                ImGui::EndPopup();
                if (chosen)
                {
                    actor = *Components::Actors::GetActor(actor.id);
                    return;
                }
            }
        }

        // what can follow it: a transition, another animation (a killstreak's motions have none)
        constexpr auto THEN_POPUP = "##actorThen";
        std::optional<ActorLogic::ClipAfter> afterChosen;
        if (advanced)
        {
            AdvancedRows advancedRows;
            if (!Components::KillstreakLogic::IsMotion(clip->animation))
            {
                Label("");
                if (ImGui::Button(ICON_FA_FORWARD " Then...##clipThen", ImVec2(-FLT_MIN, 0)))
                    ImGui::OpenPopup(THEN_POPUP);
                ImGuiEx::Keyframeable::Record("actors.clip.then");
                ImGui::SetItemTooltip(
                    "A transition or animation after this clip: a stop, a turn, a crouch... The clips "
                    "after it make room");
            }

            // what the animation's notetracks do as it plays: shots, sounds
            if (animation)
            {
                const auto shots = std::ranges::count_if(animation->notetracks, [](const Types::ActorNotetrack& note) {
                    return ActorLogic::IsShotNotetrack(note.name);
                });
                const auto sounds = std::ranges::count_if(animation->notetracks, [](const Types::ActorNotetrack& note) {
                    return !ActorLogic::GetNotetrackSound(note.name).empty();
                });
                if (shots > 0)
                {
                    Label("");
                    ImGui::BeginDisabled(actor.weapon.empty());
                    ImGui::Checkbox(std::format("Shots ({})##clipShots", shots).c_str(), &clip->notetrackShots);
                    ImGui::EndDisabled();
                    ImGui::SetItemTooltip(actor.weapon.empty() ? "Give the actor a weapon to shoot"
                                                               : "Fires its weapon where the animation shoots");
                }
                if (sounds > 0)
                {
                    Label("");
                    ImGui::Checkbox("Sounds##clipSounds", &clip->notetrackSounds);
                    ImGui::SetItemTooltip("Plays the sounds the animation names, e.g. a reload's");
                }
            }

            // the left hand on the gun's grip, or as this clip's animation has it
            if (Components::Actors::GetActorKind(actor) == Types::ActorKind::Human)
            {
                Label("Left hand");
                ImGui::BeginDisabled(actor.weapon.empty());
                ImGui::Checkbox("Auto##clipGrip", &clip->grip);
                ImGui::EndDisabled();
                ImGuiEx::Keyframeable::Record("actors.clip.lefthand");
                ImGui::SetItemTooltip("%s", actor.weapon.empty()
                                                ? "Give the actor a weapon"
                                                : "On the gun's grip. Off: as the animation has it, on this clip");
            }

            const bool walks = actor.movement == Types::ActorMovement::Path;
            if (walks)
                RenderClipWalk(actor, *clip);

            Label("Speed");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGuiEx::DragSlider("##clipSpeed", &clip->speed, 0.0f, 3.0f, "%.2fx",
                                {.resetValue = 1.0f, .flags = ImGuiSliderFlags_AlwaysClamp});
            ImGuiEx::Keyframeable::Record("actors.clip.speed");

            Label("Offset");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##clipOffset", &clip->offset, 0.01f, 0.0f, animation ? animation->duration : 600.0f,
                             "%.2f s", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Where in the animation the clip starts");

            afterChosen = RenderClipAfter(actor, *clip);
            RenderClipRootMotion(actor, *clip);

            if (walks && clip->moves)
                RenderClipWalkDetails(actor, *clip);

            // what the legs play under it, the transitions put in, presets of clips
            if (Components::Actors::GetActorKind(actor) != Types::ActorKind::Vehicle)
            {
                RenderLegs(actor);
                Label("Transitions");
                bool automatic = actor.autoTransitions;
                if (ImGui::Checkbox("Auto##actorAutoTransitions", &automatic))
                    Components::Actors::SetAutoTransitions(actor, automatic);
                ImGuiEx::Keyframeable::Record("actors.autotransitions");
                ImGui::SetItemTooltip(
                    "Joins its clips and its path with the game's transitions (starts, stops, turns, changes of "
                    "stance), put in again as they change. Off takes them out");
            }
            ImGui::SeparatorText("Presets");
            RenderPresets(Components::Actors::PresetKind::Clips, actor);

            // where the clip stops playing its animation: where it ends or the next clip starts
            const auto next = std::next(clip);
            const auto end = std::min(clip->End(), next != actor.clips.end() ? next->start : UINT32_MAX);
            if (animation)
                RenderReload(actor, *clip, *animation, end);
        }
        if (ImGui::BeginPopup(THEN_POPUP))
        {
            const auto clipId = clip->id;
            Components::Actors::Update(actor);
            const auto stored = *Components::Actors::GetActor(actor.id);
            const bool chosen = DrawThenItems(stored, clipId, "actors.then");
            ImGui::EndPopup();
            if (chosen)
            {
                actor = *Components::Actors::GetActor(actor.id);
                return;
            }
        }

        if (clip->chain != Types::ActorChain::None && *clip != unedited)
            clip->chain = Types::ActorChain::None;

        // (last: they add or take out clips)
        const auto clipId = clip->id;
        if (afterChosen)
            ActorLogic::SetClipAfter(actor, clipId, *afterChosen, Mod::GetGameInterface()->GetDemoInfo().endTick);
        if (rejoin)
            Components::Actors::JoinAround(actor, *rejoin);
    }

    // A clip of the Face row: an animation of the face alone, which plays over the face (see ActorLogic::GetFace)
    void ActorsMenu::RenderFaceClip(const Types::Actor& actor, Types::ActorClip& clip, const Catalog& catalog)
    {
        constexpr auto BROWSER_POPUP = "##actorFaceBrowser";
        Label("Face");
        const auto animation = catalog.FindAnimation(clip.animation);
        if (ImGui::Button(
                std::format("{}{}##faceClipAnimation", clip.animation, animation ? "" : " (not loaded)").c_str(),
                ImVec2(-FLT_MIN, 0)))
            ImGui::OpenPopup(BROWSER_POPUP);
        ImGuiEx::Keyframeable::Record("actors.faceclip.animation");
        ImGui::SetItemTooltip("An animation of the face alone, which plays over the face while the clip shows");
        if (AnimationBrowser(BROWSER_POPUP, catalog, actor.models, clip.animation, animationFilter, faceCategory, true))
            if (const auto chosen = catalog.FindAnimation(clip.animation))
                clip.loop = chosen->loop;

        Label("Starts");
        WhenControl("##faceClipStart", clip.start);

        Label("Length");
        float length = clip.length / 1000.0f;
        const auto onePlayWidth = ImGui::CalcTextSize("Once").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - onePlayWidth - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::DragFloat("##faceClipLength", &length, 0.05f, 0.05f, 3600.0f, "%.2f s",
                             ImGuiSliderFlags_AlwaysClamp))
            clip.length = std::max(1u, static_cast<std::uint32_t>(length * 1000.0f));
        ImGui::SameLine();
        ImGui::BeginDisabled(!animation || clip.speed <= 0.0f);
        if (ImGui::Button("Once##face", ImVec2(-FLT_MIN, 0)) && animation)
            clip.length = std::max(1u, static_cast<std::uint32_t>(std::max(animation->duration - clip.offset, 0.05f) /
                                                                  clip.speed * 1000.0f));
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Makes the clip as long as the animation plays once");

        Label("");
        ImGui::Checkbox("Loop##faceClipLoop", &clip.loop);
        ImGui::SetItemTooltip("Plays the animation again when it ends, else holds its last frame until the clip ends");
        Label("Blend");
        float blendIn = clip.blendIn / 1000.0f;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragFloat("##faceClipBlendIn", &blendIn, 0.01f, 0.0f, 5.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
            clip.blendIn = static_cast<std::uint32_t>(blendIn * 1000.0f);
        ImGuiEx::Keyframeable::Record("actors.faceclip.blend");
        ImGui::SetItemTooltip("How long it takes over from the face before it");

        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled(actor.face == Types::ActorFace::Auto ? "Over its own face (Look: Face) while it shows"
                                                                 : "Its own face is off (Look: Face)");
        ImGui::PopTextWrapPos();

        if (IsAdvanced(Page::Animation))
        {
            AdvancedRows advancedRows;
            Label("Speed");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGuiEx::DragSlider("##faceClipSpeed", &clip.speed, 0.0f, 3.0f, "%.2fx",
                                {.resetValue = 1.0f, .flags = ImGuiSliderFlags_AlwaysClamp});
            Label("Offset");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat("##faceClipOffset", &clip.offset, 0.01f, 0.0f, animation ? animation->duration : 600.0f,
                             "%.2f s", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Where in the animation the clip starts");
        }
    }

    // What the actor does once a clip ends (see ActorLogic::GetClipAfter): holds its last frame, or goes on with what
    // it played before it; returns what was chosen
    std::optional<ActorLogic::ClipAfter> ActorsMenu::RenderClipAfter(const Types::Actor& actor,
                                                                     const Types::ActorClip& clip)
    {
        using ActorLogic::ClipAfter;
        Label("End");
        const auto after = ActorLogic::GetClipAfter(actor, clip.id);
        const bool first = !actor.clips.empty() && actor.clips.front().id == clip.id;
        std::optional<ClipAfter> chosen;
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::BeginDisabled(after == ClipAfter::Next);
        const auto preview = after == ClipAfter::Next ? "Next clip" : after == ClipAfter::Resume ? "Resume" : "Hold";
        if (ImGui::BeginCombo("##clipAfter", preview))
        {
            ImGui::BeginDisabled(first);
            if (ImGui::Selectable("Resume", after == ClipAfter::Resume))
                chosen = ClipAfter::Resume;
            ImGui::EndDisabled();
            ImGui::SetItemTooltip(first ? "Nothing plays before it"
                                        : "The clip before it goes on after it, its animation where it would have "
                                          "been: a clip of it after this one, up to the next clip");
            ImGuiEx::Keyframeable::Record("actors.clip.after.resume");
            if (ImGui::Selectable("Hold", after == ClipAfter::Hold))
                chosen = ClipAfter::Hold;
            ImGui::SetItemTooltip("The actor stays as the clip ends until the next clip starts, as a death lies");
            ImGuiEx::Keyframeable::Record("actors.clip.after.hold");
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGuiEx::Keyframeable::Record("actors.clip.after");
        ImGui::SetItemTooltip(after == ClipAfter::Next
                                  ? "The next clip starts as this one ends"
                                  : "What the actor does once the clip ends: hold its last frame, or go on with what "
                                    "it played before the clip");
        return chosen != after ? chosen : std::nullopt;
    }

    // Whether a clip that doesn't walk the path moves the actor as far as its animation moves the root (see
    // ActorLogic::RootMotion): a death falls where it falls, unless it would go into a wall the actor stands by
    void ActorsMenu::RenderClipRootMotion(const Types::Actor& actor, Types::ActorClip& clip)
    {
        const auto use = Components::Actors::GetRootMotionUse(actor, clip);
        if (use == ActorLogic::RootMotionUse::None)
            return;
        Label("Root motion");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##clipRootMotion", clip.rootMotion ? "Moves" : "In place"))
        {
            if (ImGui::Selectable("Moves", clip.rootMotion))
                clip.rootMotion = true;
            ImGui::SetItemTooltip(
                "The actor goes where the animation takes it (a death falls where it falls), and what "
                "comes after starts there");
            ImGuiEx::Keyframeable::Record("actors.clip.rootmotion.moves");
            if (ImGui::Selectable("In place", !clip.rootMotion))
                clip.rootMotion = false;
            ImGui::SetItemTooltip(
                "The actor stays where it is and the body moves around it, e.g. for a death next to a "
                "wall, which actors don't stop at");
            ImGuiEx::Keyframeable::Record("actors.clip.rootmotion.stays");
            ImGui::EndCombo();
        }
        ImGuiEx::Keyframeable::Record("actors.clip.rootmotion");
        ImGui::SetItemTooltip(
            "Whether the animation moves the actor along as it moves the body, as the game moves "
            "whatever plays it");
    }

    // What the actor's legs play at the playhead under its clips of upper body animations, or as its locomotion: read
    // only, as the clips and the Movement page's stance and locomotion have them (see ActorLogic::GetLayers)
    void ActorsMenu::RenderLegs(const Types::Actor& actor)
    {
        const auto layers = Components::Actors::GetLayers(actor, Components::Playback::GetActorTick(),
                                                          Components::Actors::GetBody(actor));
        Label("Legs");
        ImGui::PushTextWrapPos(0.0f);
        if (layers.legs.empty())
            ImGui::TextDisabled("As the clip has them");
        else
        {
            const auto& legs = *std::ranges::max_element(layers.legs, {}, &ActorLogic::LegsSample::weight);
            const auto& played = legs.clip ? legs.clip->animation : legs.animation;
            if (legs.clip || std::abs(legs.rate - 1.0f) < 0.005f)
                ImGui::TextDisabled("%s", played.c_str());
            else
                ImGui::TextDisabled("%s at %.2fx", played.c_str(), legs.rate);
        }
        ImGui::PopTextWrapPos();
        ImGui::SetItemTooltip(
            "What the legs play at the playhead: under clips that move only the upper body, such as reloads and "
            "shots, the game's idle of the stance for the gun held; with Locomotion on (Movement page), the walk, "
            "run or sprint for the actor's speed. The stance is on the Movement page");
    }

    // Whether a clip walks the actor along its path, and how, as its animation's measured motion and the Advanced
    // fold's direction and speed have it (with locomotion, the actor's speed)
    void ActorsMenu::RenderClipWalk(const Types::Actor& actor, Types::ActorClip& clip)
    {
        // (a death falls where it is)
        const bool death = ActorLogic::IsDeathClip(clip, Components::Actors::GetAnimationLookup());
        Label("");
        ImGui::BeginDisabled(death);
        ImGui::Checkbox("Follows path##clipMoves", &clip.moves);
        ImGui::EndDisabled();
        ImGuiEx::Keyframeable::Record("actors.clip.moves");
        if (death)
        {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("A death falls where the actor is: its root motion moves it, not the path");
        }
        else
            ImGui::SetItemTooltip(
                "Moves the actor along its path as far as the animation steps, facing the way the "
                "animation walks: a strafe left goes along the path facing to its right. Off, the animation moves it "
                "as far as it moves the body (Advanced: Root motion), and walking on goes on from there");
        if (!clip.moves)
            return;

        const auto walk = Components::Actors::GetClipWalk(actor, clip);
        // with locomotion, its clips of the players' movement walk at the actor's speed, and upper body clips on with
        // the walk before them
        const auto catalog = Components::Actors::GetCatalog();
        const auto animation = catalog ? catalog->FindAnimation(clip.animation) : nullptr;
        const bool automatic = actor.locomotion == Types::ActorLocomotion::Auto && clip.walkSpeed <= 0.0f;
        const bool locomotion = automatic && animation && animation->kind != Types::AnimationKind::Torso &&
                                ActorLogic::FindLocomotion(actor, clip.animation);
        const bool walksOn = automatic && animation && animation->kind == Types::AnimationKind::Torso;
        Label("");
        if (walk.angle)
        {
            const auto angle = *walk.angle;
            const char* way = std::abs(angle) <= 30.0f    ? "forward"
                              : std::abs(angle) >= 150.0f ? "back"
                              : angle > 0.0f              ? "left"
                                                          : "right";
            ImGui::TextDisabled("%c%s, %.0f u/s%s", std::toupper(way[0]), way + 1, walk.speed * clip.speed,
                                locomotion ? " (its gait)"
                                : walksOn  ? " (the walk before it)"
                                           : "");
        }
        else if (!locomotion && !Components::Actors::GetAnimationMotion(clip.animation) && clip.walkSpeed <= 0.0f)
            ImGui::TextDisabled("Measuring...");
        else
            ImGui::TextDisabled("In place");
    }

    // which way and how fast a clip walks the actor, in place of its animation's
    void ActorsMenu::RenderClipWalkDetails(const Types::Actor& actor, Types::ActorClip& clip)
    {
        const auto walk = Components::Actors::GetClipWalk(actor, clip);
        Label("Direction");
        static constexpr const char* DIRECTIONS[] = {"Auto", "Forward", "Back", "Left", "Right"};
        auto direction = static_cast<int>(clip.direction);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##clipDirection", &direction, DIRECTIONS, IM_ARRAYSIZE(DIRECTIONS)))
            clip.direction = static_cast<Types::ActorWalkDirection>(direction);
        ImGui::SetItemTooltip("Which way the clip walks the actor, relative to where it faces");

        Label("Path speed");
        bool ownSpeed = clip.walkSpeed > 0.0f;
        if (ImGui::Checkbox("##clipOwnSpeed", &ownSpeed))
            clip.walkSpeed = ownSpeed ? std::max(walk.speed, 50.0f) : 0.0f;
        ImGui::SetItemTooltip("Walks at a speed of its own instead of the animation's");
        ImGui::SameLine();
        ImGui::BeginDisabled(!ownSpeed);
        ImGui::SetNextItemWidth(-FLT_MIN);
        float speed = ownSpeed ? clip.walkSpeed : walk.speed;
        if (ImGui::DragFloat("##clipWalkSpeed", &speed, 1.0f, 1.0f, 1000.0f, "%.0f units/s",
                             ImGuiSliderFlags_AlwaysClamp) &&
            ownSpeed)
            clip.walkSpeed = speed;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Units a second at 1x; the clip's speed scales it");
    }

    namespace
    {
        // where a reload's moment came from, as the Animation page says it; brief where there is little room
        const char* DescribeSource(Types::ReloadSource source, bool brief = false)
        {
            switch (source)
            {
                case Types::ReloadSource::Override:
                    return "yours";
                case Types::ReloadSource::Curated:
                    return brief ? "table" : "from the mod's table";
                case Types::ReloadSource::ClipNotetracks:
                    return brief ? "notetracks" : "from its notetracks";
                case Types::ReloadSource::SoundNotetracks:
                    return brief ? "sounds" : "from the sounds";
                case Types::ReloadSource::Geometry:
                    return "measured";
                default:
                    return "";
            }
        }

        const char* ExplainSource(Types::ReloadSource source)
        {
            switch (source)
            {
                case Types::ReloadSource::Override:
                    return "You set it, for every actor playing this animation; Reset or right-click puts it back";
                case Types::ReloadSource::Curated:
                    return "From the table of reloads that comes with the mod, made by watching each one";
                case Types::ReloadSource::ClipNotetracks:
                    return "From the animation's own notes of when the clip drops, comes and goes in, as SP scripts "
                           "use them";
                case Types::ReloadSource::SoundNotetracks:
                    return "From when the animation plays the magazine's sounds";
                case Types::ReloadSource::Geometry:
                    return "Measured from where the hand goes: to the magazine, away to a pouch and back";
                default:
                    return "";
            }
        }

        // what each moment means, and its colour on the strip
        struct MomentLook
        {
            const char* name;
            const char* meaning;
            ImU32 color;
        };
        MomentLook GetMomentLook(ReloadLogic::Moment moment)
        {
            switch (moment)
            {
                case ReloadLogic::Moment::Out:
                    return {"Out", "The magazine leaves the gun: into the hand that pulls it, or falling",
                            IM_COL32(240, 150, 60, 255)};
                case ReloadLogic::Moment::Swap:
                    return {"Swap", "The hand has the new magazine from here, and lets go of the old one it pulled",
                            IM_COL32(235, 210, 80, 255)};
                default:
                    return {"In", "The new magazine is on the gun from here", IM_COL32(110, 210, 110, 255)};
            }
        }

        const char* DescribeKind(Types::ReloadKind kind)
        {
            switch (kind)
            {
                case Types::ReloadKind::Magazine:
                    return "pulled out";
                case Types::ReloadKind::DroppedMagazine:
                    return "dropped";
                case Types::ReloadKind::Box:
                    return "an LMG box (not drawn yet)";
                case Types::ReloadKind::Shells:
                    return "shotgun shells (not drawn yet)";
                case Types::ReloadKind::Rocket:
                    return "a rocket (not drawn yet)";
                default:
                    return "off";
            }
        }

        const char* DescribeHand(Types::ReloadHand hand)
        {
            switch (hand)
            {
                case Types::ReloadHand::HandTag:
                    return "as SP animations hold it";
                case Types::ReloadHand::RightWrist:
                    return "right hand";
                default:
                    return "left hand";
            }
        }

        // what a reload does, in a sentence
        std::string DescribeReload(const Types::ReloadProfile& profile)
        {
            const char* hand = profile.hand == Types::ReloadHand::LeftWrist    ? "the left hand"
                               : profile.hand == Types::ReloadHand::RightWrist ? "the right hand"
                                                                               : "the hand";
            switch (profile.kind)
            {
                case Types::ReloadKind::Magazine:
                    return std::format("{}{} pulls the magazine out, swaps it for a new one and puts that in.",
                                       static_cast<char>(std::toupper(hand[0])), hand + 1);
                case Types::ReloadKind::DroppedMagazine:
                    return std::format("The magazine drops from the gun; {} puts a new one in.", hand);
                case Types::ReloadKind::None:
                    if (profile.kindSource == Types::ReloadSource::Override)
                        return "Off: the magazine stays on the gun.";
                    return profile.note.empty() ? "No reload found in this animation." : profile.note;
                default:
                    return profile.note;
            }
        }

        std::string Capitalized(std::string_view text)
        {
            std::string out(text);
            if (!out.empty())
                out[0] = static_cast<char>(std::toupper(out[0]));
            return out;
        }
    }  // namespace

    // The reload a clip's animation does, as its reload profile says (see Components::ReloadProfiles): what happens to
    // the magazine and the hand that holds it, a strip of a play of it with when the magazine goes out, is swapped
    // and goes in, where each of those came from, and the user's changes to them (for every actor playing the
    // animation), each one step to undo
    void ActorsMenu::RenderReload(const Types::Actor& actor, const Types::ActorClip& clip,
                                  const Catalog::Animation& animation, std::uint32_t end)
    {
        using ReloadLogic::Moment;
        using Types::ReloadKind;
        const auto weapon = Components::Actors::ResolveWeapon(actor);
        const auto body = actor.models.empty() ? std::string() : actor.models.front();
        const auto profile = weapon.empty() ? nullptr : Components::ReloadProfiles::Get(clip.animation, body, weapon);
        const auto override = Components::ReloadProfiles::GetOverride(clip.animation);
        if (!override && clip.animation.find("reload") == std::string::npos &&
            !(profile && ReloadLogic::SaysReload(*profile)))
            return;

        ImGui::SeparatorText("Reload");
        if (weapon.empty())
        {
            ImGui::TextDisabled("Give the actor a weapon to see its magazine");
            return;
        }
        if (!profile)
        {
            ImGui::TextDisabled("Working out the reload...");
            return;
        }
        const auto entry = override.value_or(ReloadLogic::ProfileEntry{});
        const auto change = [&](const ReloadLogic::ProfileEntry& changed, std::string_view what) {
            Components::ReloadProfiles::ChangeOverride(clip.animation, changed,
                                                       std::format("{} of {}", what, clip.animation));
        };
        const bool magazine = profile->kind == ReloadKind::Magazine || profile->kind == ReloadKind::DroppedMagazine;

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", DescribeReload(*profile).c_str());
        ImGui::PopStyleColor();

        // what happens to the magazine: as worked out, or as the user says
        Label("Magazine");
        const auto worked =
            profile->kindSource == Types::ReloadSource::None ? "none found" : DescribeKind(profile->kind);
        const auto kindPreview = entry.kind ? Capitalized(DescribeKind(*entry.kind)) : std::format("Auto: {}", worked);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##reloadKind", kindPreview.c_str()))
        {
            struct Choice
            {
                std::optional<ReloadKind> kind;
                const char* name;
                const char* tooltip;
            };
            const std::array<Choice, 4> KINDS{{
                {std::nullopt, "auto", "As the reload is worked out from the animation and the gun"},
                {ReloadKind::Magazine, "pulled", "The hand pulls the magazine out and swaps it for a new one"},
                {ReloadKind::DroppedMagazine, "dropped", "The magazine falls from the gun; the hand brings a new one"},
                {ReloadKind::None, "off", "The magazine stays on the gun"},
            }};
            for (const auto& [kind, name, tooltip] : KINDS)
            {
                const auto label = kind ? Capitalized(DescribeKind(*kind)) : std::string("Auto");
                if (ImGui::Selectable(label.c_str(), entry.kind == kind))
                    change(ReloadLogic::WithKind(entry, kind, *profile),
                           kind ? std::format("set magazine {}", DescribeKind(*kind)) : "set magazine auto");
                ImGui::SetItemTooltip("%s", tooltip);
                RecordReloadControl(std::format("magazine.{}", name));
            }
            ImGui::EndCombo();
        }
        RecordReloadControl("magazine");

        if (magazine && profile->out && profile->in)
        {
            // a play of the animation: the one the clip is in at the playhead, else its first
            const auto duration = animation.duration;
            const auto tick = Components::Playback::GetActorTick();
            const bool playing = tick >= clip.start && tick < end;
            const auto play = ActorLogic::GetPlayAt(clip, duration, playing ? tick : clip.start);
            const auto tickOf = [&](float time) -> std::optional<std::uint32_t> {
                const auto at = ActorLogic::GetTickInPlay(clip, duration, play, time);
                return at && *at >= clip.start && *at < end ? at : std::nullopt;
            };
            const auto toPlayhead = [&](Moment moment) {
                auto changed = entry;
                ReloadLogic::GetMoment(changed, moment) = ReloadLogic::PlaceMoment(
                    *profile, moment,
                    ActorLogic::GetTimeInPlay(clip, duration, ActorLogic::GetPlayAt(clip, duration, tick), tick));
                change(changed, std::format("move {} to the playhead", ReloadLogic::GetMomentName(moment)));
            };
            const auto toAuto = [&](Moment moment) {
                auto changed = entry;
                ReloadLogic::GetMoment(changed, moment).reset();
                change(changed, std::format("reset {}", ReloadLogic::GetMomentName(moment)));
            };

            ReloadStripInput strip{
                .shownFrom = std::clamp(ActorLogic::GetTimeInPlay(clip, duration, play, clip.start), 0.0f, 1.0f),
                .shownTo = std::clamp(ActorLogic::GetTimeInPlay(clip, duration, play, end), 0.0f, 1.0f),
            };
            if (playing)
                strip.playhead = ActorLogic::GetTimeInPlay(clip, duration, play, tick);
            std::vector<Moment> moments;
            for (const auto moment : ReloadLogic::MOMENTS)
            {
                const auto& at = ReloadLogic::GetMoment(*profile, moment);
                if (!at)
                    continue;
                const auto look = GetMomentLook(moment);
                moments.push_back(moment);
                strip.markers.push_back({
                    .label = std::string(ReloadLogic::GetMomentName(moment)),
                    .time = at->time,
                    .color = look.color,
                    .own = at->source == Types::ReloadSource::Override,
                    .tooltip = std::format("{}: {:.3f} of a play, {:.2f} s into the animation, {}\n{}\n{}", look.name,
                                           at->time, at->time * duration, DescribeSource(at->source), look.meaning,
                                           ExplainSource(at->source)),
                });
            }

            const auto moved = ReloadStrip("##reloadStrip", strip);
            if (moved.dragged)
            {
                const auto moment = moments[*moved.dragged];
                auto changed = entry;
                auto& time = ReloadLogic::GetMoment(changed, moment);
                time = ReloadLogic::PlaceMoment(*profile, moment, moved.time);
                if (moved.released)
                {
                    change(changed, std::format("move {}", ReloadLogic::GetMomentName(moment)));
                    reloadPreview.reset();
                }
                else
                {
                    // used as it is dragged, with the playhead along, so the view shows the pose there
                    Components::ReloadProfiles::PreviewOverride(clip.animation, changed);
                    reloadPreview = ReloadPreview{clip.animation, changed, ImGui::GetFrameCount()};
                    if (const auto at = tickOf(*time))
                        Seek(*at, true);
                }
            }
            if (moved.scrubbed)
            {
                if (const auto at = ActorLogic::GetTickInPlay(clip, duration, play, *moved.scrubbed))
                    Seek(std::clamp<std::int64_t>(*at, clip.start, std::max(end, clip.start + 1) - 1), true);
            }

            // right-click on a marker: go there, to the playhead, back to what is worked out
            constexpr auto MARKER_MENU = "##reloadMarkerMenu";
            if (moved.menu)
            {
                reloadMenu = moments[*moved.menu];
                ImGui::OpenPopup(MARKER_MENU);
            }
            if (ImGui::BeginPopup(MARKER_MENU))
            {
                const auto moment = reloadMenu.value_or(Moment::Swap);
                const auto& at = ReloadLogic::GetMoment(*profile, moment);
                const auto there = at ? tickOf(at->time) : std::nullopt;
                ImGui::TextDisabled("%s", GetMomentLook(moment).name);
                if (ImGui::MenuItem("Go there", nullptr, false, there.has_value()))
                    JumpTo(*there);
                RecordReloadControl("menu.there");
                if (ImGui::MenuItem("To the playhead", nullptr, false, playing))
                    toPlayhead(moment);
                RecordReloadControl("menu.playhead");
                if (ImGui::MenuItem("Back to auto", nullptr, false, ReloadLogic::GetMoment(entry, moment).has_value()))
                    toAuto(moment);
                RecordReloadControl("menu.auto");
                ImGui::EndPopup();
            }

            // each moment: when it is on the timeline in that play, where it came from, and buttons to go there and
            // to move it to the playhead
            const auto& style = ImGui::GetStyle();
            const ImVec2 buttonSize(ImGui::GetFrameHeight(), 0);
            for (const auto moment : moments)
            {
                const auto& at = *ReloadLogic::GetMoment(*profile, moment);
                const auto look = GetMomentLook(moment);
                const auto name = std::string(ReloadLogic::GetMomentName(moment));
                const auto there = tickOf(at.time);
                ImGui::PushID(name.c_str());
                Label(look.name);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(look.color), "%s",
                                   there ? FormatActorTime(*there).c_str() : "not in the clip");
                ImGui::SetItemTooltip("%s\n%.3f of a play, %.2f s into the animation%s", look.meaning, at.time,
                                      at.time * duration, there ? std::format("; tick {}", *there).c_str() : "");
                ImGui::SameLine();
                const auto buttonsX = ImGui::GetWindowContentRegionMax().x - buttonSize.x * 2 - style.ItemSpacing.x;
                const auto room = buttonsX - ImGui::GetCursorPosX() - style.ItemSpacing.x;
                const auto source = DescribeSource(at.source);
                ImGui::TextDisabled("%s",
                                    ImGui::CalcTextSize(source).x <= room ? source : DescribeSource(at.source, true));
                ImGui::SetItemTooltip("%s", ExplainSource(at.source));
                ImGui::SameLine(buttonsX);
                ImGui::BeginDisabled(!there);
                if (ImGui::Button(ICON_FA_FORWARD_STEP, buttonSize))
                    JumpTo(*there);
                ImGui::EndDisabled();
                RecordReloadControl(name + ".jump");
                ImGui::SetItemTooltip("Jump there");
                ImGui::SameLine();
                ImGui::BeginDisabled(!playing);
                if (ImGui::Button(ICON_FA_ARROWS_TO_DOT, buttonSize))
                    toPlayhead(moment);
                ImGui::EndDisabled();
                RecordReloadControl(name + ".playhead");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip(playing ? "To the playhead" : "To the playhead: it isn't in this clip");
                ImGui::PopID();
            }
        }

        // the hand that holds it
        if (magazine)
        {
            Label("Hand");
            const auto handPreview = entry.hand ? Capitalized(DescribeHand(*entry.hand))
                                                : std::format("Auto: {}", DescribeHand(profile->hand));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##reloadHand", handPreview.c_str()))
            {
                using Types::ReloadHand;
                struct Choice
                {
                    std::optional<ReloadHand> hand;
                    const char* name;
                    const char* tooltip;
                };
                const std::array<Choice, 4> HANDS{{
                    {std::nullopt, "auto",
                     "The hand that goes away from the gun, or tag_inhand where it moves with it"},
                    {ReloadHand::LeftWrist, "left", "Held in the left hand, as it was at the magazine"},
                    {ReloadHand::RightWrist, "right", "Held in the right hand, as it was at the magazine"},
                    {ReloadHand::HandTag, "tag", "On tag_inhand, where SP animations hold clips"},
                }};
                for (const auto& [hand, name, tooltip] : HANDS)
                {
                    const auto label = hand ? Capitalized(DescribeHand(*hand)) : std::string("Auto");
                    if (ImGui::Selectable(label.c_str(), entry.hand == hand))
                    {
                        auto changed = entry;
                        changed.hand = hand;
                        change(changed, hand ? std::format("set hand {}", name) : "set hand auto");
                    }
                    ImGui::SetItemTooltip("%s", tooltip);
                    RecordReloadControl(std::format("hand.{}", name));
                }
                ImGui::EndCombo();
            }
            RecordReloadControl("hand");
            if (!profile->note.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", profile->note.c_str());
                ImGui::PopStyleColor();
            }
        }

        ImGui::BeginDisabled(!override);
        if (ImGui::Button(ICON_FA_ROTATE_LEFT " Reset", ImVec2(-FLT_MIN, 0)))
        {
            Components::ReloadProfiles::ChangeOverride(clip.animation, std::nullopt,
                                                       std::format("reset reload of {}", clip.animation));
            Toasts::Show(std::format("Reload of {} worked out again", clip.animation), true);
        }
        ImGui::EndDisabled();
        RecordReloadControl("reset");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip(override ? "Forgets your changes to this animation's reload, for every actor playing it"
                                       : "Nothing changed: the reload is as worked out");
    }

    namespace
    {
        // a gun's row: the weapons, as a menu of a row's own name; empty for none
        void RenderGunList(const Types::LoadoutCatalog& loadouts, const std::string& record, std::string& gun,
                           std::vector<std::string>& attachments, const char* none, std::string* search)
        {
            const auto weapon = loadouts.FindWeapon(gun);
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool open = ImGui::BeginCombo(std::format("##{}", record).c_str(),
                                                weapon ? weapon->displayName.c_str() : none, ImGuiComboFlags_HeightLarge);
            if (!open)
            {
                ImGuiEx::Keyframeable::Record(record);
                return;
            }
            if (search)
            {
                FilterField("##actorWeaponSearch", *search);
                ImGuiEx::Keyframeable::Record(record + ".search");
            }
            if (ImGui::Selectable(none, gun.empty()))
            {
                gun.clear();
                attachments.clear();
            }
            ImGuiEx::Keyframeable::Record(record + ".none");
            // multiplayer's by category, then the campaign's and Zombies', each by kind;
            // one whose zone isn't built greyed, its tooltip saying why
            std::string category, subcategory;
            for (const auto& entry : loadouts.weapons)
            {
                if (search && !MatchesFilter(entry.displayName, *search) && !MatchesFilter(entry.name, *search))
                    continue;
                if (entry.category != category)
                {
                    category = entry.category;
                    subcategory.clear();
                    ImGui::SeparatorText(category.c_str());
                }
                if (entry.subcategory != subcategory)
                {
                    subcategory = entry.subcategory;
                    ImGui::TextDisabled("%s", subcategory.c_str());
                }
                ImGui::BeginDisabled(!entry.unbuilt.empty());
                if (ImGui::Selectable(std::format("{}##{}", entry.displayName, entry.name).c_str(), gun == entry.name) &&
                    gun != entry.name)
                {
                    gun = entry.name;
                    // attachments of another gun mostly don't fit
                    attachments.clear();
                }
                ImGui::EndDisabled();
                ImGuiEx::Keyframeable::Record(record + "." + entry.name);
                if (!entry.unbuilt.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", entry.unbuilt.c_str());
            }
            ImGui::EndCombo();
        }

        // the attachments as chips, leaving out those the gun can't have with the ones chosen
        void RenderAttachments(const Types::LoadoutCatalog& loadouts, const std::string& record, const std::string& gun,
                               std::vector<std::string>& attachments)
        {
            Label("Attachments");
            const float left = ImGui::GetCursorPosX();
            const float right = ImGui::GetContentRegionMax().x;
            bool any = false;
            for (const auto& attachment : loadouts.attachments)
            {
                const bool chosen = std::ranges::find(attachments, attachment.name) != attachments.end();
                auto with = attachments;
                const Types::LoadoutCatalog::Variant* adds = nullptr;
                if (!chosen)
                {
                    with.push_back(attachment.name);
                    std::ranges::sort(with);
                    adds = loadouts.FindVariant(gun, with);
                    if (!adds)
                        continue;
                }
                const float width =
                    ImGui::CalcTextSize(attachment.displayName.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
                if (any)
                {
                    ImGui::SameLine();
                    if (ImGui::GetCursorPosX() + width > right)
                    {
                        ImGui::NewLine();
                        ImGui::SetCursorPosX(left);
                    }
                }
                any = true;
                if (chosen)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                // (Pack-a-Punch of a weapon whose upgraded zone isn't built greyed, with why)
                const bool unbuilt = adds && !adds->unbuilt.empty();
                ImGui::BeginDisabled(unbuilt);
                if (ImGui::Button(std::format("{}##{}", attachment.displayName, record).c_str()))
                {
                    if (chosen)
                        std::erase(attachments, attachment.name);
                    else
                        attachments = with;
                }
                ImGui::EndDisabled();
                ImGuiEx::Keyframeable::Record(record + "." + attachment.name);
                if (unbuilt && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", adds->unbuilt.c_str());
                if (chosen)
                    ImGui::PopStyleColor();
            }
            if (!any)
                ImGui::TextDisabled("None for this gun");
        }

        // the camos as swatches
        void RenderCamos(const Types::LoadoutCatalog& loadouts, const char* label, const std::string& record,
                         std::int32_t& chosen)
        {
            Label(label);
            const float right = ImGui::GetContentRegionMax().x;
            const float size = ImGui::GetFrameHeight();
            const float swatchesLeft = ImGui::GetCursorPosX();
            for (std::size_t camo = 0; camo < loadouts.camos.size(); ++camo)
            {
                if (camo > 0)
                {
                    ImGui::SameLine();
                    if (ImGui::GetCursorPosX() + size > right)
                    {
                        ImGui::NewLine();
                        ImGui::SetCursorPosX(swatchesLeft);
                    }
                }
                const auto& entry = loadouts.camos[camo];
                const auto start = ImGui::GetCursorScreenPos();
                ImGui::PushID(static_cast<int>(camo));
                const ImVec4 color(((entry.color >> 16) & 0xFF) / 255.0f, ((entry.color >> 8) & 0xFF) / 255.0f,
                                   (entry.color & 0xFF) / 255.0f, 1.0f);
                if (ImGui::ColorButton(std::format("##{}", record).c_str(), color,
                                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(size, size)))
                    chosen = static_cast<std::int32_t>(camo);
                ImGuiEx::Keyframeable::Record(std::format("{}.{}", record, camo));
                ImGui::SetItemTooltip("%s", entry.displayName.c_str());
                if (chosen == static_cast<std::int32_t>(camo))
                    ImGui::GetWindowDrawList()->AddRect(ImVec2(start.x - 2, start.y - 2),
                                                        ImVec2(start.x + size + 2, start.y + size + 2),
                                                        ImGui::GetColorU32(ImGuiCol_Text), 0.0f, 0, 2.0f);
                ImGui::PopID();
            }
        }
    }  // namespace

    void ActorsMenu::RenderWeapon(Types::Actor& actor, const char* none, std::string* search)
    {
        const auto loadouts = Components::Loadouts::GetCatalog();
        if (!loadouts)
        {
            ImGui::TextDisabled("Finding the weapons of this map...");
            return;
        }

        Label("Weapon");
        RenderGunList(*loadouts, "actors.weapon", actor.weapon, actor.attachments, none, search);
        if (actor.weapon.empty())
            return;
        RenderAttachments(*loadouts, "actors.attachment", actor.weapon, actor.attachments);
        RenderCamos(*loadouts, "Camo", "actors.camo", actor.camo);
    }

    void ActorsMenu::RenderStowed(Types::Actor& actor)
    {
        const auto loadouts = Components::Loadouts::GetCatalog();
        if (!loadouts)
            return;
        Label("Stowed");
        RenderGunList(*loadouts, "actors.stowed", actor.stowedWeapon, actor.stowedAttachments, "Off", nullptr);
        if (actor.stowedWeapon.empty())
            return;
        RenderAttachments(*loadouts, "actors.stowed.attachment", actor.stowedWeapon, actor.stowedAttachments);
        RenderCamos(*loadouts, "Its camo", "actors.stowed.camo", actor.stowedCamo);
    }

    // the bones effects are put on most, by a name to show
    constexpr std::array<std::pair<const char*, const char*>, 17> EFFECT_TAGS{{
        {"j_head", "Head"},
        {"j_neck", "Neck"},
        {"j_spine4", "Chest"},
        {"j_spinelower", "Belly"},
        {"j_mainroot", "Hips"},
        {"j_shoulder_le", "Left shoulder"},
        {"j_shoulder_ri", "Right shoulder"},
        {"j_elbow_le", "Left elbow"},
        {"j_elbow_ri", "Right elbow"},
        {"j_wrist_le", "Left hand"},
        {"j_wrist_ri", "Right hand"},
        {"j_knee_le", "Left knee"},
        {"j_knee_ri", "Right knee"},
        {"j_ankle_le", "Left foot"},
        {"j_ankle_ri", "Right foot"},
        {"tag_origin", "Feet (origin)"},
        {"tag_flash", "Muzzle"},
    }};

    std::string DescribeTag(const Types::ActorEvent& event)
    {
        const auto tag = std::ranges::find(EFFECT_TAGS, event.tag, [](const auto& entry) {
            return std::string_view(entry.first);
        });
        return event.tag.empty()             ? std::string(event.type == Types::ActorEventType::Hit ? "Chest" : "Feet")
               : tag != EFFECT_TAGS.end() ? std::string(tag->second)
                                             : event.tag;
    }

    // the players of the demo by client number, from the kill finder's scan
    std::vector<std::pair<std::int32_t, std::string>> GetPlayers()
    {
        std::vector<std::pair<std::int32_t, std::string>> players;
        if (const auto events = Components::KillFinder::GetEvents())
            for (const auto& [client, player] : events->players)
                players.emplace_back(client, player.name);
        std::ranges::sort(players, {}, [](const auto& player) { return player.second; });
        return players;
    }

    // what a Look event looks at, e.g. "the camera", "Actor 2"
    std::string DescribeLook(const Types::Actor& actor, const Types::ActorEvent& event)
    {
        switch (event.lookTarget)
        {
            case Types::ActorLookTarget::Ahead:
                return "Ahead";
            case Types::ActorLookTarget::Camera:
                return "The camera";
            case Types::ActorLookTarget::Point:
                return std::format("{:.0f} {:.0f} {:.0f}", event.lookPoint.x, event.lookPoint.y, event.lookPoint.z);
            case Types::ActorLookTarget::Actor:
            {
                const auto other = Components::Actors::GetActor(event.lookActor);
                return other && other->id != actor.id ? other->name : "No actor";
            }
            case Types::ActorLookTarget::Player:
            {
                const auto players = GetPlayers();
                const auto player = std::ranges::find(players, event.lookPlayer, [](const auto& p) { return p.first; });
                return player != players.end() ? player->second : std::format("Player {}", event.lookPlayer);
            }
        }
        return {};
    }

    // what an event does beyond its type, for the list: "5 shots, 100 ms", "Chest, dies"
    std::string DescribeEventDetails(const Types::Actor& actor, const Types::ActorEvent& event)
    {
        switch (event.type)
        {
            case Types::ActorEventType::Fire:
                return event.shots == 1 ? "1 shot" : std::format("{} shots, {} ms", event.shots, event.interval);
            case Types::ActorEventType::Hit:
                // (a Hit and its death are one)
                if (const auto* death = ActorLogic::FindDeathClip(actor, event))
                    return DescribeTag(event) + " " ICON_FA_ARROW_RIGHT " dies: " + ShownName(death->animation);
                return DescribeTag(event);
            case Types::ActorEventType::Look:
                return DescribeLook(actor, event);
            case Types::ActorEventType::Effect:
            {
                // the effect's own name, without its folder
                const auto slash = event.effect.find_last_of('/');
                return event.effect.empty() ? "No effect yet"
                                            : event.effect.substr(slash == std::string::npos ? 0 : slash + 1);
            }
            default:
                return {};
        }
    }

    std::string_view DescribeEvent(Types::ActorEventType type)
    {
        switch (type)
        {
            case Types::ActorEventType::Fire:
                return ICON_FA_GUN " Fire";
            case Types::ActorEventType::Hit:
                return ICON_FA_DROPLET " Hit";
            case Types::ActorEventType::Effect:
                return ICON_FA_WAND_MAGIC_SPARKLES " Effect";
            case Types::ActorEventType::Show:
                return ICON_FA_EYE " Show";
            case Types::ActorEventType::Hide:
                return ICON_FA_EYE_SLASH " Hide";
            case Types::ActorEventType::Look:
                return ICON_FA_CROSSHAIRS " Look";
        }
        return "";
    }

    void ActorsMenu::RenderEvents(Types::Actor& actor, const Catalog& catalog)
    {
        const auto tick = Components::Playback::GetActorTick();
        auto add = [&](Types::ActorEvent event) {
            event.tick = tick;
            Components::Actors::Update(actor);
            // (a killstreak's as the game fires it, which may be a death, or at another tick: see MakeFire)
            if (event.type == Types::ActorEventType::Fire &&
                (actor.mortar || Components::KillstreakLogic::FindByModels(actor.models)))
                ActorActions::AddKillstreakFire(actor.id);
            else
                Components::Actors::SelectEvent(Components::Actors::AddEvent(actor.id, event));
            actor = *Components::Actors::GetActor(actor.id);
        };
        const auto kind = Components::Actors::GetActorKind(actor);
        const bool vehicle = kind == Types::ActorKind::Vehicle;
        // a killstreak's guns, which its Fire events fire and its Look events aim
        const auto killstreak = vehicle ? Components::KillstreakLogic::FindByModels(actor.models) : std::nullopt;
        const auto guns = killstreak ? Components::KillstreakLogic::Get(*killstreak).guns
                                     : std::span<const Components::KillstreakLogic::Gun>{};
        const auto unarmed =
            std::format("The {} has no gun", killstreak ? Components::KillstreakLogic::Get(*killstreak).name : "vehicle");

        // + Event: each kind by its name, added at the playhead
        constexpr auto EVENT_POPUP = "##addEvent";
        if (ImGui::Button(ICON_FA_PLUS " Event " ICON_FA_CARET_DOWN))
            ImGui::OpenPopup(EVENT_POPUP);
        ImGuiEx::Keyframeable::Record("actors.event.add");
        ImGui::SetItemTooltip("Adds an event at the playhead");
        if (ImGui::BeginPopup(EVENT_POPUP))
        {
            const auto item = [&](const char* label, const char* record, const std::string& tip,
                                  std::optional<std::string> unavailable, Types::ActorEvent event) {
                if (ImGui::MenuItem(label, nullptr, false, !unavailable))
                    add(event);
                ImGuiEx::Keyframeable::Record(std::string("actors.event.add.") + record);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", unavailable ? unavailable->c_str() : tip.c_str());
            };
            item(ICON_FA_GUN " Fire", "fire",
                 actor.mortar ? std::string("Shells fall on its points")
                 : killstreak ? std::string(Components::KillstreakLogic::DescribeFire(
                                    Components::KillstreakLogic::Get(*killstreak)))
                              : std::string("Fires its weapon"),
                 ActorActions::GetFireUnavailableReason(actor), Components::Actors::MakeGunFire(actor, 0));
            // (the Mortar Team has no body: its Fire, Show and Hide alone)
            const auto bodiless =
                actor.mortar ? std::optional<std::string>("The Mortar Team has no body") : std::nullopt;
            item(ICON_FA_DROPLET " Hit", "hit", "Bleeds from a bone, and dies if you choose",
                 bodiless  ? bodiless
                 : vehicle ? std::optional<std::string>("A vehicle doesn't bleed: an Effect plays explosions on it")
                           : std::nullopt,
                 {.type = Types::ActorEventType::Hit});
            item(ICON_FA_WAND_MAGIC_SPARKLES " Effect", "effect", "Plays an effect of the game on a bone", bodiless,
                 {.type = Types::ActorEventType::Effect, .tag = vehicle ? "tag_origin" : "j_spine4"});
            item(ICON_FA_CROSSHAIRS " Look", "look",
                 vehicle ? "Aims its guns at the camera, a point, an actor or a player"
                         : "Turns its head to the camera, a point, an actor or a player",
                 bodiless                  ? bodiless
                 : vehicle && guns.empty() ? std::optional(unarmed + " to aim")
                                           : std::nullopt,
                 {.type = Types::ActorEventType::Look});
            item(ICON_FA_EYE " Show", "show", "Shows it from here; before its first Show it is hidden", std::nullopt,
                 {.type = Types::ActorEventType::Show});
            item(ICON_FA_EYE_SLASH " Hide", "hide", "Hides it from here", std::nullopt,
                 {.type = Types::ActorEventType::Hide});
            ImGui::EndPopup();
        }

        if (actor.events.empty())
        {
            ImGui::TextDisabled("No events.");
            return;
        }

        const auto selectedEvent = Components::Actors::GetSelectedEvent();
        std::optional<std::pair<std::uint32_t, bool>> edit;  // an event to delete (or duplicate) from its menu
        const auto rows = std::min<std::size_t>(actor.events.size(), 5);
        if (ImGui::BeginChild("##actorEvents", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * rows + 8), true) &&
            ImGui::BeginTable("##actorEventTable", 3, ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("##type", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
            ImGui::TableSetupColumn("##time", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.0f);
            ImGui::TableSetupColumn("##details", ImGuiTableColumnFlags_WidthStretch);
            for (const auto& entry : actor.events)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(std::format("{}##event{}", DescribeEvent(entry.type), entry.id).c_str(),
                                      selectedEvent == entry.id,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    Components::Actors::SelectEvent(entry.id);
                    page = Page::Events;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        JumpTo(entry.tick);
                }
                if (ImGui::BeginPopupContextItem(std::format("##eventMenu{}", entry.id).c_str()))
                {
                    Components::Actors::SelectEvent(entry.id);
                    if (ImGui::MenuItem(ICON_FA_FORWARD_STEP " Go to"))
                        JumpTo(entry.tick);
                    if (ImGui::MenuItem(ICON_FA_CLONE " Duplicate", Commands::GetShortcut("edit.duplicate").c_str()))
                        edit = std::pair(entry.id, false);
                    if (ImGui::MenuItem(ICON_FA_TRASH " Delete", Commands::GetShortcut("edit.delete").c_str()))
                        edit = std::pair(entry.id, true);
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", FormatActorTime(entry.tick).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", DescribeEventDetails(actor, entry).c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        if (edit)
        {
            page = Page::Events;
            Components::Actors::SelectEvent(edit->first);
            DeleteOrDuplicate(actor, edit->second);
            return;
        }

        auto event = std::ranges::find(actor.events, selectedEvent.value_or(0), &Types::ActorEvent::id);
        if (event == actor.events.end())
            return;

        Label("At");
        auto at = event->tick;
        if (WhenControl("##eventTick", at))
        {
            ActorLogic::MoveEvent(actor, event->id, at);
            event = std::ranges::find(actor.events, selectedEvent.value_or(0), &Types::ActorEvent::id);
        }
        // (a killstreak's death's effects and Hide)
        if (event->type != Types::ActorEventType::Hit && event->death != 0)
        {
            Label("");
            ImGui::TextDisabled(ICON_FA_LINK " Part of its death: moves with it");
            ImGuiEx::Keyframeable::Record("actors.event.linked");
            ImGui::SetItemTooltip(
                "Goes off %s its death's %s, wherever the death is moved; deleting the death deletes "
                "it, and it can be moved, changed or deleted on its own",
                event->afterDeathEnd ? "after" : "from", event->afterDeathEnd ? "end" : "start");
        }

        if (event->type == Types::ActorEventType::Fire)
        {
            // a killstreak's guns or its rockets (the Gunship's)
            const auto firing = Components::KillstreakLogic::FindByModels(actor.models);
            if (firing && Components::KillstreakLogic::GetRockets(*firing))
            {
                Label("Fire");
                const auto& info = Components::KillstreakLogic::Get(*firing);
                if (const auto picked =
                        ButtonRow("##eventFireWeapon",
                                  {{ICON_FA_GUN, "Guns", "Its gun", false, !event->rockets, "actors.event.fire.guns"},
                                   {ICON_FA_ROCKET, "Rockets", "Its rockets, two at a time", false, event->rockets,
                                    "actors.event.fire.rockets"}}))
                    Components::KillstreakLogic::SetRockets(info, *event, *picked == 1);
            }
            Label("Shots");
            auto shots = static_cast<int>(event->shots);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragInt("##eventShots", &shots, 0.2f, 1, 500, "%d", ImGuiSliderFlags_AlwaysClamp))
                event->shots = static_cast<std::uint32_t>(shots);
            if (actor.mortar)
                ImGui::SetItemTooltip("Shells on each point");
        }
        if (event->type == Types::ActorEventType::Effect)
            RenderEffect(*event, catalog);
        if (event->type == Types::ActorEventType::Look)
            RenderLookAt(actor, *event);
        if (event->type == Types::ActorEventType::Hit)
        {
            // (which can change the actor, a death clip added)
            RenderDeath(actor, *event, catalog);
            event = std::ranges::find(actor.events, selectedEvent.value_or(0), &Types::ActorEvent::id);
            if (event == actor.events.end())
                return;
            // its blood (or any effect of the map) and where
            RenderEffect(*event, catalog);
        }

        // less used: how fast a burst fires and its tracers, how fast a look turns
        if (event->type != Types::ActorEventType::Fire && event->type != Types::ActorEventType::Look)
            return;
        if (!IsAdvanced(Page::Events))
            return;
        AdvancedRows advancedRows;
        switch (event->type)
        {
            case Types::ActorEventType::Fire:
            {
                Label("Interval");
                auto interval = static_cast<int>(event->interval);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragInt("##eventInterval", &interval, 1.0f, 30, 5000, "%d ms", ImGuiSliderFlags_AlwaysClamp))
                    event->interval = static_cast<std::uint32_t>(interval);
                ImGuiEx::Keyframeable::Record("actors.event.every");
                ImGui::SetItemTooltip("The time between shots; an AK-47 fires every 80 ms");
                // (what a killstreak drops has none)
                if (const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models);
                    !actor.mortar && (!killstreak || !Components::KillstreakLogic::Get(*killstreak).guns.empty()))
                {
                    Label("Spread");
                    const auto loadouts = Components::Loadouts::GetCatalog();
                    const auto gun = loadouts ? loadouts->FindWeapon(actor.weapon) : nullptr;
                    auto spread =
                        Components::ShotLogic::GetSpread(1, gun ? gun->aimedSpread : 0.0f, event->spread).degrees;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::SliderFloat("##eventSpread", &spread, 0.0f, Components::ShotLogic::MOST_SPREAD,
                                           "%.1f degrees", ImGuiSliderFlags_AlwaysClamp))
                        event->spread = spread;
                    ImGuiEx::Keyframeable::Record("actors.event.spread");
                    ImGui::SetItemTooltip("How far shots go off the barrel");
                    Label("");
                    ImGui::Checkbox("Tracers##eventTracers", &event->tracers);
                    Label("");
                    ImGui::Checkbox("Hits##eventHits", &event->hits);
                    ImGuiEx::Keyframeable::Record("actors.event.hits");
                    ImGui::SetItemTooltip("Holes, blood and blasts where shots land");
                }
                break;
            }
            case Types::ActorEventType::Look:
            {
                Label("Turn time");
                float turn = event->lookTurn / 1000.0f;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragFloat("##lookTurn", &turn, 0.01f, 0.0f, 5.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
                    event->lookTurn = static_cast<std::uint32_t>(turn * 1000.0f);
                ImGui::SetItemTooltip("How long the head takes to turn there from what it looked at before");
                break;
            }
            default:
                break;
        }
    }

    // The effect a Hit or Effect event plays, and the bone it plays on
    void ActorsMenu::RenderEffect(Types::ActorEvent& event, const Catalog& catalog)
    {
        const bool hit = event.type == Types::ActorEventType::Hit;
        Label("Effect");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto preview = event.effect.empty() ? std::string(hit ? "Blood" : "Choose an effect") : event.effect;
        const bool effectOpen = ImGui::BeginCombo("##eventEffect", preview.c_str(), ImGuiComboFlags_HeightLarge);
        ImGuiEx::Keyframeable::Record("actors.event.effect");
        if (effectOpen)
        {
            FilterField("##effectFilter", effectFilter);
            if (hit && effectFilter.empty() && ImGui::Selectable("Blood", event.effect.empty()))
                event.effect.clear();
            // blood first for hits
            std::vector<const std::string*> effects;
            for (const auto& effect : catalog.effects)
                if (MatchesFilter(effect, effectFilter))
                    effects.push_back(&effect);
            if (hit)
                std::ranges::stable_sort(effects, {}, [](const std::string* effect) {
                    return effect->find("flesh") == std::string::npos && effect->find("blood") == std::string::npos;
                });
            constexpr std::size_t MAX_LISTED = 400;
            for (std::size_t i = 0; i < effects.size() && i < MAX_LISTED; ++i)
                if (ImGui::Selectable(effects[i]->c_str(), event.effect == *effects[i]))
                    event.effect = *effects[i];
            if (effects.size() > MAX_LISTED)
                ImGui::TextDisabled("%zu more; search to find them", effects.size() - MAX_LISTED);
            if (effects.empty())
                ImGui::TextDisabled("Nothing found");
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The effects the map and the loaded pack have");

        Label("On");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool tagOpen = ImGui::BeginCombo("##eventTag", DescribeTag(event).c_str(), ImGuiComboFlags_HeightLarge);
        ImGuiEx::Keyframeable::Record("actors.event.tag");
        if (tagOpen)
        {
            for (const auto& [name, description] : EFFECT_TAGS)
                if (ImGui::Selectable(std::format("{}##{}", description, name).c_str(), event.tag == name))
                    event.tag = name;
            ImGui::EndCombo();
        }
    }

    // What a Look event looks at (how fast it turns is in the Advanced fold)
    void ActorsMenu::RenderLookAt(Types::Actor& actor, Types::ActorEvent& look)
    {
        Label("At");
        constexpr std::array TARGETS{"Ahead", "The camera", "A point", "An actor", "A player"};
        auto target = static_cast<int>(look.lookTarget);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##lookTarget", &target, TARGETS.data(), static_cast<int>(TARGETS.size())))
        {
            look.lookTarget = static_cast<Types::ActorLookTarget>(target);
            // a point starts where the camera is
            if (look.lookTarget == Types::ActorLookTarget::Point && look.lookPoint == glm::vec3(0))
                look.lookPoint = Components::CameraManager::Get().GetActiveCamera()->GetPosition();
        }
        ImGui::SetItemTooltip("Ahead stops looking: the head moves with its animation again");

        switch (look.lookTarget)
        {
            case Types::ActorLookTarget::Point:
            {
                Label("Point");
                const auto buttonWidth = ImGui::GetFrameHeight();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
                ImGui::DragFloat3("##lookPoint", &look.lookPoint.x, 1.0f, 0.0f, 0.0f, "%.0f");
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_VIDEO "##lookPointCamera", ImVec2(buttonWidth, 0)))
                    look.lookPoint = Components::CameraManager::Get().GetActiveCamera()->GetPosition();
                ImGui::SetItemTooltip("Where the camera is now");
                break;
            }
            case Types::ActorLookTarget::Actor:
            {
                Label("Actor");
                const auto other = Components::Actors::GetActor(look.lookActor);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##lookActor", other ? other->name.c_str() : "Choose an actor"))
                {
                    for (const auto& entry : Components::Actors::GetActors())
                        if (entry.id != actor.id &&
                            ImGui::Selectable(std::format("{}##{}", entry.name, entry.id).c_str(),
                                              entry.id == look.lookActor))
                            look.lookActor = entry.id;
                    ImGui::EndCombo();
                }
                break;
            }
            case Types::ActorLookTarget::Player:
            {
                Label("Player");
                const auto players = GetPlayers();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##lookPlayer", DescribeLook(actor, look).c_str(), ImGuiComboFlags_HeightLarge))
                {
                    for (const auto& [client, name] : players)
                        if (ImGui::Selectable(std::format("{}##{}", name, client).c_str(), client == look.lookPlayer))
                            look.lookPlayer = client;
                    if (players.empty())
                        ImGui::TextDisabled("Finding players...");
                    ImGui::EndCombo();
                }
                break;
            }
            default:
                break;
        }
    }

    // A Hit's death: the death clip that starts with it, to choose or take away
    void ActorsMenu::RenderDeath(Types::Actor& actor, Types::ActorEvent& hit, const Catalog& catalog)
    {
        constexpr auto DEATH_POPUP = "##actorDeathBrowser";
        const auto death = ActorLogic::FindDeathClip(actor, hit);
        Label("Death");
        // < > through the deaths that suit what it is doing then (proposals 4, 5, 11)
        const ImVec2 arrow(ImGui::GetFrameHeight(), 0);
        std::optional<std::string> stepped;
        if (death)
        {
            const auto candidates = Components::Actors::GetDeathCandidates(actor, hit.tick, 16);
            auto list = candidates.suited;
            list.insert(list.end(), candidates.others.begin(), candidates.others.end());
            const auto at = std::ranges::find(list, death->animation);
            const auto step = [&](int by) {
                if (list.empty())
                    return;
                const auto from = at != list.end() ? static_cast<int>(at - list.begin()) : -by;
                const auto count = static_cast<int>(list.size());
                stepped = list[static_cast<std::size_t>(((from + by) % count + count) % count)];
            };
            if (ImGui::Button(ICON_FA_CHEVRON_LEFT "##deathPrev", arrow))
                step(-1);
            ImGuiEx::Keyframeable::Record("actors.death.prev");
            ImGui::SetItemTooltip("The death before it, of those that suit it %s first", candidates.motion.c_str());
            ImGui::SameLine(0, 2);
        }
        const auto removeWidth = death ? (arrow.x + 2) * 2 + ImGui::GetStyle().ItemSpacing.x : 0.0f;
        // (a killstreak's death by its name: Explode, Spin out)
        const auto deathName = death && Components::KillstreakLogic::IsMotion(death->animation)
                                   ? std::string(Components::KillstreakLogic::GetMotionLabel(death->animation))
                               : death ? death->animation
                                       : std::string();
        const auto text = death ? std::format(ICON_FA_SKULL " {}##hitDeath", deathName) : std::string("None##hitDeath");
        if (ImGui::Button(text.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - removeWidth, 0)))
        {
            deathAnimation = death ? death->animation : std::string();
            animationCategory = "Deaths";
            browserLikeThis = deathAnimation;
            ImGui::OpenPopup(DEATH_POPUP);
        }
        ImGui::SetItemTooltip(death ? "Plays this death animation from the hit on; click to choose another"
                                    : "Plays a death animation you choose from the hit on, as a clip");
        if (death)
        {
            ImGui::SameLine(0, 2);
            if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "##deathNext", arrow) && !stepped)
            {
                const auto candidates = Components::Actors::GetDeathCandidates(actor, hit.tick, 16);
                auto list = candidates.suited;
                list.insert(list.end(), candidates.others.begin(), candidates.others.end());
                const auto at = std::ranges::find(list, death->animation);
                if (!list.empty())
                    stepped =
                        list[at != list.end() ? static_cast<std::size_t>(at - list.begin() + 1) % list.size() : 0];
            }
            ImGuiEx::Keyframeable::Record("actors.death.next");
            ImGui::SetItemTooltip("The death after it");
        }
        // (another death, as every death is made)
        const auto swapDeath = [&](const std::string& animation) {
            const auto id = death->id;
            ActorLogic::SwapAnimation(actor, id, animation, Components::Actors::GetAnimationLookup());
            Components::Actors::BlendCleanly(actor.id, id);
            Components::Actors::AddRecentAnimation(animation);
        };
        if (stepped && death)
        {
            if (catalog.FindAnimation(*stepped))
                swapDeath(*stepped);
            return;
        }
        if (death)
        {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_XMARK "##hitNoDeath", ImVec2(ImGui::GetFrameHeight(), 0)))
            {
                const auto id = death->id;
                std::erase_if(actor.clips, [&](const Types::ActorClip& clip) { return clip.id == id; });
                return;
            }
            ImGui::SetItemTooltip("Doesn't die: deletes the death clip");
        }

        if (!AnimationBrowser(DEATH_POPUP, catalog, actor.models, deathAnimation, animationFilter, animationCategory))
            return;
        const auto animation = catalog.FindAnimation(deathAnimation);
        if (!animation)
            return;
        // (Die here in the view's menu dies this way next)
        if (death)
        {
            swapDeath(deathAnimation);
            return;
        }
        Components::Actors::AddRecentAnimation(deathAnimation);
        // a fatal wound's blood, unless another effect was chosen
        if (hit.effect.empty())
            hit.effect = ActorLogic::FATAL_BLOOD;
        const auto tick = hit.tick;
        Components::Actors::Update(actor);
        // (the Hit dies with it, as every death is made: moving either moves both)
        actor = *Components::Actors::GetActor(actor.id);
        const auto clipId =
            ActorLogic::AddClip(actor, {.animation = deathAnimation, .start = tick, .blendIn = ActorLogic::DEATH_BLEND});
        ActorLogic::MakeDeath(actor, clipId, animation->duration);
        Components::Actors::Update(actor, true);
        Components::Actors::BlendCleanly(actor.id, clipId);
    }

    void ActorsMenu::RenderMovement(Types::Actor& actor)
    {
        const auto tick = Components::Playback::GetActorTick();
        auto& graphics = GFX::GraphicsManager::Get();
        const bool advanced = IsAdvanced(Page::Movement);
        const auto body = Components::Actors::GetBody(actor);

        // the way it moves: standing, or along a path; by keys or attached with Advanced (or when it moves so)
        using Movement = Types::ActorMovement;
        std::vector<RowButton> modes{
            {ICON_FA_PERSON, "Stand", "Stands where it is placed", false, actor.movement == Movement::Still,
             "actors.movement.still"},
            {ICON_FA_ROUTE, "Path", "Goes along a path: click the ground in the view for its points", false,
             actor.movement == Movement::Path, "actors.movement.path"}};
        const bool more = advanced || actor.movement == Movement::Keyframes || actor.movement == Movement::Attached;
        if (more)
        {
            modes.push_back({ICON_FA_DIAMOND, "Keys", "Moves between where it is keyed at times of the timeline", false,
                             actor.movement == Movement::Keyframes, "actors.movement.keys"});
            modes.push_back({ICON_FA_LINK, "Attached", "Moves with a bone of another actor or of a player", false,
                             actor.movement == Movement::Attached, "actors.movement.attached"});
        }
        const auto mode = ButtonRow("##actorMovement", modes);
        constexpr std::array MODES{Movement::Still, Movement::Path, Movement::Keyframes, Movement::Attached};
        if (mode && MODES[*mode] != actor.movement)
        {
            const auto placement = Components::Actors::EvaluatePlacement(actor, tick);
            // a path: from where it stands, its points clicked next (as Walk to...)
            if (MODES[*mode] == Movement::Path && !ActorActions::GetWalkUnavailableReason(actor.id, true))
            {
                Components::Actors::Update(actor);
                const auto gaits = Components::Actors::GetGaits(actor);
                ActorActions::WalkOn(actor.id, gaits.empty() ? std::nullopt : std::optional(Types::ActorGait::Walk));
                actor = *Components::Actors::GetActor(actor.id);
                return;
            }
            actor.movement = MODES[*mode];
            if (actor.movement == Movement::Keyframes && actor.keys.empty())
                actor.keys.push_back({tick, placement.position, placement.yaw});
            if (actor.movement == Movement::Path && actor.path.empty())
                actor.path.push_back(placement.position);
            if (actor.movement == Movement::Still)
            {
                actor.position = placement.position;
                actor.yaw = placement.yaw;
            }
        }

        if (actor.movement == Movement::Attached)
        {
            RenderAttachment(actor);
            if (advanced)
            {
                AdvancedRows rows;
                RenderAttachmentDetails(actor);
                RenderLocomotion(actor);
            }
            return;
        }

        // standing: a gait starts a path from where it stands, its points clicked next
        if (actor.movement == Movement::Still && body.kind != Types::ActorKind::Vehicle)
        {
            RenderGaits(actor, std::nullopt);
            if (actor.movement != Movement::Still)
                return;
        }

        if (actor.movement != Movement::Path)
        {
            const auto place =
                ButtonRow("##actorPlace",
                          {{ICON_FA_LOCATION_CROSSHAIRS, "To crosshair",
                            actor.movement == Movement::Keyframes
                                ? "Keys it on the ground where the camera looks, facing the camera, at the playhead"
                                : "Puts it on the ground where the camera looks, facing the camera"},
                           {ICON_FA_ARROW_DOWN, "Drop to ground", "Puts it on the ground below it"}});
            if (place)
            {
                const auto placement = Components::Actors::EvaluatePlacement(actor, tick);
                Components::Actors::Update(actor);
                if (place == 0)
                    PlaceInFrontOfCamera(actor);
                else
                    Components::Actors::RequestPlacement(actor.id, placement.position + glm::vec3(0, 0, 40),
                                                         glm::vec3(0, 0, -1));
                actor = *Components::Actors::GetActor(actor.id);
                return;
            }
        }

        if (actor.movement == Movement::Keyframes)
        {
            // where it stands at the playhead: changing it keys it there
            auto placement = Components::Actors::EvaluatePlacement(actor, tick);
            bool placementChanged = false;
            Label("Here at");
            ImGui::SetNextItemWidth(-FLT_MIN);
            placementChanged |= ImGui::DragFloat3("##actorPosition", &placement.position.x, 1.0f, 0.0f, 0.0f, "%.1f");
            Label("Facing");
            ImGui::SetNextItemWidth(-FLT_MIN);
            placementChanged |= ImGui::DragFloat("##actorYaw", &placement.yaw, 1.0f, 0.0f, 0.0f, "%.1f deg");
            if (placementChanged)
                Components::Actors::ApplyPlacement(actor, tick, placement.position,
                                                   std::remainder(placement.yaw, 360.0f));
            const auto key = std::ranges::find(actor.keys, tick, &Types::ActorKey::tick);
            const auto deleteTip = WithKey("Deletes the key at the playhead", "edit.delete");
            const auto keyButtons = ButtonRow(
                "##actorKeyButtons",
                {{ICON_FA_PLUS, "Key here", "Keys where it stands now at the playhead; moving it does too"},
                 {ICON_FA_TRASH, "Delete key", deleteTip.c_str(), key == actor.keys.end()}});
            if (keyButtons == 0)
            {
                const auto here = Components::Actors::EvaluatePlacement(actor, tick);
                Components::Actors::ApplyPlacement(actor, tick, here.position, here.yaw);
            }
            else if (keyButtons == 1)
                actor.keys.erase(key);
            const auto rows = std::min<std::size_t>(std::max<std::size_t>(actor.keys.size(), 1), 4);
            if (ImGui::BeginChild("##actorKeys", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * rows + 8), true))
            {
                for (const auto& entry : actor.keys)
                {
                    // a key is edited by being at its tick
                    if (ImGui::Selectable(std::format("{} Key at {}##key{}", entry.tick == tick ? ICON_FA_DIAMOND : " ",
                                                      FormatActorTime(entry.tick), entry.tick)
                                              .c_str(),
                                          entry.tick == tick))
                        JumpTo(entry.tick);
                    ImGui::SetItemTooltip("Tick %u; click to go there and change it", entry.tick);
                }
            }
            ImGui::EndChild();
        }

        if (actor.movement == Movement::Path)
            RenderPath(actor, body);
        else if (actor.movement == Movement::Keyframes && body.kind == Types::ActorKind::Vehicle)
            RenderLocomotion(actor);

        if (!advanced)
            return;
        AdvancedRows rows;
        if (actor.movement == Movement::Still)
        {
            // where it stands
            auto placement = Components::Actors::EvaluatePlacement(actor, tick);
            bool placementChanged = false;
            Label("Position");
            ImGui::SetNextItemWidth(-FLT_MIN);
            placementChanged |= ImGui::DragFloat3("##actorPosition", &placement.position.x, 1.0f, 0.0f, 0.0f, "%.1f");
            Label("Facing");
            ImGui::SetNextItemWidth(-FLT_MIN);
            placementChanged |= ImGui::DragFloat("##actorYaw", &placement.yaw, 1.0f, 0.0f, 0.0f, "%.1f deg");
            if (placementChanged)
                Components::Actors::ApplyPlacement(actor, tick, placement.position,
                                                   std::remainder(placement.yaw, 360.0f));
        }
        if (body.kind == Types::ActorKind::Human)
        {
            RenderMovementSet(actor);
            RenderStance(actor);
        }
        if (actor.movement == Movement::Path)
        {
            Label("");
            ImGui::Checkbox("Snap to ground##actorGround", &actor.followGround);
            ImGuiEx::Keyframeable::Record("actors.movement.ground");
            ImGui::SetItemTooltip("Keeps it on the ground below where it walks, between the points too");
        }
        // the gizmo (free and orbit cameras) moves along the actor's axes or the world's, or turns it
        Label("Gizmo");
        const auto gizmoMode = graphics.GetGizmoMode();
        const auto gizmo =
            ButtonRow("##actorGizmo", {{ICON_FA_MAXIMIZE, "Local", "Moves it along its own axes", false,
                                        gizmoMode == GFX::GizmoMode::TranslateLocal, "actors.movement.gizmo"},
                                       {ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "World", "Moves it along the world's axes",
                                        false, gizmoMode == GFX::GizmoMode::TranslateGlobal},
                                       {ICON_FA_ROTATE, "Rotate", "Turns it", actor.movement == Movement::Path,
                                        gizmoMode == GFX::GizmoMode::Rotate}});
        if (gizmo)
        {
            graphics.SetGizmoMode(*gizmo == 0   ? GFX::GizmoMode::TranslateLocal
                                  : *gizmo == 1 ? GFX::GizmoMode::TranslateGlobal
                                                : GFX::GizmoMode::Rotate);
            graphics.FocusActor();
        }
    }

    // An actor's path: how it goes (its gaits, or a killstreak's height and speed), where it goes and when, the point
    // selected in the view, and the pace with Advanced
    void ActorsMenu::RenderPath(Types::Actor& actor, const ActorLogic::Body& body)
    {
        auto& graphics = GFX::GraphicsManager::Get();
        auto selectedPoint = Components::Actors::GetSelectedWaypoint();
        if (selectedPoint && *selectedPoint >= actor.path.size())
            selectedPoint.reset();

        if (body.kind == Types::ActorKind::Vehicle)
            RenderVehicle(actor, body);
        else
            RenderGaits(actor, selectedPoint);

        // where it goes and when, and placing more points
        const auto timing = Components::Actors::GetPathTiming(actor);
        Label("Path");
        const bool placing = graphics.IsPlacingPathPoints();
        const auto addLabel = placing ? ICON_FA_CHECK " Done" : ICON_FA_PLUS " Points";
        const auto addWidth = ImGui::CalcTextSize(addLabel).x + ImGui::GetStyle().FramePadding.x * 2;
        {
            const auto length = Components::Actors::GetPathLength(actor);
            // (a time it hasn't been measured walking yet is a guess, but drawn the same; times always, or why not)
            auto when = timing.start && timing.arrival ? std::format(" · {:.2f} " ICON_FA_ARROW_RIGHT " {:.2f}",
                                                                     *timing.start / 1000.0f, *timing.arrival / 1000.0f)
                                                       : std::string();
            if (timing.start && !timing.arrival)
            {
                const auto cut = Components::Actors::GetCutAt(actor);
                when = cut ? std::format(" · {:.2f} " ICON_FA_ARROW_RIGHT " dies at {:.2f}", *timing.start / 1000.0f,
                                         *cut / 1000.0f)
                           : std::format(" · {:.2f} " ICON_FA_ARROW_RIGHT " short of its end", *timing.start / 1000.0f);
            }
            const auto summary = std::format("{} point{} · {:.0f} u{}", actor.path.size(),
                                             actor.path.size() == 1 ? "" : "s", length, when);
            const auto room = ImGui::GetContentRegionAvail().x - addWidth - ImGui::GetStyle().ItemSpacing.x;
            ImGui::AlignTextToFramePadding();
            ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                                ImVec2(ImGui::GetCursorScreenPos().x + room, ImGui::GetCursorScreenPos().y + 100),
                                true);
            ImGui::TextDisabled("%s", summary.c_str());
            ImGui::PopClipRect();
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - addWidth);
        }
        if (placing)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(addLabel, ImVec2(addWidth, 0)))
        {
            graphics.SetPlacingPathPoints(!placing);
            graphics.FocusActor();
        }
        if (placing)
            ImGui::PopStyleColor();
        ImGuiEx::Keyframeable::Record("actors.path.add");
        ImGui::SetItemTooltip(placing ? "Stops placing points (Enter, Escape, or a double-click; 1, 2, 3 pick the next "
                                        "stretch's gait)"
                                      : "Click the ground in the view to add points after the selected one");
        RenderNotes(actor);
        // (when it arrives is timed on the timeline: its path bar slides the walk, its end sets the pace)
        const auto gait = Components::Actors::GetGait(actor, 0);

        // the point selected in the view
        if (selectedPoint)
        {
            Label("");
            ImGui::TextDisabled("%s selected",
                                *selectedPoint == 0 ? "Start" : std::format("Point {}", *selectedPoint).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(actor.path.size() <= 1);
            if (ImGui::SmallButton(ICON_FA_TRASH "##deletePoint"))
            {
                ActorLogic::RemoveWaypoint(actor, *selectedPoint);
                Components::Actors::SelectWaypoint(std::nullopt);
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", WithKey("Deletes the point", "edit.delete").c_str());
        }

        if (!IsAdvanced(Page::Movement))
            return;
        AdvancedRows rows;
        if (gait)
        {
            Label("Pace");
            auto pace = gait->pace * 100.0f;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGuiEx::DragSlider("##actorPace", &pace, ActorLogic::MIN_PACE * 100.0f, ActorLogic::MAX_PACE * 100.0f,
                                    gait->fromBefore ? "%.0f%% (from before)" : "%.0f%%",
                                    {.resetValue = 100.0f, .flags = ImGuiSliderFlags_AlwaysClamp}))
                Components::Actors::SetPace(actor, pace / 100.0f);
            ImGuiEx::Keyframeable::Record("actors.path.pace");
            ImGui::SetItemTooltip(
                "How fast it goes against its gaits' own speeds: within 80%% to 120%% the feet "
                "keep to the ground");
        }
        if (selectedPoint)
        {
            Label("Point");
            const auto lookWidth = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - lookWidth - ImGui::GetStyle().ItemSpacing.x);
            ImGui::DragFloat3("##actorPoint", &actor.path[*selectedPoint].x, 1.0f, 0.0f, 0.0f, "%.1f");
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS "##actorPointLook", ImVec2(lookWidth, 0)))
            {
                Components::Actors::Update(actor);
                PlaceInFrontOfCamera(actor, *selectedPoint);
                actor = *Components::Actors::GetActor(actor.id);
            }
            ImGui::SetItemTooltip("Moves the point to the ground where the camera looks");
        }
    }

    // The gait buttons of a person or dog on a path: its body's walk, run and sprint (and
    // crouched ones), each underlined in its colour; for the stretch from the selected point, else the whole path. On
    // an actor standing still, a gait starts a path from where it stands, its points clicked next.
    namespace
    {
        // The gaits of an actor's body as buttons, a row standing and one crouched under it, each underlined in its
        // colour as the path and its bar on the timeline have it (faint where the path doesn't go at it: which gaits a
        // mixed path has); those active shown so. Each is recorded as <record>.<its name in lower case>; returns the
        // one clicked.
        std::optional<std::pair<Types::ActorGait, bool>> GaitButtons(
            const Types::Actor& actor, const char* id, const std::string& record,
            const std::function<bool(Types::ActorGait, bool)>& active, const char* where)
        {
            const auto gaits = Components::Actors::GetGaits(actor);
            const auto& style = ImGui::GetStyle();
            std::optional<std::pair<Types::ActorGait, bool>> chosen;
            for (const bool crouched : {false, true})
            {
                std::vector<const ActorLogic::SpeedPreset*> row;
                for (const auto& preset : gaits)
                    if ((preset.stance == Types::ActorStance::Crouch) == crouched)
                        row.push_back(&preset);
                if (row.empty())
                    continue;
                Label(crouched ? "" : "Gait");
                const auto count = static_cast<float>(row.size());
                const auto width = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * (count - 1)) / count;
                for (std::size_t i = 0; i < row.size(); ++i)
                {
                    const auto& preset = *row[i];
                    const auto gait = preset.gait == ActorLogic::Gait::Sprint ? Types::ActorGait::Sprint
                                      : preset.gait == ActorLogic::Gait::Run  ? Types::ActorGait::Run
                                                                              : Types::ActorGait::Walk;
                    const bool on = active(gait, crouched);
                    if (i > 0)
                        ImGui::SameLine();
                    if (on)
                        ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
                    if (ImGui::Button(std::format("{}##{}{}{}", preset.name, id, crouched, i).c_str(),
                                      ImVec2(width, 0)))
                        chosen = std::pair(gait, crouched);
                    if (on)
                        ImGui::PopStyleColor();
                    std::string name(preset.name);
                    std::erase(name, ' ');
                    std::ranges::transform(name, name.begin(),
                                           [](char c) { return static_cast<char>(std::tolower(c)); });
                    ImGuiEx::Keyframeable::Record(record + "." + name);
                    bool used = actor.stretches.empty();
                    for (std::size_t s = 0; s + 1 < actor.path.size() && s < actor.stretches.size(); ++s)
                        used = used || (actor.stretches[s].gait == gait && actor.stretches[s].crouched == crouched);
                    auto color = GetGaitColor(gait, crouched);
                    if (!used)
                        color.w = 0.25f;
                    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x, max.y - 3.0f), max,
                                                              ImGui::GetColorU32(color));
                    ImGui::SetItemTooltip("%.0f units a second%s", preset.speed, where);
                }
            }
            return chosen;
        }
    }  // namespace

    void ActorsMenu::RenderGaits(Types::Actor& actor, std::optional<std::size_t> point)
    {
        if (Components::Actors::GetGaits(actor).empty())
            return;
        const auto onStretch = point && *point + 1 < actor.path.size() ? point : std::nullopt;
        const auto state = Components::Actors::GetGait(actor, onStretch.value_or(0));
        // (along the whole path: none shown as picked while its stretches go at different gaits)
        bool mixed = false;
        if (!onStretch)
            for (std::size_t i = 1; i + 1 < actor.path.size() && i < actor.stretches.size(); ++i)
                mixed = mixed || actor.stretches[i].gait != actor.stretches[0].gait ||
                        actor.stretches[i].crouched != actor.stretches[0].crouched;
        const auto chosen = GaitButtons(
            actor, "gait", "actors.gait",
            [&](Types::ActorGait gait, bool crouched) {
                return !mixed && state && state->gait == gait && state->crouched == crouched;
            },
            onStretch ? ", from the selected point to the next" : ", along the whole path");
        // what it applies to
        if (actor.movement == Types::ActorMovement::Path && actor.path.size() >= 2)
        {
            // (scrolled to, flashing a moment, when a point was just picked)
            const auto age = std::chrono::duration<float>(std::chrono::steady_clock::now() - pointShownAt).count();
            if (age < 0.6f)
            {
                const auto min = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(min.x - 4.0f, min.y), ImVec2(min.x + ImGui::GetContentRegionAvail().x, min.y + ImGui::GetFrameHeight()),
                    ImGui::GetColorU32(ImGuiCol_HeaderActive, 1.0f - age / 0.6f));
            }
            if (scrollToPoint)
            {
                ImGui::SetScrollHereY(0.25f);
                scrollToPoint = false;
            }
            Label("");
            if (onStretch)
                ImGui::TextDisabled("Stretch %zu: %s " ICON_FA_ARROW_RIGHT " point %zu", *onStretch + 1,
                                    *onStretch == 0 ? "start" : std::format("point {}", *onStretch).c_str(),
                                    *onStretch + 1);
            else if (mixed)
                ImGui::TextDisabled("Mixed: a gait clicked goes along the whole path");
            else
                ImGui::TextDisabled("Whole path");
            // (its looks are changed on the walk's pieces, in the Animation section)
        }
        if (!chosen)
            return;
        // (a path from before gaits walks at them from now on)
        actor.locomotion = Types::ActorLocomotion::Auto;
        if (actor.movement != Types::ActorMovement::Path || actor.path.size() < 2)
        {
            Components::Actors::Update(actor);
            ActorActions::WalkOn(actor.id, chosen->first, chosen->second);
            actor = *Components::Actors::GetActor(actor.id);
            return;
        }
        if (onStretch)
            Components::Actors::SetGait(actor, *onStretch, *onStretch + 1, chosen->first, chosen->second);
        else
            Components::Actors::SetGait(actor, 0, actor.path.size(), chosen->first, chosen->second);
    }

    // The look of a walk's piece (proposals 1a and 5, 11.4 F2): the animation its stretches
    // play, < > to the one before or after it among those like it, and their list: favourites, those tried on this
    // path, those like it, each shown on the actor as the mouse is over it (Up/Down step through them, Enter keeps one,
    // Escape goes back), for the piece's stretches or the whole path as Apply to says (the list has it at its top), and
    // A/B between the last two tried
    void ActorsMenu::RenderLookRow(Types::Actor& actor, const ActorLogic::WalkPiece& piece)
    {
        constexpr auto LOOK_POPUP = "##lookList";
        if (actor.stretches.empty())
            return;
        const auto index = std::min(piece.first, actor.stretches.size() - 1);
        const auto stretch = actor.stretches[index];
        const auto candidates = Components::Actors::GetLookCandidates(actor, stretch.gait, stretch.crouched);
        const auto tried = Components::Actors::GetTriedLooks(actor.id);
        const bool own = !stretch.look.empty();
        // the look set on the piece's stretches, or the whole path's
        const auto setLook = [&](Types::Actor& on, const std::string& name, bool said) {
            if (applyToPath)
                Components::Actors::SetLook(on, std::nullopt, name, said);
            else
                for (auto i = piece.first; i <= piece.last && i < on.stretches.size(); ++i)
                    Components::Actors::SetLook(on, i, name, said);
        };
        // (as the game measured it, else as the catalog of every animation has it)
        const auto speedOf = [&](const std::string& name) {
            if (const auto measured = Components::Actors::GetAnimationMotion(name))
                if (const auto* entry = Components::Actors::GetAnimationCatalog().Find(name))
                    return glm::length(*measured) / std::max(entry->Duration(), 0.01f);
            const auto* entry = Components::Actors::GetAnimationCatalog().Find(name);
            return entry ? glm::length(glm::vec2(entry->motion)) / std::max(entry->Duration(), 0.01f)
                         : ActorLogic::GetAnimationSpeed(name, Components::Actors::GetAnimationLookup());
        };
        // a look picked: kept (from as it was before any preview), and tried
        const auto keep = [&](const std::string& name) {
            if (lookPreviewWas)
            {
                Components::Actors::Preview(*lookPreviewWas);
                actor = *lookPreviewWas;
                lookPreviewWas.reset();
            }
            Components::Actors::SetPreviewLoop(std::nullopt);
            setLook(actor, name, true);
            Components::Actors::AddTriedLook(actor.id, name);
            Components::Actors::Update(actor, false, "look " + name);
        };
        // shown on the actor at once, going through the piece again and again (the whole walk for the path's look)
        const auto preview = [&](const std::string& name) {
            if (!lookPreviewWas)
                lookPreviewWas = actor;
            auto shown = *lookPreviewWas;
            setLook(shown, name, false);
            Components::Actors::FitOnlyWalk(shown);
            Components::Actors::Preview(shown);
            const auto timing = ActorLogic::GetPathTiming(shown, Components::Actors::GetAnimationLookup());
            const auto last = std::min(piece.last + 1, timing.reached.size() - 1);
            const auto from = !applyToPath ? timing.left[piece.first] : timing.start;
            const auto to = !applyToPath ? timing.reached[last] : timing.arrival;
            Components::Actors::SetPreviewLoop(
                from && to ? std::optional(Components::Actors::PreviewLoop{actor.id, *from, *to}) : std::nullopt);
        };

        // Browse... in the list: any animation of the game as its look, from the browser (as the walk clip's Animation
        // button had it)
        constexpr auto LOOK_BROWSER = "##lookBrowser";
        if (openLookBrowser)
        {
            openLookBrowser = false;
            browserLikeThis = stretch.animation;
            lookBrowsed = stretch.animation;
            ImGui::OpenPopup(LOOK_BROWSER);
        }
        if (const auto catalog = Components::Actors::GetCatalog();
            catalog &&
            AnimationBrowser(LOOK_BROWSER, *catalog, actor.models, lookBrowsed, animationFilter, animationCategory))
        {
            keep(lookBrowsed);
            Components::Actors::AddRecentAnimation(lookBrowsed);
        }

        Label("Animation");
        const ImVec2 arrow(ImGui::GetFrameHeight(), 0);
        const auto at = std::ranges::find(candidates, stretch.animation);
        const auto step = [&](int by) {
            if (candidates.empty())
                return;
            const auto from = at != candidates.end() ? static_cast<int>(at - candidates.begin()) : -by;
            const auto count = static_cast<int>(candidates.size());
            keep(candidates[static_cast<std::size_t>(((from + by) % count + count) % count)]);
        };
        if (ImGui::Button(ICON_FA_CHEVRON_LEFT "##lookPrev", arrow))
            step(-1);
        ImGuiEx::Keyframeable::Record("actors.look.prev");
        ImGui::SetItemTooltip("The look before it among those like it");
        ImGui::SameLine(0, 2);
        const auto width = ImGui::GetContentRegionAvail().x - arrow.x - 2;
        const auto label = std::format("{}  " ICON_FA_CARET_DOWN "##lookName", ShownName(stretch.animation));
        if (ImGui::Button(label.c_str(), ImVec2(width, 0)))
        {
            lookRow = -1;
            ImGui::OpenPopup(LOOK_POPUP);
        }
        ImGuiEx::Keyframeable::Record("actors.look.list");
        ImGui::SetItemTooltip("What its %s plays%s, %.0f u/s: click for others like it",
                              ActorActions::GetGaitName(actor, stretch.gait, stretch.crouched).c_str(),
                              own ? " (its own look)" : "", speedOf(stretch.animation));
        ImGui::SameLine(0, 2);
        if (ImGui::Button(ICON_FA_CHEVRON_RIGHT "##lookNext", arrow))
            step(1);
        ImGuiEx::Keyframeable::Record("actors.look.next");
        ImGui::SetItemTooltip("The look after it among those like it");

        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 21, 0));
        if (!ImGui::BeginPopup(LOOK_POPUP))
        {
            // closed without one kept: back as it was
            if (lookPreviewWas)
            {
                Components::Actors::Preview(*lookPreviewWas);
                Components::Actors::SetPreviewLoop(std::nullopt);
                actor = *lookPreviewWas;
                lookPreviewWas.reset();
            }
            return;
        }
        // where a look picked goes, as on the piece
        RenderApplyTo("lookScope", "actors.look.scope");
        ImGui::TextDisabled("%s looks for this body",
                            ActorActions::GetGaitName(actor, stretch.gait, stretch.crouched).c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Up/Down, Enter").x);
        ImGui::TextDisabled("Up/Down, Enter");
        // the rows: favourites, tried, like this (each once)
        struct Row
        {
            std::string name;
            const char* section;
        };
        std::vector<Row> rows;
        const auto add = [&](const std::string& name, const char* section) {
            if (std::ranges::none_of(rows, [&](const Row& row) { return row.name == name; }))
                rows.push_back({name, section});
        };
        for (const auto& name : tried)
            if (Components::Actors::IsFavoriteAnimation(name))
                add(name, ICON_FA_STAR " Favourites");
        for (const auto& name : candidates)
            if (Components::Actors::IsFavoriteAnimation(name))
                add(name, ICON_FA_STAR " Favourites");
        for (const auto& name : tried)
            add(name, "Tried on this path");
        for (const auto& name : candidates)
            add(name, "Like this, by speed");
        // stepping with the keys
        if (!rows.empty())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            {
                lookRow = std::min(lookRow + 1, static_cast<int>(rows.size()) - 1);
                preview(rows[static_cast<std::size_t>(lookRow)].name);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            {
                lookRow = std::max(lookRow - 1, 0);
                preview(rows[static_cast<std::size_t>(lookRow)].name);
            }
            if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && lookRow >= 0)
            {
                keep(rows[static_cast<std::size_t>(lookRow)].name);
                ImGui::CloseCurrentPopup();
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        // the one shown: previewed, else the stretch's
        const auto shownLook =
            lookPreviewWas ? std::string(Components::Actors::GetActor(actor.id)
                                             ? Components::Actors::GetActor(actor.id)->stretches[index].animation
                                             : stretch.animation)
                           : stretch.animation;
        const char* section = nullptr;
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            if (row.section != section)
            {
                section = row.section;
                ImGui::SeparatorText(section);
            }
            ImGui::PushID(static_cast<int>(i));
            const bool favorite = Components::Actors::IsFavoriteAnimation(row.name);
            ImGui::PushStyleColor(ImGuiCol_Text, favorite ? Theme::Favorite()
                                                          : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::SmallButton(ICON_FA_STAR))
                Components::Actors::SetFavoriteAnimation(row.name, !favorite);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            const bool current = row.name == stretch.animation && !lookPreviewWas;
            if (ImGui::Selectable(std::format("{}{}##row", ShownName(row.name),
                                              row.name == (lookPreviewWas ? lookPreviewWas->stretches[index].animation
                                                                          : stretch.animation)
                                                  ? "  " ICON_FA_CHECK
                                                  : "")
                                      .c_str(),
                                  current || row.name == shownLook || lookRow == static_cast<int>(i)))
            {
                keep(row.name);
                ImGui::CloseCurrentPopup();
            }
            ImGuiEx::Keyframeable::Record("actors.look.row." + row.name);
            if (ImGui::IsItemHovered() && lookRow != static_cast<int>(i))
            {
                lookRow = static_cast<int>(i);
                preview(row.name);
            }
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFontSize() * 4.0f);
            ImGui::TextDisabled("%.0f u/s", speedOf(row.name));
            ImGui::PopID();
        }
        if (rows.empty())
            ImGui::TextDisabled("None like it fits this body");
        ImGui::Separator();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN " Browse...##lookBrowse"))
        {
            openLookBrowser = true;
            ImGui::CloseCurrentPopup();
        }
        ImGuiEx::Keyframeable::Record("actors.look.browse");
        ImGui::SetItemTooltip("Any animation of the game as its look");
        // A/B: the other of the last two tried
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFontSize() * 3.2f);
        ImGui::BeginDisabled(tried.size() < 2);
        if (ImGui::Button("A/B##lookAB", ImVec2(ImGui::GetFontSize() * 3.2f, 0)))
        {
            keep(tried.size() >= 2 && tried[0] == stretch.animation ? tried[1] : tried.front());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGuiEx::Keyframeable::Record("actors.look.ab");
        if (tried.size() >= 2)
            ImGui::SetItemTooltip("Back to %s (and again to %s)", ShownName(tried[1]).c_str(),
                                  ShownName(tried[0]).c_str());
        ImGui::EndPopup();
    }

    // Apply to: This stretch or Whole path, the one picked in the editor's blue so
    // it reads at a glance, recorded as <record>.stretch and <record>.path
    void ActorsMenu::RenderApplyTo(const char* id, const char* record)
    {
        const auto& style = ImGui::GetStyle();
        const auto width = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2;
        const auto blue = style.Colors[ImGuiCol_CheckMark];
        for (const bool path : {false, true})
        {
            if (path)
                ImGui::SameLine();
            const bool picked = applyToPath == path;
            if (picked)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(blue.x * 0.7f, blue.y * 0.7f, blue.z * 0.7f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, blue);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            }
            if (ImGui::Button(std::format("{}##{}{}", path ? "Whole path" : "This stretch", id, path).c_str(),
                              ImVec2(width, 0)))
                applyToPath = path;
            if (picked)
                ImGui::PopStyleColor(3);
            ImGuiEx::Keyframeable::Record(std::format("{}.{}", record, path ? "path" : "stretch"));
            ImGui::SetItemTooltip("%s", path ? "Its animation, gait and speed change along the whole path"
                                             : "Its animation, gait and speed change on this stretch only");
        }
    }

    void ActorsMenu::RenderWalkPiece(Types::Actor& actor, std::uint32_t clipId, const ActorLogic::WalkPiece& piece)
    {
        // (This stretch again each time another piece is shown)
        const auto key = std::tuple(actor.id, clipId, piece.first);
        if (shownPiece != key)
        {
            shownPiece = key;
            applyToPath = false;
        }
        pieceScope = PieceScope{actor.id, piece.first, piece.last, applyToPath};
        const auto first = piece.first, last = std::min(piece.last, actor.stretches.size() - 1);
        const auto timing = Components::Actors::GetPathTiming(actor);
        const auto animations = Components::Actors::GetAnimationLookup();

        // which piece: its stretches, between which points
        ImGui::TextDisabled("%s", first == last ? std::format("Walk along the path "
                                                              "\xc2\xb7"
                                                              " Stretch {}: {} " ICON_FA_ARROW_RIGHT " point {}",
                                                              first + 1, PointName(first), first + 1)
                                                      .c_str()
                                                : std::format("Walk along the path "
                                                              "\xc2\xb7"
                                                              " Stretches {}-{}: {} " ICON_FA_ARROW_RIGHT " point {}",
                                                              first + 1, last + 1, PointName(first), last + 1)
                                                      .c_str());
        Label("Apply to");
        RenderApplyTo("pieceScope", "actors.piece.scope");

        // what it plays (the look list)
        RenderLookRow(actor, piece);

        // its gait: of its stretches, or of the whole path
        const auto chosen = GaitButtons(
            actor, "pieceGait", "actors.piece.gait",
            [&](Types::ActorGait gait, bool crouched) {
                const auto from = applyToPath ? 0 : first, to = applyToPath ? actor.stretches.size() - 1 : last;
                for (auto i = from; i <= to && i < actor.stretches.size(); ++i)
                    if (i + 1 < actor.path.size() &&
                        (actor.stretches[i].gait != gait || actor.stretches[i].crouched != crouched))
                        return false;
                return true;
            },
            applyToPath ? ", along the whole path" : ", on this stretch");
        if (chosen)
        {
            if (applyToPath)
                Components::Actors::SetGait(actor, 0, actor.path.size(), chosen->first, chosen->second);
            else
                Components::Actors::SetGait(actor, first, last + 1, chosen->first, chosen->second);
        }

        // its speed: its stretches' own pace, or the path's
        Label("Speed");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (applyToPath)
        {
            auto pace = actor.pace * 100.0f;
            if (ImGuiEx::DragSlider("##pieceSpeed", &pace, ActorLogic::MIN_PACE * 100.0f, ActorLogic::MAX_PACE * 100.0f,
                                    "Path %.0f%%", {.resetValue = 100.0f, .flags = ImGuiSliderFlags_AlwaysClamp}))
                Components::Actors::SetPace(actor, pace / 100.0f);
        }
        else
        {
            const auto speed = ActorLogic::GetStretchSpeed(actor, actor.stretches[first], animations);
            auto pace = actor.stretches[first].pace * 100.0f;
            if (ImGuiEx::DragSlider("##pieceSpeed", &pace, ActorLogic::MIN_PACE * 100.0f, ActorLogic::MAX_PACE * 100.0f,
                                    std::format("{:.0f} u/s "
                                                "\xc2\xb7"
                                                " %.0f%%",
                                                speed)
                                        .c_str(),
                                    {.resetValue = 100.0f, .flags = ImGuiSliderFlags_AlwaysClamp}))
                for (auto i = first; i <= last; ++i)
                    actor.stretches[i].pace = pace / 100.0f;
        }
        ImGuiEx::Keyframeable::Record("actors.piece.speed");
        ImGui::SetItemTooltip("%s", applyToPath ? "How fast the whole path goes against its gaits' own speeds: within "
                                                  "80%% to 120%% the feet keep to the ground"
                                                : "How fast this stretch goes against its look's own speed, on top of "
                                                  "the path's: within 80%% to 120%% the feet keep to the ground");

        // its blend: of its join (a piece after a turn or a wait: how it takes over from those), else the walk's own
        // from the clip before it (found again: the look list puts the actor back as it was when a preview ends)
        const auto clipAt = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id);
        if (clipAt == actor.clips.end() || first >= actor.stretches.size())
            return;
        auto& clip = *clipAt;
        auto& stretch = actor.stretches[first];
        const auto clipIndex = static_cast<std::size_t>(clipAt - actor.clips.begin());
        const bool takesOver = !piece.joined && clip.chain == Types::ActorChain::Walk;
        const auto blend = piece.joined ? ActorLogic::GetJoinBlend(actor, first, animations) : clip.blendIn;
        std::optional<float> distance;
        if (piece.joined)
            distance = Components::Actors::GetJoinDistance(actor, first);
        else if (clipIndex > 0)
        {
            const auto poses = Components::PoseSignatures::GetLookup(actor);
            const auto before = poses(actor.clips[clipIndex - 1].animation), now = poses(clip.animation);
            if (before && now)
                distance = TransitionLogic::GetPoseDistance(before->last, now->first);
        }
        {
            Label("Blend");
            const auto& style = ImGui::GetStyle();
            const auto autoWidth = ImGui::CalcTextSize("Auto").x + style.FramePadding.x * 2;
            const bool smooth = distance && TransitionLogic::IsCleanJoin(*distance, blend);
            const auto join = !distance ? "" : smooth ? "Smooth" : ICON_FA_TRIANGLE_EXCLAMATION " Pops";
            const auto joinWidth = distance ? ImGui::CalcTextSize(join).x + style.ItemSpacing.x : 0.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - autoWidth - joinWidth - style.ItemSpacing.x);
            float seconds = blend / 1000.0f;
            if (ImGui::DragFloat("##pieceBlend", &seconds, 0.01f, 0.05f, 1.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp))
            {
                const auto ticks = static_cast<std::uint32_t>(std::lround(seconds * 1000.0f));
                if (piece.joined || takesOver)
                {
                    ActorLogic::UpdateJoin(stretch);
                    stretch.blend = ticks;
                }
                if (!piece.joined)
                    clip.blendIn = ticks;
            }
            ImGuiEx::Keyframeable::Record("actors.piece.blend");
            ImGui::SetItemTooltip("%s", piece.joined ? "How long the legs and the speed take to go over from the look "
                                                       "before; drag the ramp's edges on the timeline too"
                                                     : "How long the walk takes over from the clip before it");
            ImGui::SameLine();
            const bool autoBlend = piece.joined || takesOver ? !stretch.blend && !stretch.joinAsBefore : false;
            if (autoBlend)
                ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
            if (ImGui::Button("Auto##pieceBlendAuto", ImVec2(autoWidth, 0)))
            {
                if (piece.joined || takesOver)
                {
                    ActorLogic::UpdateJoin(stretch);
                    stretch.blend.reset();
                }
                // (the walk's own: the blend that joins it cleanly, or once their poses are worked out)
                else if (const auto clean =
                             clip.start > 0 ? Components::Actors::GetCleanBlend(actor, clip.start - 1, clip.animation)
                                            : std::nullopt)
                    clip.blendIn = *clean;
                else
                    Components::Actors::BlendCleanly(actor.id, clip.id);
            }
            if (autoBlend)
                ImGui::PopStyleColor();
            ImGuiEx::Keyframeable::Record("actors.piece.blend.auto");
            ImGui::SetItemTooltip("%s", piece.joined ? "One step of the slower look"
                                                     : "As it is planned, or as joins "
                                                       "it cleanly");
            if (distance)
            {
                ImGui::SameLine();
                if (smooth)
                    ImGui::TextDisabled("%s", join);
                else
                    ImGui::TextColored(MisfitColor(), "%s", join);
                ImGuiEx::Keyframeable::Record("actors.piece.join");
                ImGui::SetItemTooltip(
                    "The poses are %.1f units apart as the blend starts (hips, spine, head, knees, "
                    "ankles and wrists), over a blend of %u ms",
                    *distance, blend);
            }
        }
        const auto at = stretch.joinAsBefore ? Types::ActorJoinAt::After : stretch.blendAt;
        if (piece.joined && first > 0)
        {
            Label("");
            ImGui::TextDisabled("from %s, %s %s", ShownName(actor.stretches[first - 1].animation).c_str(),
                                at == Types::ActorJoinAt::Centred  ? "centred on"
                                : at == Types::ActorJoinAt::Before ? "ending at"
                                                                   : "starting at",
                                PointName(first).c_str());
        }

        // where its join sits and its feet
        if (piece.joined && IsAdvanced(Page::Animation))
        {
            AdvancedRows rows;
            Label(std::format("At {}", PointName(first)).c_str());
            const auto& style = ImGui::GetStyle();
            const auto width = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 2) / 3;
            constexpr std::array PLACES{std::tuple(Types::ActorJoinAt::Before, "Before", "actors.piece.at.before",
                                                   "The blend ends at the point"),
                                        std::tuple(Types::ActorJoinAt::Centred, "Centred", "actors.piece.at.centred",
                                                   "Half the blend before the point, half after"),
                                        std::tuple(Types::ActorJoinAt::After, "After", "actors.piece.at.after",
                                                   "The blend starts at the point")};
            for (std::size_t i = 0; i < PLACES.size(); ++i)
            {
                const auto& [place, name, record, tooltip] = PLACES[i];
                if (i > 0)
                    ImGui::SameLine();
                if (place == at)
                    ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
                if (ImGui::Button(std::format("{}##pieceAt", name).c_str(), ImVec2(width, 0)))
                {
                    ActorLogic::UpdateJoin(stretch);
                    stretch.blendAt = place;
                }
                if (place == at)
                    ImGui::PopStyleColor();
                ImGuiEx::Keyframeable::Record(record);
                ImGui::SetItemTooltip("%s", tooltip);
            }
            Label("Feet");
            bool feet = stretch.joinAsBefore ? false : stretch.matchFeet;
            if (ImGui::Checkbox("Match the steps##pieceFeet", &feet))
            {
                ActorLogic::UpdateJoin(stretch);
                stretch.matchFeet = feet;
            }
            ImGuiEx::Keyframeable::Record("actors.piece.feet");
            ImGui::SetItemTooltip("The look comes in on the foot after the last one the look before came down on");
        }

        // when it gets there
        const auto from = first == 0 ? timing.start : timing.reached[first];
        const auto to = last + 1 < timing.reached.size() ? timing.reached[last + 1] : std::nullopt;
        if (from)
        {
            Label("");
            ImGui::TextDisabled("%s",
                                std::format("{} at {:.2f} s{}", first == 0 ? "Sets off" : "Reaches " + PointName(first),
                                            *from / 1000.0f,
                                            to && *to > *from ? std::format(" "
                                                                            "\xc2\xb7"
                                                                            " {:.2f} s to point {}",
                                                                            (*to - *from) / 1000.0f, last + 1)
                                                              : std::string())
                                    .c_str());
        }
    }

    // What re-plans did that wasn't asked for, said once with the other choice
    void ActorsMenu::RenderNotes(Types::Actor& actor)
    {
        const auto notes = Components::Actors::GetNotes(actor.id);
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            const auto& note = notes[i];
            ImGui::PushID(static_cast<int>(i));
            Label("");
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Note());
            ImGui::TextUnformatted(ICON_FA_CIRCLE_INFO);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(note.text.c_str());
            ImGui::PopTextWrapPos();
            Label("");
            if (note.keepArrival)
            {
                if (ImGui::SmallButton("Keep time"))
                {
                    Components::Actors::ArriveAt(actor, *note.keepArrival);
                    Components::Actors::DismissNotes(actor.id);
                }
                ImGuiEx::Keyframeable::Record("actors.note.keeptime");
                ImGui::SetItemTooltip("Arrives at %.2f s as before: its pace follows", *note.keepArrival / 1000.0f);
                ImGui::SameLine();
            }
            if (note.letMovePace)
            {
                if (ImGui::SmallButton("Let it move"))
                {
                    Components::Actors::SetPace(actor, *note.letMovePace);
                    Components::Actors::FitOnlyWalk(actor);
                    Components::Actors::DismissNotes(actor.id);
                }
                ImGuiEx::Keyframeable::Record("actors.note.letmove");
                ImGui::SetItemTooltip("Back to the pace it had (%.0f%%): it arrives when that takes it there",
                                      *note.letMovePace * 100.0f);
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Undo"))
            {
                Commands::Run("edit.undo");
                Components::Actors::DismissNotes(actor.id);
            }
            ImGuiEx::Keyframeable::Record("actors.note.undo");
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_XMARK))
                Components::Actors::DismissNotes(actor.id);
            ImGui::PopID();
        }
    }

    // How the actor's legs stand and walk: its stance (under upper body clips, and walking), whether its locomotion
    // walks and runs it as the game's players (see ActorLogic::GetLayers) and, on a path, how fast its walking clips
    // walk it
    void ActorsMenu::RenderLocomotion(Types::Actor& actor)
    {
        // (a killstreak goes as the game moves it)
        const auto body = Components::Actors::GetBody(actor);
        if (body.kind == Types::ActorKind::Vehicle)
        {
            RenderVehicle(actor, body);
            return;
        }
        if (body.kind == Types::ActorKind::Dog)
            return;
        RenderMovementSet(actor);
        RenderStance(actor);
    }

    // A killstreak's height (a flying one's) and, on a path, its speed, with its scripts' speeds as presets:
    // its moving clips go at it, with Locomotion Auto, which a killstreak always has
    void ActorsMenu::RenderVehicle(Types::Actor& actor, const ActorLogic::Body& body)
    {
        const auto& info = Components::KillstreakLogic::Get(body.killstreak);
        if (Components::KillstreakLogic::Flies(info.vehicleClass))
        {
            // the actor and its path up or down together, and the points placed after it as high
            Label("Height");
            auto height = actor.flyHeight;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##actorFlyHeight", &height, 2.0f, 0.0f, 20000.0f, "%.0f u",
                                 ImGuiSliderFlags_AlwaysClamp))
            {
                const auto lift = height - actor.flyHeight;
                actor.flyHeight = height;
                actor.position.z += lift;
                for (auto& point : actor.path)
                    point.z += lift;
                for (auto& key : actor.keys)
                    key.position.z += lift;
            }
            ImGuiEx::Keyframeable::Record("actors.flyheight");
            ImGui::SetItemTooltip(
                "How high above the ground it flies: the whole path goes up or down, and the points clicked after it "
                "are put as high (the gizmo moves a point on its own). The game's: %.0f",
                info.flyHeight);
        }
        if (actor.locomotion != Types::ActorLocomotion::Auto)
        {
            actor.locomotion = Types::ActorLocomotion::Auto;
            Components::Actors::FitOnlyWalk(actor);
        }
        if (actor.movement != Types::ActorMovement::Path || info.presets.empty())
            return;
        Label("Speed");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto most = std::max(1000.0f, info.presets.back().speed * 1.5f);
        ImGui::DragFloat("##actorPathSpeed", &actor.pathSpeed, std::max(1.0f, most / 1000.0f), 0.0f, most,
                         "%.0f units/s", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit())
            Components::Actors::FitOnlyWalk(actor);
        ImGuiEx::Keyframeable::Record("actors.pathspeed");
        ImGui::SetItemTooltip(
            "How fast its clips that drive or fly take it along the path (a clip's own speed, in its Advanced fold, "
            "goes first); it gets up to speed and stops at the game's rate, as the clips blend. A drive or flight to "
            "the path's end is made as long as it takes then");
        std::vector<std::string> tips, records;
        for (const auto& preset : info.presets)
        {
            tips.push_back(std::format("{:.0f} units/s ({:.0f} mph), as the game's", preset.speed, preset.speed / 17.6f));
            std::string name;
            for (const auto c : preset.name)
                if (c != ' ')
                    name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            records.push_back("actors.speed." + name);
        }
        const auto icon = info.vehicleClass == Types::VehicleClass::Car          ? ICON_FA_CAR_SIDE
                          : info.vehicleClass == Types::VehicleClass::Helicopter ? ICON_FA_HELICOPTER
                                                                                 : ICON_FA_PLANE;
        std::vector<RowButton> buttons;
        for (std::size_t i = 0; i < info.presets.size(); ++i)
            buttons.push_back({i + 1 == info.presets.size() && i > 0 ? ICON_FA_FORWARD_FAST : icon,
                               info.presets[i].name.data(), tips[i].c_str(), false,
                               std::abs(actor.pathSpeed - info.presets[i].speed) < 0.5f, records[i].c_str()});
        Label("");
        if (const auto chosen = ButtonRow("##actorVehicleSpeeds", buttons))
        {
            actor.pathSpeed = info.presets[*chosen].speed;
            Components::Actors::FitOnlyWalk(actor);
        }
    }

    // Whose walks, runs, turns, starts and stops a person goes with
    void ActorsMenu::RenderMovementSet(Types::Actor& actor)
    {
        static constexpr std::array NAMES{"Auto", "Soldier", "Relaxed AI", "Close-quarters AI", "Run-and-gun AI"};
        static constexpr std::array RECORDS{"auto", "players", "lowready", "cqb", "runandgun"};
        const auto resolved = Components::Actors::GetMovementSet(actor);
        const auto preview =
            actor.movementSet == Types::ActorMovementSet::Auto
                ? std::format("{} ({})", NAMES[0], NAMES[static_cast<std::size_t>(resolved)])
                : std::string(NAMES[static_cast<std::size_t>(actor.movementSet)]);
        Label("Style");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool open = ImGui::BeginCombo("##actorMovementSet", preview.c_str());
        ImGuiEx::Keyframeable::Record("actors.movementset");
        ImGui::SetItemTooltip(
            "How it moves, starts, stops and turns: as the game's players (Soldier: no starts or stops, turns on the "
            "spot) or one of the campaign AI's styles. Auto: the AI's on a campaign body, a soldier's on another. "
            "It never changes the gait");
        if (!open)
            return;
        for (std::size_t i = 0; i < NAMES.size(); ++i)
        {
            if (ImGui::Selectable(NAMES[i], static_cast<std::size_t>(actor.movementSet) == i))
                Components::Actors::SetMovementSet(actor, static_cast<Types::ActorMovementSet>(i));
            ImGuiEx::Keyframeable::Record(std::string("actors.movementset.") + RECORDS[i]);
        }
        ImGui::EndCombo();
    }

    void ActorsMenu::RenderStance(Types::Actor& actor)
    {
        Label("Stance");
        static constexpr const char* STANCES[] = {"Auto", "Standing", "Crouching", "Prone"};
        auto stance = static_cast<int>(actor.stance);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##actorStance", &stance, STANCES, IM_ARRAYSIZE(STANCES)))
            actor.stance = static_cast<Types::ActorStance>(stance);
        ImGuiEx::Keyframeable::Record("actors.stance");
        ImGui::SetItemTooltip(
            "How the legs stand under clips that move only the upper body (reloads, shots) and how they go along its "
            "path. Auto: as the clip and the gait say");
    }

    // Whether its locomotion walks and runs it as the game's players (or dogs), and on a path how fast its walking
    // clips walk it, with presets of the speeds of its body's animations
    void ActorsMenu::RenderSpeed(Types::Actor& actor, const ActorLogic::Body& body)
    {
        const bool dog = body.kind == Types::ActorKind::Dog;
        Label("Locomotion");
        bool automatic = actor.locomotion == Types::ActorLocomotion::Auto;
        // (a walk to the path's end still gets there, at the speed it walks now)
        if (ImGui::Checkbox("Walks and runs by its speed##actorLocomotion", &automatic))
        {
            actor.locomotion = automatic ? Types::ActorLocomotion::Auto : Types::ActorLocomotion::Off;
            Components::Actors::FitOnlyWalk(actor);
        }
        ImGuiEx::Keyframeable::Record("actors.locomotion");
        ImGui::SetItemTooltip(dog ? "Its clips of the dogs' walks, runs and idles play its own breed's for the "
                                    "actor's speed, as fast as the speed asks. Off, each clip plays its own animation "
                                    "at its own speed"
                                  : "Its clips of walks, runs and idles play the one of its movement set the game would "
                                    "for the actor's speed, stance and the way it goes, with the gun held, and as fast as "
                                    "the speed asks; clips that move only the upper body play over it and walk on with "
                                    "it. Off, each clip plays its own animation at its own speed");
        if (!automatic)
            return;

        Label("Speed");
        if (actor.movement == Types::ActorMovement::Keyframes)
        {
            ImGui::TextDisabled("As fast as it goes between its keys");
            return;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##actorPathSpeed", &actor.pathSpeed, 1.0f, 0.0f, 1000.0f, "%.0f units/s",
                         ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemDeactivatedAfterEdit())
            Components::Actors::FitOnlyWalk(actor);
        ImGuiEx::Keyframeable::Record("actors.pathspeed");
        ImGui::SetItemTooltip(
            "How fast its walking clips walk it along the path; the walk, run or sprint is picked "
            "for it (a clip's own speed, in its Advanced fold, goes first). A walk to the path's end is made as long "
            "as it takes then");

        // the speeds of the rifle's animations, and their stances; a dog's breed's
        if (dog)
        {
            const auto presets = ActorLogic::GetSpeedPresets(body);
            std::vector<std::string> tips, records;
            for (const auto& preset : presets)
            {
                tips.push_back(std::format("{:.0f} units/s", preset.speed));
                std::string name(preset.name);
                std::ranges::transform(name, name.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
                records.push_back("actors.speed." + name);
            }
            std::vector<RowButton> buttons;
            for (std::size_t i = 0; i < presets.size(); ++i)
                buttons.push_back({i == 0                    ? ICON_FA_DOG
                                   : i + 1 == presets.size() ? ICON_FA_FORWARD_FAST
                                                             : ICON_FA_PAW,
                                   presets[i].name.data(), tips[i].c_str(), false,
                                   std::abs(actor.pathSpeed - presets[i].speed) < 0.5f, records[i].c_str()});
            Label("");
            if (const auto chosen = ButtonRow("##actorDogSpeeds", buttons))
            {
                actor.pathSpeed = presets[*chosen].speed;
                Components::Actors::FitOnlyWalk(actor);
            }
            return;
        }
        // its movement set's: standing, then crouched
        const auto presets = ActorLogic::GetSpeedPresets(body);
        const auto isPreset = [&](const ActorLogic::SpeedPreset& preset) {
            const bool crouching = actor.stance == Types::ActorStance::Crouch;
            return std::abs(actor.pathSpeed - preset.speed) < 0.5f &&
                   (preset.stance == Types::ActorStance::Crouch
                        ? crouching
                        : !crouching && actor.stance != Types::ActorStance::Prone);
        };
        std::vector<std::string> tips, records;
        for (const auto& preset : presets)
        {
            tips.push_back(std::format("{:.0f} units/s{}", preset.speed,
                                       preset.stance == Types::ActorStance::Crouch ? ", crouching" : ""));
            std::string name;
            for (const auto c : preset.name)
                if (c != ' ')
                    name += static_cast<char>(std::tolower(c));
            records.push_back("actors.speed." + name);
        }
        std::optional<std::size_t> chosen;
        for (const bool crouched : {false, true})
        {
            std::vector<RowButton> buttons;
            std::vector<std::size_t> indices;
            for (std::size_t i = 0; i < presets.size(); ++i)
            {
                if ((presets[i].stance == Types::ActorStance::Crouch) != crouched)
                    continue;
                using ActorLogic::Gait;
                const auto icon = crouched ? (presets[i].gait == Gait::Walk ? ICON_FA_PERSON_ARROW_DOWN_TO_LINE
                                                                            : ICON_FA_ANGLES_DOWN)
                                  : presets[i].gait == Gait::Walk ? ICON_FA_PERSON_WALKING
                                  : presets[i].gait == Gait::Run  ? ICON_FA_PERSON_RUNNING
                                                                  : ICON_FA_FORWARD_FAST;
                indices.push_back(i);
                buttons.push_back({icon, presets[i].name.data(), tips[i].c_str(), false, isPreset(presets[i]),
                                   records[i].c_str()});
            }
            if (buttons.empty())
                continue;
            Label("");
            if (const auto pressed = ButtonRow(crouched ? "##actorCrouchSpeeds" : "##actorSpeeds", buttons))
                chosen = indices[*pressed];
        }
        if (chosen)
        {
            const auto& preset = presets[*chosen];
            actor.pathSpeed = preset.speed;
            if (preset.stance == Types::ActorStance::Crouch)
                actor.stance = Types::ActorStance::Crouch;
            else if (actor.stance == Types::ActorStance::Crouch || actor.stance == Types::ActorStance::Prone)
                actor.stance = Types::ActorStance::Auto;
            Components::Actors::FitOnlyWalk(actor);
        }
    }

    // What an attached actor is on: another actor or a player
    void ActorsMenu::RenderAttachment(Types::Actor& actor)
    {
        auto& attachment = actor.attachment;
        const auto players = GetPlayers();
        std::string describe = "Choose an actor or a player";
        if (attachment.kind == Types::ActorAttachment::Kind::Actor)
        {
            if (const auto other = Components::Actors::GetActor(attachment.actor); other && other->id != actor.id)
                describe = ICON_FA_PERSON " " + other->name;
        }
        else
        {
            const auto player = std::ranges::find(players, attachment.player, [](const auto& p) { return p.first; });
            describe = ICON_FA_USER " " +
                       (player != players.end() ? player->second : std::format("Player {}", attachment.player));
        }

        Label("On");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##attachedTo", describe.c_str(), ImGuiComboFlags_HeightLarge))
        {
            ImGui::SeparatorText("Actors");
            for (const auto& other : Components::Actors::GetActors())
            {
                if (other.id == actor.id)
                    continue;
                const bool chosen =
                    attachment.kind == Types::ActorAttachment::Kind::Actor && attachment.actor == other.id;
                if (ImGui::Selectable(std::format("{}##actor{}", other.name, other.id).c_str(), chosen))
                {
                    attachment.kind = Types::ActorAttachment::Kind::Actor;
                    attachment.actor = other.id;
                }
            }
            ImGui::SeparatorText("Players");
            for (const auto& [client, name] : players)
            {
                const bool chosen =
                    attachment.kind == Types::ActorAttachment::Kind::Player && attachment.player == client;
                if (ImGui::Selectable(std::format("{}##player{}", name, client).c_str(), chosen))
                {
                    attachment.kind = Types::ActorAttachment::Kind::Player;
                    attachment.player = client;
                }
            }
            if (players.empty())
                ImGui::TextDisabled("Finding players...");
            ImGui::EndCombo();
        }
    }

    // The bone of what an attached actor is on, and where from it
    void ActorsMenu::RenderAttachmentDetails(Types::Actor& actor)
    {
        auto& attachment = actor.attachment;
        Label("Bone");
        TagCombo("##attachedTag", attachment.tag);
        ImGuiEx::Keyframeable::Record("actors.movement.bone");
        Label("Moved");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat3("##attachedOffset", &attachment.offset.x, 0.5f, 0.0f, 0.0f, "%.1f");
        ImGui::SetItemTooltip("From the bone: forward, left and up of the way it faces");
        Label("Facing");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##attachedYaw", &attachment.yaw, 1.0f, -180.0f, 180.0f, "%.0f deg");
        ImGui::SetItemTooltip("Turned from the way the bone faces");
    }

    // Delete (or Ctrl+D) on a page: the selected clip, event, path point or key at the playhead, or the actor itself
    void ActorsMenu::DeleteOrDuplicate(Types::Actor& actor, bool remove)
    {
        const auto tick = Components::Playback::GetActorTick();
        // what was deleted, for the message, as the step of the undo history is named
        std::string deleted;
        switch (page)
        {
            case Page::Look:
                if (remove)
                {
                    Components::Actors::Remove(actor.id);
                    Toasts::Show(actor.name + " deleted", true);
                }
                else
                {
                    Components::Actors::Select(Components::Actors::Duplicate(actor.id));
                }
                return;
            case Page::Animation:
            {
                if (const auto faceClip = Components::Actors::GetSelectedFaceClip())
                {
                    const auto selected = std::ranges::find(actor.faceClips, *faceClip, &Types::ActorClip::id);
                    if (selected == actor.faceClips.end())
                        return;
                    if (remove)
                    {
                        actor.faceClips.erase(selected);
                        Components::Actors::SelectFaceClip(std::nullopt);
                        deleted = "face clip";
                    }
                    else
                    {
                        auto copy = *selected;
                        copy.start = selected->End();
                        Components::Actors::SelectFaceClip(ActorLogic::AddFaceClip(actor, copy));
                    }
                    break;
                }
                const auto clip = Components::Actors::GetSelectedClip();
                if (!clip)
                    return;
                if (remove)
                {
                    std::erase_if(actor.clips, [&](const Types::ActorClip& entry) { return entry.id == *clip; });
                    Components::Actors::SelectClip(std::nullopt);
                    deleted = "clip";
                }
                else
                {
                    Components::Actors::SelectClip(ActorLogic::DuplicateClip(actor, *clip));
                }
                break;
            }
            case Page::Events:
            {
                const auto event = Components::Actors::GetSelectedEvent();
                if (!event)
                    return;
                if (remove)
                {
                    std::erase_if(actor.events, [&](const Types::ActorEvent& entry) { return entry.id == *event; });
                    Components::Actors::SelectEvent(std::nullopt);
                    deleted = "event";
                }
                else
                {
                    Components::Actors::SelectEvent(ActorLogic::DuplicateEvent(actor, *event));
                }
                break;
            }
            case Page::Movement:
            {
                if (actor.movement == Types::ActorMovement::Keyframes && remove)
                {
                    std::erase_if(actor.keys, [&](const Types::ActorKey& key) { return key.tick == tick; });
                    deleted = "key";
                }
                else if (actor.movement == Types::ActorMovement::Path)
                {
                    const auto point = Components::Actors::GetSelectedWaypoint();
                    if (remove && point && *point < actor.path.size() && actor.path.size() > 1)
                    {
                        ActorLogic::RemoveWaypoint(actor, *point);
                        Components::Actors::SelectWaypoint(std::nullopt);
                        deleted = "path point";
                    }
                    else if (!remove)
                    {
                        Components::Actors::SelectWaypoint(ActorLogic::InsertWaypoint(
                            actor, point.value_or(std::max<std::size_t>(actor.path.size(), 1) - 1)));
                    }
                }
                break;
            }
        }
        if (deleted.empty())
        {
            Components::Actors::Update(actor);
            return;
        }
        Components::Actors::Update(actor, false, std::format("delete {} of {}", deleted, actor.name));
        Toasts::Show(std::format("{} of {} deleted", Capitalized(deleted), actor.name), true);
    }

    std::optional<std::string> ActorsMenu::GetEditUnavailableReason(bool remove)
    {
        if (!UIManager::Get().IsShown(PanelLogic::Page::Actors) ||
            Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo || !Components::Actors::GetCatalog())
            return "Select an actor on the Scene tab";
        const auto selected = Components::Actors::GetSelected();
        const auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt;
        if (!actor)
            return "Select an actor first";

        switch (page)
        {
            case Page::Look:
                return std::nullopt;
            case Page::Animation:
                if (!Components::Actors::GetSelectedClip() && !Components::Actors::GetSelectedFaceClip())
                    return "Select a clip first";
                return std::nullopt;
            case Page::Events:
                if (!Components::Actors::GetSelectedEvent())
                    return "Select an event first";
                return std::nullopt;
            case Page::Movement:
            {
                if (actor->movement == Types::ActorMovement::Keyframes)
                {
                    if (!remove)
                        return "Keys are added with the gizmo or Add key";
                    const auto tick = Components::Playback::GetActorTick();
                    if (std::ranges::find(actor->keys, tick, &Types::ActorKey::tick) == actor->keys.end())
                        return "There's no key at the playhead";
                    return std::nullopt;
                }
                if (actor->movement == Types::ActorMovement::Path)
                {
                    const auto point = Components::Actors::GetSelectedWaypoint();
                    if (remove && (!point || *point >= actor->path.size()))
                        return "Select a path point first";
                    if (remove && actor->path.size() <= 1)
                        return "A path keeps at least one point";
                    return std::nullopt;
                }
                return "The actor stands still";
            }
        }
        return std::nullopt;
    }

    void ActorsMenu::ShowPage(Page shown)
    {
        requestedPage = shown;
    }

    void ActorsMenu::AddEffect()
    {
        openEffectBrowser = true;
        effectBrowserAdds = true;
        effectBrowsed.clear();
        UIManager::Get().SelectTab(PanelLogic::Tab::Scene, PanelLogic::Page::Actors);
    }

    void ActorsMenu::AddProp()
    {
        openPropBrowser = true;
        propBrowserAdds = true;
        propBrowsed.clear();
        UIManager::Get().SelectTab(PanelLogic::Tab::Scene, PanelLogic::Page::Actors);
    }

    void ActorsMenu::DeleteOrDuplicateSelected(bool remove)
    {
        const auto selected = Components::Actors::GetSelected();
        if (auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt)
            DeleteOrDuplicate(*actor, remove);
    }

    void ActorsMenu::RenderInspector()
    {
        // (set again while a walk's piece is shown)
        pieceScope.reset();
        const auto selected = Components::Actors::GetSelected();
        auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt;
        if (!actor)
        {
            // (with none, the list says how to add one)
            if (!Components::Actors::GetActors().empty())
                ImGui::TextDisabled("Select an actor.");
            return;
        }
        const auto catalog = Components::Actors::GetCatalog();
        const auto original = *actor;

        // what was picked on the timeline or in the view: its section is opened and scrolled to (another actor's
        // clip 1 is another clip, also when no actor was shown between them)
        const auto clip = Components::Actors::GetSelectedClip();
        const auto faceClip = Components::Actors::GetSelectedFaceClip();
        const auto event = Components::Actors::GetSelectedEvent();
        const auto waypoint = Components::Actors::GetSelectedWaypoint();
        if (seenActor != actor->id)
        {
            // (Delete and Duplicate act on another actor itself, until a section of it is clicked)
            seenActor = actor->id;
            page = Page::Look;
            seenClip.reset();
            seenFaceClip.reset();
            seenEvent.reset();
            seenWaypoint.reset();
        }
        if ((clip && clip != seenClip) || (faceClip && faceClip != seenFaceClip))
            requestedPage = Page::Animation;
        if (event && event != seenEvent)
            requestedPage = Page::Events;
        // (a point of the path, but not the walk's piece it is in, picked with the walk: that is shown in Animation)
        const auto pieces =
            clip && waypoint ? Components::Actors::GetWalkPieces(*actor) : std::vector<ActorLogic::WalkPiece>{};
        const bool piecePicked = std::ranges::any_of(pieces, [&](const ActorLogic::WalkPiece& piece) {
            return piece.clip == *clip && *waypoint >= piece.first && *waypoint <= piece.last;
        });
        if (waypoint && waypoint != seenWaypoint && !piecePicked)
            requestedPage = Page::Movement;
        seenClip = clip;
        seenFaceClip = faceClip;
        seenEvent = event;
        seenWaypoint = waypoint;
        if (requestedPage)
            page = *requestedPage;
        if (requestedPage == Page::Movement && waypoint)
        {
            pointShownAt = std::chrono::steady_clock::now();
            scrollToPoint = true;
        }

        // an effect in the scene: its own sections
        if (actor->effect)
        {
            RenderSceneEffect(*actor, *catalog);
            requestedPage.reset();
            openSections = false;
            if (*actor != original)
                Components::Actors::Update(*actor);
            return;
        }

        // a light in the scene: its own sections
        if (actor->light)
        {
            RenderSceneLight(*actor);
            requestedPage.reset();
            openSections = false;
            if (*actor != original)
                Components::Actors::Update(*actor);
            return;
        }

        // the Mortar Team: its own sections
        if (actor->mortar)
        {
            RenderSceneMortar(*actor, *catalog);
            requestedPage.reset();
            openSections = false;
            if (*actor != original)
                Components::Actors::Update(*actor);
            return;
        }

        // a prop in the scene: its own sections
        if (actor->placedProp)
        {
            RenderSceneProp(*actor, *catalog);
            requestedPage.reset();
            openSections = false;
            if (*actor != original)
                Components::Actors::Update(*actor);
            return;
        }

        // what the Events section's Advanced holds: a Fire's interval and tracers, a Look's turn time
        const auto selectedEvent = std::ranges::find(actor->events, event.value_or(0), &Types::ActorEvent::id);
        const bool eventMore = selectedEvent != actor->events.end() &&
                               (selectedEvent->type == Types::ActorEventType::Fire ||
                                selectedEvent->type == Types::ActorEventType::Look);

        // one section of the actor's: opened and scrolled to when asked; a click in it gives it Delete and Ctrl+D
        const auto section = [&](const char* label, Page which, const std::function<void()>& body) {
            constexpr std::array NAMES{"look", "animation", "movement", "events"};
            constexpr std::array TITLES{"Look", "Animation", "Movement", "Events"};
            const auto name = NAMES[static_cast<std::size_t>(which)];
            ImGui::Dummy(ImVec2(0, 1));
            // (right before the header: any item in between, the spacer too, takes the request)
            if (requestedPage == which || openSections || advancedOpened == which)
                ImGui::SetNextItemOpen(true);
            const bool open =
                ImGui::CollapsingHeader(std::format("{}##section{}", label, name).c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
            // (the harness clicks them as actors.page.look and so on)
            ImGuiEx::Keyframeable::Record(std::string("actors.page.") + name);
            if (requestedPage == which && !openSections)
                ImGui::SetScrollHereY(0.0f);
            if (advancedOpened == which)
                advancedOpened.reset();

            // its Advanced, at the header's right: dim when it has nothing more for what is selected
            {
                const auto headerMin = ImGui::GetItemRectMin(), headerMax = ImGui::GetItemRectMax();
                const auto after = ImGui::GetCursorScreenPos();
                const auto& style = ImGui::GetStyle();
                const auto padding = ImVec2(style.FramePadding.x, 1.0f);
                const auto box = ImGui::GetFontSize() + padding.y * 2.0f;
                const auto width = box + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Advanced").x;
                ImGui::SetCursorScreenPos(ImVec2(headerMax.x - width - style.FramePadding.x,
                                                 headerMin.y + (headerMax.y - headerMin.y - box) / 2.0f));
                const bool more = which != Page::Events || eventMore;
                bool advanced = IsAdvanced(which);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * (more ? 1.0f : 0.45f));
                if (ImGui::Checkbox(std::format("Advanced##advanced{}", name).c_str(), &advanced))
                {
                    const bool all = ImGui::GetIO().KeyAlt;
                    SetAdvanced(which, advanced, all);
                    // (turned on while folded, the section opens)
                    if (advanced && !open)
                        advancedOpened = which;
                }
                ImGui::PopStyleVar(2);
                ImGuiEx::Keyframeable::Record(std::string("actors.advanced.") + name);
                if (more)
                    ImGui::SetItemTooltip("Shows the rest of %s's options, marked at the left. Alt+click: every "
                                          "section's",
                                          TITLES[static_cast<std::size_t>(which)]);
                else
                    ImGui::SetItemTooltip("Nothing more here for this event");
                ImGui::SetCursorScreenPos(after);
            }
            if (!open)
                return;
            ImGui::BeginGroup();
            body();
            ImGui::EndGroup();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                     ImGuiHoveredFlags_AllowWhenBlockedByPopup) &&
                (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
                page = which;
        };
        section(ICON_FA_SHIRT " Look", Page::Look, [&] { RenderLook(*actor, *catalog); });
        section(ICON_FA_ROUTE " Movement", Page::Movement, [&] { RenderMovement(*actor); });
        section(ICON_FA_FILM " Animation", Page::Animation, [&] { RenderClips(*actor, *catalog); });
        section(ICON_FA_BOLT " Events", Page::Events, [&] { RenderEvents(*actor, *catalog); });
        requestedPage.reset();
        openSections = false;
        scrollToPoint = false;

        if (*actor != original)
            Components::Actors::Update(*actor);
    }

    // An effect in the scene's sections: Effect (the effect, picked with the effect browser,
    // and how it plays by itself), Placement (on what the camera looks at or the ground below, where it is and which
    // way it fires, the gizmo) and Plays (Auto, Once or Loop, how it ends, how often a looping one-shot plays again).
    // Nothing is behind an Advanced fold: there is little.
    void ActorsMenu::RenderSceneEffect(Types::Actor& actor, const Catalog& catalog)
    {
        auto& effect = *actor.effect;
        auto& graphics = GFX::GraphicsManager::Get();
        const auto found = catalog.effectInfo.find(effect.name);
        const bool known = found != catalog.effectInfo.end();
        const auto info = known ? found->second : Catalog::EffectInfo{};
        const auto header = [](const char* label, const char* name) {
            ImGui::Dummy(ImVec2(0, 1));
            const bool open = ImGui::CollapsingHeader(std::format("{}##effectSection{}", label, name).c_str(),
                                                      ImGuiTreeNodeFlags_DefaultOpen);
            ImGuiEx::Keyframeable::Record(std::string("actors.effect.page.") + name);
            return open;
        };

        if (header(ICON_FA_WAND_MAGIC_SPARKLES " Effect", "effect"))
        {
            Label("Effect");
            if (ImGui::Button(std::format("{}  " ICON_FA_CARET_DOWN "##effectName", effect.name).c_str(),
                              ImVec2(-FLT_MIN, 0)))
            {
                openEffectBrowser = true;
                effectBrowserAdds = false;
                effectBrowsed = effect.name;
            }
            ImGuiEx::Keyframeable::Record("actors.effect.name");
            ImGui::SetItemTooltip("Another of the map's effects, in its place");
            Label("");
            ImGui::PushTextWrapPos(0.0f);
            if (!known)
                ImGui::TextColored(Theme::Misfit(),
                                   ICON_FA_TRIANGLE_EXCLAMATION " This map hasn't it: it doesn't play");
            else
                ImGui::TextDisabled("%s%s", ActorLogic::DescribeEffect(info).c_str(),
                                    ActorLogic::EffectLoops(info) ? ": one of the game's 1,024 while it plays" : "");
            ImGui::PopTextWrapPos();
        }

        if (header(ICON_FA_LOCATION_CROSSHAIRS " Placement", "placement"))
        {
            const auto place = ButtonRow(
                "##effectPlace",
                {{ICON_FA_LOCATION_CROSSHAIRS, "To crosshair",
                  "Puts it on what the camera looks at, facing out of it", false, false, "actors.effect.crosshair"},
                 {ICON_FA_ARROW_DOWN, "Drop to ground", "Puts it on the ground below it, facing up", false, false,
                  "actors.effect.ground"}});
            if (place == 0)
                PlaceInFrontOfCamera(actor);
            else if (place == 1)
                Components::Actors::RequestPlacement(actor.id, actor.position + glm::vec3(0, 0, 8),
                                                     glm::vec3(0, 0, -1));
            Label("Position");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::DragFloat3("##effectPosition", &actor.position.x, 1.0f, 0.0f, 0.0f, "%.1f");
            ImGuiEx::Keyframeable::Record("actors.effect.position");
            Label("Facing");
            glm::vec3 facing(effect.pitch, actor.yaw, effect.roll);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat3("##effectFacing", &facing.x, 1.0f, 0.0f, 0.0f, "%.1f deg"))
            {
                effect.pitch = std::clamp(facing.x, -90.0f, 90.0f);
                actor.yaw = std::remainder(facing.y, 360.0f);
                effect.roll = std::remainder(facing.z, 360.0f);
            }
            ImGuiEx::Keyframeable::Record("actors.effect.facing");
            ImGui::SetItemTooltip("Pitch, yaw and roll of the way it fires: pitch -90 is straight up");
            Label("Gizmo");
            const auto gizmoMode = graphics.GetGizmoMode();
            const auto gizmo = ButtonRow(
                "##effectGizmo",
                {{ICON_FA_MAXIMIZE, "Local", "Moves it along its own axes", false,
                  gizmoMode == GFX::GizmoMode::TranslateLocal, "actors.effect.gizmo"},
                 {ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "World", "Moves it along the world's axes", false,
                  gizmoMode == GFX::GizmoMode::TranslateGlobal},
                 {ICON_FA_ROTATE, "Rotate", "Turns it about up", false, gizmoMode == GFX::GizmoMode::Rotate}});
            if (gizmo)
            {
                graphics.SetGizmoMode(*gizmo == 0   ? GFX::GizmoMode::TranslateLocal
                                      : *gizmo == 1 ? GFX::GizmoMode::TranslateGlobal
                                                    : GFX::GizmoMode::Rotate);
                graphics.FocusActor();
            }
        }

        if (header(ICON_FA_REPEAT " Plays", "plays"))
        {
            const bool loops = ActorLogic::EffectLoops(info);
            const auto autoTip = std::format("As the effect is made: it {}", loops ? "loops" : "plays once");
            const RowButton plays[] = {
                {ICON_FA_WAND_MAGIC_SPARKLES, "Auto", autoTip.c_str(), false, effect.plays == Types::EffectPlays::Auto,
                 "actors.effect.plays.auto"},
                {ICON_FA_PLAY, "Once", "Once from its start: a looping one for as long as it loops by itself", false,
                 effect.plays == Types::EffectPlays::Once, "actors.effect.plays.once"},
                {ICON_FA_REPEAT, "Loop", "Again as it ends, till its bar's end", false,
                 effect.plays == Types::EffectPlays::Loop, "actors.effect.plays.loop"}};
            Label("Plays");
            if (const auto picked = ButtonRow("##effectPlays", plays))
                effect.plays = *picked == 0 ? Types::EffectPlays::Auto
                               : *picked == 1 ? Types::EffectPlays::Once
                                              : Types::EffectPlays::Loop;
            Label("Ends");
            if (const auto picked =
                    ButtonRow("##effectEnds", {{ICON_FA_FEATHER, "Fade", "Its particles finish as its bar ends", false,
                                                effect.ends == Types::EffectEnds::Fade, "actors.effect.ends.fade"},
                                               {ICON_FA_SCISSORS, "Cut", "Gone at once as its bar ends", false,
                                                effect.ends == Types::EffectEnds::Cut, "actors.effect.ends.cut"}}))
                effect.ends = *picked == 0 ? Types::EffectEnds::Fade : Types::EffectEnds::Cut;
            if (effect.plays == Types::EffectPlays::Loop && !loops)
            {
                Label("Every");
                float every = effect.every / 1000.0f;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragFloat("##effectEvery", &every, 0.01f, 0.1f, 60.0f, "%.2f s",
                                     ImGuiSliderFlags_AlwaysClamp))
                    effect.every = static_cast<std::uint32_t>(every * 1000.0f);
                ImGuiEx::Keyframeable::Record("actors.effect.every");
                ImGui::SetItemTooltip("How often it plays again");
            }
            Label("");
            ImGui::PushTextWrapPos(0.0f);
            const auto played = ActorLogic::GetEffectPlayedEnd(effect, info);
            if (ActorLogic::PlaysLooping(effect, info))
                ImGui::TextDisabled("Plays %s to %s, then %s", FormatActorTime(effect.start).c_str(),
                                    FormatActorTime(effect.end).c_str(),
                                    effect.ends == Types::EffectEnds::Fade ? "dies down" : "is gone at once");
            else
                ImGui::TextDisabled("Plays once at %s, till %s", FormatActorTime(effect.start).c_str(),
                                    FormatActorTime(played).c_str());
            ImGui::PopTextWrapPos();
        }
    }

    namespace
    {
        // a row of a light that has no keys: nothing where the keyed rows' diamond is, so that the labels line up
        void BeginLightRow(const char* label)
        {
            constexpr auto COLUMN_PERCENT = 0.4f;
            ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 1.5f, ImGui::GetFrameHeight()));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * COLUMN_PERCENT);
            ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * (1.0f - COLUMN_PERCENT) -
                                    ImGui::GetStyle().WindowPadding.x);
        }

        // What a light can follow: the demo's players and the scene's actors with a body
        std::vector<Types::BoneCameraTarget> GetFollowTargets()
        {
            std::vector<Types::BoneCameraTarget> targets;
            if (const auto events = Components::KillFinder::GetEvents())
            {
                for (const auto& [client, player] : events->players)
                    targets.push_back({Types::BoneCameraTargetKind::Player, client});
            }
            for (const auto& actor : Components::Actors::GetActors())
                if (!actor.models.empty())
                    targets.push_back({Types::BoneCameraTargetKind::Actor, static_cast<std::int32_t>(actor.id)});
            return targets;
        }

        std::string DescribeFollowTarget(const Types::BoneCameraTarget& target)
        {
            const auto name = Components::BoneCamera::DescribeTarget(target);
            return target.kind == Types::BoneCameraTargetKind::Player ? name + " (player)" : name;
        }

        std::string DescribeFollowBone(const std::string& bone)
        {
            const auto label = Components::BoneCameraLogic::DescribeBone(bone);
            return label == bone ? bone : std::format("{}  ({})", label, bone);
        }
    }  // namespace

    // A light in the scene's sections: Light (its kind and values, those that can have keys
    // with their diamonds), Placement (where it is and where it points, what it follows) and Flicker
    void ActorsMenu::RenderSceneLight(Types::Actor& actor)
    {
        namespace LightLogic = Components::LightLogic;
        using Channel = LightLogic::Channel;
        auto& light = *actor.light;
        auto& graphics = GFX::GraphicsManager::Get();
        const bool spot = light.kind == Types::LightKind::Spot;
        const auto header = [](const char* label, const char* name) {
            ImGui::Dummy(ImVec2(0, 1));
            const bool open = ImGui::CollapsingHeader(std::format("{}##lightSection{}", label, name).c_str(),
                                                      ImGuiTreeNodeFlags_DefaultOpen);
            ImGuiEx::Keyframeable::Record(std::string("actors.light.page.") + name);
            return open;
        };
        // a row with keys shows theirs at the playhead and keys it there; without, it is the light's own value
        const auto keyed = [&](Channel channel) {
            auto& keyframeManager = Components::KeyframeManager::Get();
            return !keyframeManager.GetKeyframes(Components::Lights::GetProperty(light.id, channel)).empty();
        };
        const auto slider = [&](const char* label, Channel channel, float& value, float low, float high,
                                const char* format, ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp) {
            auto shown = value;
            const ImGuiEx::DragSliderOptions options{.flags = flags};
            const auto name = std::format("{} {}", actor.name, label);
            if (ImGuiEx::Keyframeable::SliderFloat(label, &shown, low, high,
                                                   Components::Lights::GetProperty(light.id, channel).type, format,
                                                   options, name.c_str()) &&
                !keyed(channel))
                value = shown;
        };

        if (header(ICON_FA_LIGHTBULB " Light", "light"))
        {
            BeginLightRow("Kind");
            if (const auto picked = ButtonRow(
                    "##lightKind", {{ICON_FA_LIGHTBULB, "Point", "Lights all around it. No highlights", false, !spot,
                                     "actors.light.kind.point"},
                                    {ICON_FA_CIRCLE_DOT, "Spot", "A cone, with highlights. It can cast shadows", false,
                                     spot, "actors.light.kind.spot"}}))
                light.kind = *picked == 0 ? Types::LightKind::Point : Types::LightKind::Spot;
            auto color = light.color;
            const auto colorName = std::format("{} Color", actor.name);
            if (ImGuiEx::Keyframeable::ColorEdit3("Color", &color.x,
                                                  Components::Lights::GetProperty(light.id, Channel::Color).type,
                                                  std::nullopt, colorName.c_str()) &&
                !keyed(Channel::Color))
                light.color = color;
            slider("Intensity", Channel::Intensity, light.intensity, 0.0f, LightLogic::MAX_INTENSITY, "%.2f");
            slider("Radius", Channel::Radius, light.radius, LightLogic::MIN_RADIUS, LightLogic::MAX_RADIUS, "%.0f",
                   ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
            if (spot)
            {
                slider("Cone", Channel::Cone, light.cone, LightLogic::MIN_CONE, LightLogic::MAX_CONE, "%.0f deg");
                BeginLightRow("Softness");
                ImGuiEx::DragSlider("##lightSoftness", &light.softness, 0.0f, 1.0f, "%.2f",
                                    {.flags = ImGuiSliderFlags_AlwaysClamp});
                ImGuiEx::Keyframeable::Record("actors.light.softness");
                ImGui::SetItemTooltip("How much of its edge fades");
                BeginLightRow("Shadows");
                if (const auto picked = ButtonRow(
                        "##lightShadows",
                        {{ICON_FA_CIRCLE_XMARK, "Off", "Casts none", false, !light.shadows, "actors.light.shadows.off"},
                         {ICON_FA_CIRCLE_HALF_STROKE, "On", "One spot casts shadows at a time", false, light.shadows,
                          "actors.light.shadows.on"}}))
                    light.shadows = *picked == 1;
                if (LightLogic::LosesShadows(Components::Actors::GetActors(), actor.id,
                                             Components::Playback::GetActorTick()))
                {
                    BeginLightRow("");
                    ImGui::TextColored(Theme::Misfit(), "Off: one at a time");
                    ImGuiEx::Keyframeable::Record("actors.light.shadows.lost");
                }
                const bool advanced = ImGui::TreeNodeEx("Advanced##lightAdvanced", ImGuiTreeNodeFlags_SpanAvailWidth);
                ImGuiEx::Keyframeable::Record("actors.light.advanced");
                if (advanced)
                {
                    BeginLightRow("Highlights");
                    ImGuiEx::DragSlider("##lightHighlights", &light.highlights, 0.0f, LightLogic::MAX_HIGHLIGHTS,
                                        "%.2f", {.resetValue = 1.0f, .flags = ImGuiSliderFlags_AlwaysClamp});
                    ImGuiEx::Keyframeable::Record("actors.light.highlights");
                    ImGui::SetItemTooltip("How strong its highlights are");
                    ImGui::TreePop();
                }
            }
        }

        if (header(ICON_FA_LOCATION_CROSSHAIRS " Placement", "placement"))
        {
            const auto& camera = Components::CameraManager::Get().GetActiveCamera();
            // (on a bone, its Offset places it and its Aim is from the bone)
            if (!light.follow)
            {
                std::vector<RowButton> buttons = {
                    {ICON_FA_LOCATION_CROSSHAIRS, "To crosshair",
                     spot ? "Puts it at the camera, aimed at what it looks at"
                          : "Puts it just off what the camera looks at",
                     false, false, "actors.light.crosshair"}};
                if (spot)
                    buttons.push_back({ICON_FA_BULLSEYE, "Aim at crosshair", "Points it at what the camera looks at",
                                       false, false, "actors.light.aim"});
                if (const auto place = ButtonRow("##lightPlace", buttons))
                {
                    if (*place == 0)
                        Components::Actors::RequestPlacement(actor.id, camera->GetPosition(),
                                                             camera->GetForwardVector());
                    else
                        Components::Actors::RequestAim(actor.id, camera->GetPosition(), camera->GetForwardVector());
                }
            }
            // its place keyed as its other values are: one key holds
            // a row's values; on a bone the same row is its Offset, and so are its keys
            const auto placeName = std::format("{} {}", actor.name, light.follow ? "Offset" : "Position");
            Types::KeyframeValue place(LightLogic::GetValue(actor, Channel::Position));
            if (ImGuiEx::Keyframeable::Row(
                    light.follow ? "Offset" : "Position",
                    Components::Lights::GetProperty(light.id, Channel::Position).type, place,
                    [&](const char* id, Types::KeyframeValue& value) {
                        const bool changed =
                            ImGui::DragFloat3(id, &value.vector3.x, light.follow ? 0.25f : 1.0f, 0.0f, 0.0f, "%.1f");
                        ImGuiEx::Keyframeable::Record(light.follow ? "actors.light.offset" : "actors.light.position");
                        ImGui::SetItemTooltip("%s",
                                              light.follow ? "Forward, left and up from the bone" : "Where it is");
                        return changed;
                    },
                    placeName.c_str()) &&
                !keyed(Channel::Position))
                LightLogic::SetValue(actor, Channel::Position, place);
            if (spot)
            {
                const auto& aimProperty = Components::Lights::GetProperty(light.id, Channel::Aim);
                const auto aimName = std::format("{} Aim", actor.name);
                Types::KeyframeValue aim(LightLogic::GetValue(actor, Channel::Aim));
                if (ImGuiEx::Keyframeable::Row(
                        "Aim", aimProperty.type, aim,
                        [&](const char* id, Types::KeyframeValue& value) {
                            if (!ImGui::DragFloat2(id, &value.vector3.x, 1.0f, 0.0f, 0.0f, "%.1f deg"))
                            {
                                ImGuiEx::Keyframeable::Record("actors.light.aimangles");
                                ImGui::SetItemTooltip("Pitch and yaw of the way it points: pitch 90 is straight down");
                                return false;
                            }
                            ImGuiEx::Keyframeable::Record("actors.light.aimangles");
                            value.vector3.x = std::clamp(value.vector3.x, -90.0f, 90.0f);
                            value.vector3.z = 0.0f;
                            // (a key takes the yaw nearest the key before it, so it turns the short way)
                            value.vector3 =
                                LightLogic::GetAimForKey(Components::KeyframeManager::Get().GetKeyframes(aimProperty),
                                                         Components::Playback::GetTimelineTick(), value.vector3);
                            return true;
                        },
                        aimName.c_str()) &&
                    !keyed(Channel::Aim))
                    LightLogic::SetValue(actor, Channel::Aim, aim);
            }
            if (!light.follow)
            {
                BeginLightRow("Gizmo");
                const bool turning = spot && graphics.GetGizmoMode() == GFX::GizmoMode::Rotate;
                std::vector<RowButton> gizmos = {{ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "Move",
                                                  "Moves it along the world's axes", false, !turning,
                                                  "actors.light.gizmo"}};
                if (spot)
                    gizmos.push_back({ICON_FA_ROTATE, "Rotate", "Turns it about up", false, turning,
                                      "actors.light.gizmo.rotate"});
                if (const auto gizmo = ButtonRow("##lightGizmo", gizmos))
                {
                    graphics.SetGizmoMode(*gizmo == 0 ? GFX::GizmoMode::TranslateGlobal : GFX::GizmoMode::Rotate);
                    graphics.FocusActor();
                }
            }

            // what it follows: an actor or a player, the bone, how far from it
            BeginLightRow("Follow");
            const auto followed = light.follow ? std::optional(light.follow->target.kind) : std::nullopt;
            // (as wide as their words: an even row has room for icons alone)
            const auto followButton = [](const char* text, const char* tooltip, bool active, const char* record) {
                if (active)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
                const bool clicked = ImGui::Button(text);
                ImGuiEx::Keyframeable::Record(record);
                if (active)
                    ImGui::PopStyleColor();
                ImGui::SetItemTooltip("%s", tooltip);
                return clicked;
            };
            if (followButton("None", "Stays where it is", !followed, "actors.light.follow.none"))
                light.follow.reset();
            ImGui::SameLine();
            if (followButton("Actor...", "A bone of an actor", followed == Types::BoneCameraTargetKind::Actor,
                             "actors.light.follow.actor"))
                ImGui::OpenPopup("##lightFollowActor");
            ImGui::SameLine();
            if (followButton("Player...", "A bone of a player", followed == Types::BoneCameraTargetKind::Player,
                             "actors.light.follow.player"))
                ImGui::OpenPopup("##lightFollowPlayer");
            const auto followMenu = [&](const char* popup, Types::BoneCameraTargetKind kind, const char* name,
                                        const char* none) {
                if (!ImGui::BeginPopup(popup))
                    return;
                auto any = false;
                for (const auto& target : GetFollowTargets())
                {
                    if (target.kind != kind)
                        continue;
                    any = true;
                    const bool chosen = light.follow && light.follow->target == target;
                    if (ImGui::Selectable(DescribeFollowTarget(target).c_str(), chosen))
                    {
                        auto follow = light.follow.value_or(Types::LightFollow{.bone = "tag_weapon_right"});
                        follow.target = target;
                        light.follow = follow;
                    }
                    ImGuiEx::Keyframeable::Record(std::format("actors.light.follow.{}{}", name, target.id));
                }
                if (!any)
                    ImGui::TextDisabled("%s", none);
                ImGui::EndPopup();
            };
            followMenu("##lightFollowActor", Types::BoneCameraTargetKind::Actor, "actor", "No actors");
            followMenu("##lightFollowPlayer", Types::BoneCameraTargetKind::Player, "player", "No players");
            if (light.follow)
            {
                BeginLightRow("Target");
                ImGui::TextUnformatted(DescribeFollowTarget(light.follow->target).c_str());
                const auto bones = Mod::GetGameInterface()->GetTargetBones(light.follow->target);
                const auto [ordered, named] = Components::BoneCameraLogic::OrderBones(bones);
                BeginLightRow("Bone");
                if (ImGui::BeginCombo("##lightBone", DescribeFollowBone(light.follow->bone).c_str(),
                                      ImGuiComboFlags_HeightLarge))
                {
                    for (std::size_t i = 0; i < ordered.size(); ++i)
                    {
                        if (i == named && named > 0)
                            ImGui::Separator();
                        if (ImGui::Selectable(DescribeFollowBone(ordered[i]).c_str(), ordered[i] == light.follow->bone))
                            light.follow->bone = ordered[i];
                        ImGuiEx::Keyframeable::Record("actors.light.bone." + ordered[i]);
                    }
                    if (ordered.empty())
                        ImGui::TextDisabled("Its bones show once it is drawn");
                    ImGui::EndCombo();
                }
                ImGuiEx::Keyframeable::Record("actors.light.bone");
                // which gun it goes by in that player's own view
                if (light.follow->target.kind == Types::BoneCameraTargetKind::Player)
                {
                    using Mode = Types::LightFirstPerson;
                    const auto mode = light.follow->firstPerson;
                    // (words, as Follow's: icons alone don't say these)
                    BeginLightRow("First person");
                    if (followButton("Auto", "The gun on screen for the gun's bones, the body for the rest",
                                     mode == Mode::Auto, "actors.light.firstperson.auto"))
                        light.follow->firstPerson = Mode::Auto;
                    ImGui::SameLine();
                    if (followButton("Body", "The body, as the free camera shows it", mode == Mode::Body,
                                     "actors.light.firstperson.body"))
                        light.follow->firstPerson = Mode::Body;
                    ImGui::SameLine();
                    if (followButton("View gun", "The gun and arms on screen, for the bones they have",
                                     mode == Mode::ViewGun, "actors.light.firstperson.viewgun"))
                        light.follow->firstPerson = Mode::ViewGun;
                }
                BeginLightRow("Steady");
                ImGui::SliderFloat("##lightSteady", &light.follow->steady, 0.0f, 0.5f,
                                   light.follow->steady > 0.0f ? "%.2f s" : "Off", ImGuiSliderFlags_AlwaysClamp);
                ImGuiEx::Keyframeable::Record("actors.light.steady");
                ImGui::SetItemTooltip("Smooths the bone's sway over this long");
            }
        }

        if (header(ICON_FA_BOLT " Flicker", "flicker"))
        {
            BeginLightRow("Amount");
            ImGuiEx::DragSlider("##lightFlickerAmount", &light.flickerAmount, 0.0f, 1.0f, "%.2f",
                                {.flags = ImGuiSliderFlags_AlwaysClamp});
            ImGuiEx::Keyframeable::Record("actors.light.flicker.amount");
            ImGui::SetItemTooltip("How far it dips");
            BeginLightRow("Rate");
            ImGuiEx::DragSlider("##lightFlickerRate", &light.flickerRate, 0.0f, LightLogic::MAX_FLICKER_RATE, "%.1f /s",
                                {.flags = ImGuiSliderFlags_AlwaysClamp});
            ImGuiEx::Keyframeable::Record("actors.light.flicker.rate");
            ImGui::SetItemTooltip("How often it changes");
        }
    }

    // A prop in the scene's sections: Prop (its model, picked with the prop
    // browser) and Placement (on what the camera looks at or the ground below, where it stands and how it's turned,
    // both with keys as a light's place has (DJ1), its scale (DI1), the gizmo)
    void ActorsMenu::RenderSceneProp(Types::Actor& actor, const Catalog& catalog)
    {
        namespace PropLogic = Components::PropLogic;
        using Channel = PropLogic::Channel;
        auto& prop = *actor.placedProp;
        auto& graphics = GFX::GraphicsManager::Get();
        const bool known = std::ranges::binary_search(catalog.props, prop.model);
        const auto header = [](const char* label, const char* name) {
            ImGui::Dummy(ImVec2(0, 1));
            const bool open = ImGui::CollapsingHeader(std::format("{}##propSection{}", label, name).c_str(),
                                                      ImGuiTreeNodeFlags_DefaultOpen);
            ImGuiEx::Keyframeable::Record(std::string("actors.prop.page.") + name);
            return open;
        };
        // a row with keys shows theirs at the playhead and keys it there; without, it is the prop's own value
        const auto keyed = [&](Channel channel) {
            return !Components::KeyframeManager::Get()
                        .GetKeyframes(Components::Props::GetProperty(prop.id, channel))
                        .empty();
        };

        if (header(ICON_FA_CUBE " Prop", "prop"))
        {
            BeginLightRow("Model");
            if (ImGui::Button(std::format("{}  " ICON_FA_CARET_DOWN "##propModel", prop.model).c_str(),
                              ImVec2(-FLT_MIN, 0)))
            {
                openPropBrowser = true;
                propBrowserAdds = false;
                propBrowsed = prop.model;
            }
            ImGuiEx::Keyframeable::Record("actors.prop.model");
            ImGui::SetItemTooltip("Another of the map's models, in its place");
            if (!known)
            {
                BeginLightRow("");
                ImGui::TextColored(Theme::Misfit(), ICON_FA_TRIANGLE_EXCLAMATION " This map hasn't it");
            }
        }

        if (header(ICON_FA_LOCATION_CROSSHAIRS " Placement", "placement"))
        {
            const auto& camera = Components::CameraManager::Get().GetActiveCamera();
            const auto place = ButtonRow(
                "##propPlace",
                {{ICON_FA_LOCATION_CROSSHAIRS, "To crosshair",
                  "Stands it on what the camera looks at, facing the camera", false, false, "actors.prop.crosshair"},
                 {ICON_FA_ARROW_DOWN, "Drop to ground", "Stands it on the ground below it", false, false,
                  "actors.prop.ground"}});
            if (place == 0)
                Components::Actors::RequestPlacement(actor.id, camera->GetPosition(), camera->GetForwardVector(),
                                                     std::nullopt, ActorActions::GetYawFacingCamera());
            else if (place == 1)
                Components::Actors::RequestPlacement(
                    actor.id,
                    Components::Props::WithKeyedPlacement(actor, Components::Playback::GetTimelineTick()).position +
                        glm::vec3(0, 0, 8),
                    glm::vec3(0, 0, -1));

            const auto positionName = std::format("{} Position", actor.name);
            Types::KeyframeValue position(PropLogic::GetValue(actor, Channel::Position));
            if (ImGuiEx::Keyframeable::Row(
                    "Position", Components::Props::GetProperty(prop.id, Channel::Position).type, position,
                    [&](const char* id, Types::KeyframeValue& value) {
                        const bool changed = ImGui::DragFloat3(id, &value.vector3.x, 1.0f, 0.0f, 0.0f, "%.1f");
                        ImGuiEx::Keyframeable::Record("actors.prop.position");
                        ImGui::SetItemTooltip("Where it stands");
                        return changed;
                    },
                    positionName.c_str()) &&
                !keyed(Channel::Position))
                PropLogic::SetValue(actor, Channel::Position, position);

            const auto& turnProperty = Components::Props::GetProperty(prop.id, Channel::Turn);
            const auto turnName = std::format("{} Turn", actor.name);
            Types::KeyframeValue turn(PropLogic::GetValue(actor, Channel::Turn));
            if (ImGuiEx::Keyframeable::Row(
                    "Turn", turnProperty.type, turn,
                    [&](const char* id, Types::KeyframeValue& value) {
                        const bool changed = ImGui::DragFloat3(id, &value.vector3.x, 1.0f, 0.0f, 0.0f, "%.1f deg");
                        ImGuiEx::Keyframeable::Record("actors.prop.turn");
                        ImGui::SetItemTooltip("Pitch, yaw and roll");
                        // (a key takes the angles nearest the key before it, so it turns the short way)
                        if (changed)
                            value.vector3 =
                                PropLogic::GetTurnForKey(Components::KeyframeManager::Get().GetKeyframes(turnProperty),
                                                         Components::Playback::GetTimelineTick(), value.vector3);
                        return changed;
                    },
                    turnName.c_str()) &&
                !keyed(Channel::Turn))
                PropLogic::SetValue(actor, Channel::Turn, turn);

            // (1 is as it was made)
            BeginLightRow("Scale");
            ImGui::SliderFloat("##propScale", &prop.scale, ActorLogic::MIN_PROP_SCALE, ActorLogic::MAX_PROP_SCALE,
                               "%.2f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
            ImGuiEx::Keyframeable::Record("actors.prop.scale");
            ImGui::SetItemTooltip("How big it is. Right-click: Default");
            if (ImGui::BeginPopupContextItem("##propScaleMenu"))
            {
                if (ImGui::MenuItem("Default"))
                    prop.scale = 1.0f;
                ImGui::EndPopup();
            }

            BeginLightRow("Gizmo");
            const auto gizmoMode = graphics.GetGizmoMode();
            const auto gizmo = ButtonRow(
                "##propGizmo",
                {{ICON_FA_MAXIMIZE, "Local", "Moves it along its own axes", false,
                  gizmoMode == GFX::GizmoMode::TranslateLocal, "actors.prop.gizmo"},
                 {ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "World", "Moves it along the world's axes", false,
                  gizmoMode == GFX::GizmoMode::TranslateGlobal},
                 {ICON_FA_ROTATE, "Rotate", "Turns it about up", false, gizmoMode == GFX::GizmoMode::Rotate}});
            if (gizmo)
            {
                graphics.SetGizmoMode(*gizmo == 0   ? GFX::GizmoMode::TranslateLocal
                                      : *gizmo == 1 ? GFX::GizmoMode::TranslateGlobal
                                                    : GFX::GizmoMode::Rotate);
                graphics.FocusActor();
            }
        }
    }

    // The Mortar Team's sections: Mortar Team (each point, put on what
    // the camera looks at, and the way the shells fly in) and Events (its Fire events: when the first shell lands, and
    // how many on each point)
    void ActorsMenu::RenderSceneMortar(Types::Actor& actor, const Catalog& catalog)
    {
        auto& mortar = *actor.mortar;
        const auto header = [](const char* label, const char* name) {
            ImGui::Dummy(ImVec2(0, 1));
            const bool open = ImGui::CollapsingHeader(std::format("{}##mortarSection{}", label, name).c_str(),
                                                      ImGuiTreeNodeFlags_DefaultOpen);
            ImGuiEx::Keyframeable::Record(std::string("actors.mortar.page.") + name);
            return open;
        };
        if (header(ICON_FA_BOMB " Mortar Team", "mortar"))
        {
            const auto& camera = Components::CameraManager::Get().GetActiveCamera();
            for (std::size_t i = 0; i < mortar.points.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                BeginLightRow(std::format("Point {}", i + 1).c_str());
                const auto button = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
                ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - button - ImGui::GetStyle().ItemSpacing.x);
                ImGui::DragFloat3("##mortarPoint", &mortar.points[i].x, 1.0f, 0.0f, 0.0f, "%.0f");
                ImGuiEx::Keyframeable::Record(std::format("actors.mortar.point.{}", i));
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_LOCATION_CROSSHAIRS))
                    Components::Actors::RequestPlacement(actor.id, camera->GetPosition(), camera->GetForwardVector(),
                                                         i);
                ImGuiEx::Keyframeable::Record(std::format("actors.mortar.crosshair.{}", i));
                ImGui::SetItemTooltip("To crosshair: puts it on what the camera looks at");
                ImGui::PopID();
            }
            BeginLightRow("Heading");
            auto heading = actor.yaw;
            if (ImGui::SliderFloat("##mortarHeading", &heading, -180.0f, 180.0f, "%.0f deg",
                                   ImGuiSliderFlags_AlwaysClamp))
                actor.yaw = heading;
            ImGuiEx::Keyframeable::Record("actors.mortar.heading");
            ImGui::SetItemTooltip("The way its shells fly in");
            if (!mortar.points.empty())
                actor.position = mortar.points[mortar.points.size() / 2];
        }
        if (header(ICON_FA_BOLT " Events", "events"))
            RenderEvents(actor, catalog);
    }

    void ActorsMenu::RenderPropBrowser()
    {
        constexpr auto PROP_BROWSER_POPUP = "##propBrowser";
        if (openPropBrowser)
        {
            ImGui::OpenPopup(PROP_BROWSER_POPUP);
            openPropBrowser = false;
        }
        const auto catalog = Components::Actors::GetCatalog();
        if (!catalog)
            return;
        const auto selected = Components::Actors::GetSelected();
        auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt;
        // (a new one where the camera looks; another model for one in the scene where it stands)
        const auto& camera = Components::CameraManager::Get().GetActiveCamera();
        PropPreviewRay preview{camera->GetPosition(), camera->GetForwardVector(), ActorActions::GetYawFacingCamera()};
        if (!propBrowserAdds && actor && actor->placedProp)
        {
            const auto keyed = Components::Props::WithKeyedPlacement(*actor, Components::Playback::GetTimelineTick());
            preview = {keyed.position + glm::vec3(0, 0, 8), glm::vec3(0, 0, -1), keyed.yaw};
        }
        if (!PropBrowser(PROP_BROWSER_POPUP, *catalog, propBrowsed, propBrowserFilter, propBrowserGroup,
                         propBrowserAdds ? ICON_FA_LOCATION_CROSSHAIRS " Place at the crosshair"
                                         : ICON_FA_CHECK " Use this model",
                         preview, ImVec2(ImGui::GetMainViewport()->GetCenter().x - 1.0f, 0.0f)))
            return;
        if (propBrowserAdds)
        {
            ActorActions::AddProp(propBrowsed, camera->GetPosition(), camera->GetForwardVector());
            return;
        }
        if (!actor || !actor->placedProp || actor->placedProp->model == propBrowsed)
            return;
        actor->placedProp->model = propBrowsed;
        Components::Actors::AddRecentProp(propBrowsed);
        Components::Actors::Update(*actor, false, "change the model of " + actor->name);
    }

    void ActorsMenu::RenderEffectBrowser()
    {
        constexpr auto EFFECT_BROWSER_POPUP = "##effectBrowser";
        if (openEffectBrowser)
        {
            ImGui::OpenPopup(EFFECT_BROWSER_POPUP);
            openEffectBrowser = false;
        }
        const auto catalog = Components::Actors::GetCatalog();
        if (!catalog || !EffectBrowser(EFFECT_BROWSER_POPUP, *catalog, effectBrowsed, effectBrowserFilter,
                                       effectBrowserCategory,
                                       effectBrowserAdds ? ICON_FA_LOCATION_CROSSHAIRS " Place at the crosshair"
                                                         : ICON_FA_CHECK " Use this effect"))
            return;
        if (effectBrowserAdds)
        {
            const auto& camera = Components::CameraManager::Get().GetActiveCamera();
            ActorActions::AddEffect(effectBrowsed, camera->GetPosition(), camera->GetForwardVector());
            return;
        }
        const auto selected = Components::Actors::GetSelected();
        auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt;
        if (!actor || !actor->effect || actor->effect->name == effectBrowsed)
            return;
        actor->effect->name = effectBrowsed;
        Components::Actors::AddRecentEffect(effectBrowsed);
        Components::Actors::Update(*actor, false, "change the effect of " + actor->name);
    }

    void ResetActorPanel()
    {
        openSections = true;
        if (auto& open = PreferencesConfiguration::Get().actorsAdvancedOpen; !open.empty())
        {
            open.clear();
            Configuration::Get().Write(true);
        }
    }

    namespace
    {
        // Words side by side, one of them chosen, each a click away: for what is changed often. Recorded for the
        // harness as <record>.<word>, in lower case.
        std::optional<std::size_t> Choices(const char* id, std::span<const std::string_view> words, std::size_t chosen,
                                           std::string_view record)
        {
            const auto& style = ImGui::GetStyle();
            const auto count = static_cast<float>(words.size());
            const auto width = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * (count - 1)) / count;
            std::optional<std::size_t> clicked;
            ImGui::PushID(id);
            for (std::size_t i = 0; i < words.size(); ++i)
            {
                if (i > 0)
                    ImGui::SameLine();
                if (i == chosen)
                    ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_ButtonActive]);
                if (ImGui::Button(std::string(words[i]).c_str(), ImVec2(width, 0)))
                    clicked = i;
                std::string lower(words[i]);
                std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
                ImGuiEx::Keyframeable::Record(std::format("{}.{}", record, lower));
                if (i == chosen)
                    ImGui::PopStyleColor();
            }
            ImGui::PopID();
            return clicked;
        }

        constexpr std::array<std::string_view, 3> LIVE_MODES = {"Still", "Guard", "Roam"};
        constexpr std::array<std::string_view, 2> LIVE_SIDES = {"Enemy", "Friend"};
        constexpr std::array<std::string_view, 4> LIVE_SKILLS = {"Easy", "Normal", "Hard", "Veteran"};
        constexpr std::array<std::string_view, 2> LIVE_DAMAGE = {"Gun's", "Kill"};

        // A live game's bodies: all of them now, or each a few seconds after it fell
        void RenderLiveBodies()
        {
            namespace Game = Components::LiveGame;
            if (ImGui::Button(WithKey(ICON_FA_BROOM "  Clear Bodies", "live.clearBodies").c_str(), ImVec2(-FLT_MIN, 0)))
                Commands::Run("live.clearBodies");
            ImGuiEx::Keyframeable::Record("live.clearbodies");
            ImGui::SetItemTooltip("Every body goes, from the film too");

            auto settings = Game::GetSettings();
            const auto before = settings;
            ImGui::Checkbox("Bodies go after##liveBodyTimer", &settings.bodyTimer);
            ImGuiEx::Keyframeable::Record("live.bodytimer");
            ImGui::BeginDisabled(!settings.bodyTimer);
            Label("Seconds");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderInt("##liveBodySeconds", &settings.bodySeconds, Components::LiveGameLogic::BODY_SECONDS_LEAST,
                             Components::LiveGameLogic::BODY_SECONDS_MOST, "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGuiEx::Keyframeable::Record("live.bodyseconds");
            ImGui::EndDisabled();
            if (settings != before)
                Game::SetSettings(settings);
        }
    }  // namespace

    // A row of the list: a dot for alive (nothing for the row of all of them), its name and what it does, and an x
    void ActorsMenu::RenderLiveRow(std::uint32_t id, const std::string& name, const std::string& detail,
                                   std::optional<bool> alive, bool selected, const std::function<void()>& remove)
    {
        ImGui::PushID(static_cast<int>(id));
        const auto height = ImGui::GetFrameHeight();
        const auto width = ImGui::GetContentRegionAvail().x - height - ImGui::GetStyle().ItemSpacing.x;
        const auto at = ImGui::GetCursorScreenPos();
        const auto record =
            id == Components::LiveActors::ALL_ACTORS ? std::string("live.row.all") : std::format("live.row.{}", id);
        if (ImGui::Selectable("##row", selected, 0, ImVec2(width, height)))
            Components::LiveActors::Select(id);
        ImGuiEx::Keyframeable::Record(record);

        auto* drawList = ImGui::GetWindowDrawList();
        const auto textY = at.y + (height - ImGui::GetTextLineHeight()) / 2.0f;
        const auto padding = ImGui::GetStyle().FramePadding.x;
        if (alive)
            drawList->AddCircleFilled(ImVec2(at.x + padding + 4.0f, at.y + height / 2.0f), 4.0f,
                                      *alive ? IM_COL32(63, 191, 127, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled));
        const auto detailSize = ImGui::CalcTextSize(detail.c_str());
        drawList->AddText(ImVec2(at.x + width - detailSize.x - padding, textY),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), detail.c_str());
        drawList->AddText(ImVec2(at.x + padding + 14.0f, textY), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_XMARK, ImVec2(height, height)))
            remove();
        ImGuiEx::Keyframeable::Record(record + ".remove");
        ImGui::SetItemTooltip("Remove");
        ImGui::PopID();
    }

    // A live game's bots: everything on this page, one level deep, with no menus
    // to open: a bot, its gun and what it does; the rest is done after, in the demo. Does, Side, Place and the gun, the
    // game's names, "Bots". All at the top of the list changes every bot.
    void ActorsMenu::RenderLive()
    {
        namespace Live = Components::LiveActors;
        const auto actors = Live::GetActors();
        const auto room = Live::GetRoom();
        ImGui::BeginDisabled(room > 0 && std::cmp_greater_equal(actors.size(), room));
        if (ImGui::Button(WithKey(ICON_FA_USER_PLUS "  Add Bot", "live.addActor").c_str(), ImVec2(-FLT_MIN, 0)))
            Commands::Run("live.addActor");
        ImGuiEx::Keyframeable::Record("live.add");
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Where you look. Right-click the view: there");

        Label("Skill");
        if (const auto skill = Choices("skill", LIVE_SKILLS, static_cast<std::size_t>(Live::GetSkill()), "live.skill"))
            Live::SetSkill(static_cast<Types::LiveActorSkill>(*skill));

        ImGui::TextDisabled("%zu of %d. %s hides this. %s adds, %s removes.", actors.size(), room,
                            Commands::GetShortcut("view.toggleOverlay").c_str(),
                            Commands::GetShortcut("live.addActor").c_str(),
                            Commands::GetShortcut("live.removeActor").c_str());

        const auto selected = Live::GetSelected();
        const bool all = selected == Live::ALL_ACTORS && actors.size() > 1;
        if (actors.size() > 1)
            RenderLiveRow(Live::ALL_ACTORS, std::format("All ({})", actors.size()), "", std::nullopt, all,
                          [] { Live::Clear(); });
        for (const auto& actor : actors)
        {
            const auto state = Live::GetState(actor.id);
            RenderLiveRow(actor.id, state.name.empty() ? "Joining..." : state.name,
                          std::format("{} \xC2\xB7 {}", LIVE_SIDES[static_cast<std::size_t>(actor.side)],
                                      LIVE_MODES[static_cast<std::size_t>(actor.mode)]),
                          state.alive, !all && selected == actor.id, [id = actor.id] { Live::Remove(id); });
        }

        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.25f));
        RenderLiveBodies();

        const auto chosen = selected ? std::ranges::find(actors, *selected, &Types::LiveActor::id) : actors.end();
        if (!all && chosen == actors.end())
        {
            if (!actors.empty())
                Live::Select(actors.back().id);
            return;
        }
        const auto targets = all ? actors : std::vector<Types::LiveActor>{*chosen};
        const auto& shown = targets.front();
        const auto state = Live::GetState(shown.id);
        const auto edit = [&](const std::function<void(Types::LiveActor&)>& change) {
            for (auto actor : targets)
            {
                change(actor);
                Live::Update(actor);
            }
        };

        ImGui::PushID(all ? 0 : static_cast<int>(shown.id));
        ImGui::SeparatorText(all ? "All bots" : state.name.empty() ? "Joining..." : state.name.c_str());
        if (!state.problem.empty())
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  %s", state.problem.c_str());

        Label("Does");
        if (const auto mode = Choices("does", LIVE_MODES, static_cast<std::size_t>(shown.mode), "live.does"))
            edit([&](Types::LiveActor& actor) { actor.mode = static_cast<Types::LiveActorMode>(*mode); });
        Label("Side");
        if (const auto side = Choices("side", LIVE_SIDES, static_cast<std::size_t>(shown.side), "live.side"))
            edit([&](Types::LiveActor& actor) { actor.side = static_cast<Types::LiveActorSide>(*side); });
        if (!all)
        {
            Label("Place");
            if (ImGui::Button(ICON_FA_LOCATION_DOT "  Move here"))
                Live::RequestPlace(shown.id);
            ImGuiEx::Keyframeable::Record("live.move");
            ImGui::SetItemTooltip("To where you look");
        }

        // the Weapon row of an actor of a demo, with Auto for its None
        Types::Actor gun;
        gun.weapon = shown.gun;
        gun.attachments = shown.attachments;
        gun.camo = shown.camo;
        const auto before = gun;
        const auto loadouts = Components::Loadouts::GetCatalog();
        const auto held = loadouts ? loadouts->FindVariant(state.weapon) : nullptr;
        const auto heldWeapon = held ? loadouts->FindWeapon(held->weapon) : nullptr;
        const auto autoLabel = heldWeapon ? std::format("Auto: {}", heldWeapon->displayName) : std::string("Auto");
        RenderWeapon(gun, autoLabel.c_str(), &liveWeaponFilter);
        if (gun.weapon != before.weapon || gun.attachments != before.attachments || gun.camo != before.camo)
        {
            const auto variant = Components::Actors::ResolveWeapon(gun);
            edit([&](Types::LiveActor& actor) {
                actor.gun = gun.weapon;
                actor.attachments = gun.attachments;
                actor.camo = gun.camo;
                actor.weapon = variant;
            });
        }

        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.25f));
        if (ImGui::Button(all ? ICON_FA_TRASH "  Remove all" : ICON_FA_TRASH "  Remove", ImVec2(-FLT_MIN, 0)))
        {
            if (all)
                Live::Clear();
            else
                Live::Remove(shown.id);
        }
        ImGuiEx::Keyframeable::Record("live.remove");
        ImGui::PopID();
    }

    // You, in a game you host: any
    // gun, any killstreak, Canswap and Explosive Bullet. A given gun takes the place of the one in hand and goes with
    // death, as a picked-up one does. Nothing here reaches a game someone else hosts.
    void ActorsMenu::RenderYou()
    {
        namespace Game = Components::LiveGame;
        namespace GameLogic = Components::LiveGameLogic;
        if (const auto problem = Game::GetProblem(); !problem.empty())
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  %s", problem.c_str());

        ImGui::PushID("you");
        ImGui::SeparatorText("Gun");
        RenderWeapon(youGun, "None", &youWeaponFilter);
        ImGui::BeginDisabled(youGun.weapon.empty());
        if (ImGui::Button(ICON_FA_GUN "  Give", ImVec2(-FLT_MIN, 0)))
            Game::RequestGun({Components::Actors::ResolveWeapon(youGun), youGun.camo, true});
        ImGuiEx::Keyframeable::Record("you.give");
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("In place of the gun in your hand");

        // all 17 a click each, two to a line
        ImGui::SeparatorText("Killstreak");
        const auto width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
        for (std::size_t i = 0; i < GameLogic::KILLSTREAKS.size(); ++i)
        {
            const auto& killstreak = GameLogic::KILLSTREAKS[i];
            if (i % 2 == 1)
                ImGui::SameLine();
            if (ImGui::Button(std::string(killstreak.name).c_str(), ImVec2(width, 0)))
                Game::RequestKillstreak(std::string(killstreak.type));
            ImGuiEx::Keyframeable::Record(std::format("you.killstreak.{}", killstreak.type));
        }

        ImGui::SeparatorText("You");
        auto settings = Game::GetSettings();
        const auto before = settings;
        ImGui::Checkbox("Canswap##youCanswap", &settings.canswap);
        ImGuiEx::Keyframeable::Record("you.canswap");
        ImGui::SetItemTooltip("A gun's first raise each time it comes out");
        ImGui::Checkbox("Explosive Bullet##youExplosiveBullet", &settings.explosiveBullet);
        ImGuiEx::Keyframeable::Record("you.explosive");
        ImGui::SetItemTooltip("A shot also hits the nearest enemy around where you aim");
        ImGui::BeginDisabled(!settings.explosiveBullet);
        Label("Radius");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##youRadius", &settings.radius, GameLogic::RADIUS_LEAST, GameLogic::RADIUS_MOST, "%.0f",
                               ImGuiSliderFlags_AlwaysClamp))
            settings.radius = std::round(settings.radius / 10.0f) * 10.0f;
        ImGuiEx::Keyframeable::Record("you.radius");
        Label("Damage");
        if (const auto damage = Choices("damage", LIVE_DAMAGE, settings.bulletKills ? 1 : 0, "you.damage"))
            settings.bulletKills = *damage == 1;
        ImGui::EndDisabled();
        if (settings != before)
            Game::SetSettings(settings);
        ImGui::PopID();
    }

    void ActorsMenu::Render()
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar;
        // stays behind the tool windows (Preferences, Player Loadouts...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("Actors", nullptr, flags))
        {
            if (Mod::GetGameInterface()->IsHostingLiveGame())
            {
                if (UIManager::Get().GetPage(PanelLogic::Tab::Scene) == PanelLogic::Page::You)
                    RenderYou();
                else
                    RenderLive();
            }
            else if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            {
                UI::DrawInaccessibleTabWarning();
            }
            else if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_Actors))
            {
                ImGui::TextWrapped("Actors aren't supported for this game yet.");
            }
            else if (!Components::Actors::GetCatalog())
            {
                ImGui::TextWrapped(ICON_FA_PERSON "  Finding the characters of this map...");
            }
            else
            {
                EndActorEditStepWhenIdle();
                if (reloadPreview && reloadPreview->frame + 1 < ImGui::GetFrameCount())
                {
                    Components::ReloadProfiles::ChangeOverride(reloadPreview->animation, reloadPreview->entry,
                                                               "move marker of " + reloadPreview->animation);
                    reloadPreview.reset();
                }
                // (the content pack the characters come from is chosen in Body's More characters...)
                RenderList();
                RenderInspector();
                RenderEffectBrowser();
                RenderPropBrowser();
            }
        }
        ImGui::End();
    }

    void ActorsMenu::Release()
    {
    }
}  // namespace IWXMVM::UI
