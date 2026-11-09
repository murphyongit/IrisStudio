#include "StdInclude.hpp"
#include "GameView.hpp"

#include "UI/Theme.hpp"

#include "UI/ImGuiEx/ImGuiExtensions.hpp"
#include "Mod.hpp"
#include "D3D9.hpp"
#include "UI/UIManager.hpp"
#include "Utilities/PathUtils.hpp"
#include "Utilities/MathUtils.hpp"
#include "Events.hpp"
#include "Input.hpp"
#include "Components/Rewinding.hpp"
#include "Components/CaptureManager.hpp"
#include "Graphics/Graphics.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Components/Playback.hpp"
#include "UI/Commands.hpp"
#include "UI/Toasts.hpp"
#include "UI/Layout.hpp"
#include "UI/AreaMap.hpp"
#include "UI/ViewMenu.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "Components/Actors.hpp"
#include "Components/LiveActors.hpp"
#include "Components/QualityLogic.hpp"

#include "imgui_internal.h"

namespace IWXMVM::UI
{
    ImVec2 ClampImage(ImVec2 window)
    {
        float aspectRatio = ImGui::GetIO().DisplaySize.x / ImGui::GetIO().DisplaySize.y;

        if (window.x / window.y > aspectRatio)
        {
            // If too wide, adjust width
            window.x = window.y * aspectRatio;
        }
        else if (window.x / window.y < aspectRatio)
        {
            // If too tall, adjust height
            window.y = window.x / aspectRatio;
        }

        return ImVec2(window.x, window.y);
    }

    bool wasGizmoButtonClickedThisFrame = false;

    // The gizmo's modes at the top left of the picture, for campath nodes and actors alike. Over the image, so in its
    // window, which takes no mouse input (see Render): they are drawn and hit-tested by hand, and are part of the
    // overlay, so the view's picking and gizmo leave their clicks.
    void GameView::DrawGizmoControls(bool viewHovered)
    {
        auto& graphicsManager = GFX::GraphicsManager::Get();
        if (!graphicsManager.IsGizmoShown())
            return;

        // an actor walking a path faces along it, so its gizmo doesn't turn it (as on the Actors tab)
        bool canTurn = true;
        if (graphicsManager.GetGizmoTarget() == GFX::GizmoTarget::Actor)
        {
            const auto selected = Components::Actors::GetSelected();
            const auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt;
            canTurn = !actor || actor->movement != Types::ActorMovement::Path;
        }

        struct Button
        {
            const char* icon;
            GFX::GizmoMode mode;
            const char* tooltip;
            const char* name;  // for the harness
            bool disabled = false;
        };
        const std::array<Button, 3> buttons = {{
            {ICON_FA_MAXIMIZE, GFX::GizmoMode::TranslateLocal, "Move along its own axes", "view.gizmo.local"},
            {ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, GFX::GizmoMode::TranslateGlobal, "Move along the world's axes",
             "view.gizmo.world"},
            {ICON_FA_ROTATE, GFX::GizmoMode::Rotate, canTurn ? "Turn" : "An actor walking a path faces along it",
             "view.gizmo.rotate", !canTurn},
        }};

        const auto& style = ImGui::GetStyle();
        const auto size = ImGui::GetFontSize() * 2;
        const auto spacing = size / 10;
        const auto offset = ImGui::GetFontSize() * 0.833f;
        auto drawList = ImGui::GetWindowDrawList();
        auto min = viewportPosition + ImVec2(offset, offset);
        for (const auto& button : buttons)
        {
            const auto max = min + ImVec2(size, size);
            const bool hovered = viewHovered && ImGui::IsMouseHoveringRect(min, max);
            const bool current = graphicsManager.GetGizmoMode() == button.mode;
            if (hovered && !button.disabled && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                graphicsManager.SetGizmoMode(button.mode);
                wasGizmoButtonClickedThisFrame = true;
            }

            // as ImGui's buttons look, the current mode's pressed
            const auto color = button.disabled                                        ? ImGuiCol_Button
                               : hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) ? ImGuiCol_ButtonActive
                               : hovered                                              ? ImGuiCol_ButtonHovered
                               : current                                              ? ImGuiCol_ButtonActive
                                                                                      : ImGuiCol_Button;
            const auto alpha = button.disabled ? style.DisabledAlpha : 1.0f;
            drawList->AddRectFilled(min, max, ImGui::GetColorU32(color, alpha), style.FrameRounding);
            const auto iconSize = ImGui::CalcTextSize(button.icon);
            drawList->AddText(ImFloor(min + (ImVec2(size, size) - iconSize) * 0.5f),
                              ImGui::GetColorU32(ImGuiCol_Text, alpha), button.icon);
            if (hovered)
                ImGui::SetTooltip("%s", button.tooltip);

            overlay.emplace_back(min, max);
            ImGuiEx::Keyframeable::RecordRect(button.name, min, max);
            min.x = max.x + spacing;
        }
    }

    // A click on nothing in the view (not in the panels around it, nor on the gizmo's buttons, the strip or a
    // message, where it was pressed either) puts the gizmo away
    void GameView::PutGizmoAwayOnClick()
    {
        auto& graphicsManager = GFX::GraphicsManager::Get();
        if (graphicsManager.IsGizmoShown() && Input::KeyUp(ImGuiKey_MouseLeft) &&
            !graphicsManager.WasObjectHoveredThisFrame() && !graphicsManager.WasObjectDraggedThisFrame() &&
            !wasGizmoButtonClickedThisFrame && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
            !IsMouseOverOverlay() && !IsOverOverlay(ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left]))
        {
            graphicsManager.ResetSelection();
        }
    }

    namespace
    {
        // a key (or keys) and what it does, in the strip
        struct Hint
        {
            std::string keys;
            std::string_view what;
        };

        // every key of a command, "Enter/Esc"
        std::string AllShortcuts(std::string_view id)
        {
            std::string text;
            for (const auto chord : Commands::GetChords(id))
                text += (text.empty() ? "" : "/") + Commands::FormatChord(chord);
            return text;
        }

        // "WASD" for single letters, else the keys with slashes
        std::string JoinShortcuts(std::initializer_list<std::string_view> ids)
        {
            std::vector<std::string> keys;
            bool letters = true;
            for (const auto id : ids)
            {
                keys.push_back(Commands::GetShortcut(id));
                letters = letters && keys.back().size() == 1;
            }
            std::string text;
            for (const auto& key : keys)
                text += (text.empty() || letters ? "" : "/") + key;
            return text;
        }
    }  // namespace

    // The mode the view is in and three or four keys for it, along the bottom of the view: flying, placing path
    // points, recording, with time frozen; else how to fly (or orbit). Returns the strip's top.
    float GameView::DrawModeStrip(ImVec2 min, ImVec2 max, bool viewHovered)
    {
        struct Mode
        {
            std::string_view name;
            ImU32 color;
        };
        std::vector<Mode> modes;
        std::vector<Hint> hints;

        const auto shortcut = [](std::string_view id) { return Commands::GetShortcut(id); };
        const auto cameraMode = Components::CameraManager::Get().GetActiveCamera()->GetMode();
        // (Freeze time, or bullet time while the playhead is in a span of it)
        if (Components::Playback::IsGameFrozen() || Components::Playback::GetCurrentFrozenSpan())
            modes.push_back({"FROZEN", IM_COL32(60, 170, 200, 255)});
        if (Components::CaptureManager::Get().IsCapturing() && !Components::CaptureManager::Get().IsRestarting())
        {
            modes.push_back({"RECORDING", IM_COL32(210, 50, 50, 255)});
            hints = {{shortcut("capture.stop"), "stop"}, {shortcut("view.toggleOverlay"), "hide the editor"}};
        }
        else if (UIManager::Get().IsFlying())
        {
            modes.push_back({"FLY", IM_COL32(60, 120, 230, 255)});
            hints = {{JoinShortcuts({"fly.forward", "fly.left", "fly.backward", "fly.right"}), "move"},
                     {JoinShortcuts({"fly.down", "fly.up"}), "down/up"},
                     {JoinShortcuts({"fly.faster", "fly.slower"}), "faster/slower"},
                     {"Wheel", "FOV"}};
            if (cameraMode != Components::Camera::Mode::Bone)
                hints.push_back({shortcut("campath.addNode"), "add node"});
            hints.push_back(flyingWhileHeld ? Hint{"Release right mouse", "stop"}
                                            : Hint{shortcut("fly.toggle") + "/" + shortcut("fly.stop"), "stop"});
        }
        else if (GFX::GraphicsManager::Get().IsPlacingPathPoints())
        {
            modes.push_back({"PLACING", IM_COL32(60, 170, 90, 255)});
            hints = {{"Click", "add a point"},
                     {shortcut("placing.removeLast"), "remove it"},
                     {AllShortcuts("placing.finish"), "done"}};
        }
        else if (cameraMode == Components::Camera::Mode::Orbit)
        {
            hints = {{"Right drag", "orbit"}, {"Shift+right drag", "pan"}, {"Wheel", "zoom"}};
        }
        else if (UIManager::Get().IsControllableCameraModeSelected())
        {
            hints = {{shortcut("fly.toggle") + " or hold right mouse", "fly"}};
            if (cameraMode != Components::Camera::Mode::Bone)
                hints.push_back({shortcut("campath.addNode"), "add node"});
        }
        if (!PreferencesConfiguration::Get().showKeybindHints)
            hints.clear();
        std::erase_if(hints, [](const Hint& hint) { return hint.keys.empty(); });

        const auto fontSize = ImGui::GetFontSize();
        const auto padding = ImVec2(fontSize * 0.5f, fontSize * 0.3f);
        const auto height = ImGui::GetTextLineHeight() + padding.y * 2;
        const auto top = max.y - height;
        auto drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(ImVec2(min.x, top), max, IM_COL32(15, 15, 15, 170));

        // "? shortcuts" on the right, which opens them
        const auto sheetKey = shortcut("help.shortcuts");
        const auto sheetLabel = sheetKey.empty() ? std::string("Shortcuts") : sheetKey + "  all shortcuts";
        const auto sheetSize = ImGui::CalcTextSize(sheetLabel.c_str());
        const auto sheetPosition = ImVec2(max.x - padding.x - sheetSize.x, top + padding.y);
        const bool sheetHovered = viewHovered && ImGui::IsMouseHoveringRect(sheetPosition, sheetPosition + sheetSize);
        if (sheetHovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (sheetHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            Commands::Run("help.shortcuts");
        drawList->AddText(sheetPosition, ImGui::GetColorU32(sheetHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                          sheetLabel.c_str());

        auto x = min.x + padding.x;
        const auto right = sheetPosition.x - fontSize;
        ImGui::PushFont(UIManager::Get().GetBoldFont());
        for (const auto& mode : modes)
        {
            const auto size = ImGui::CalcTextSize(mode.name.data(), mode.name.data() + mode.name.size());
            const auto badgeMin = ImVec2(x, top + padding.y * 0.5f);
            const auto badgeMax = ImVec2(x + size.x + padding.x * 2, max.y - padding.y * 0.5f);
            drawList->AddRectFilled(badgeMin, badgeMax, mode.color, 4.0f);
            drawList->AddText(ImVec2(x + padding.x, top + padding.y), IM_COL32_WHITE, mode.name.data(),
                              mode.name.data() + mode.name.size());
            x = badgeMax.x + padding.x;
        }
        ImGui::PopFont();

        for (const auto& hint : hints)
        {
            const auto keysSize = ImGui::CalcTextSize(hint.keys.c_str());
            const auto whatSize = ImGui::CalcTextSize(hint.what.data(), hint.what.data() + hint.what.size());
            const auto width = keysSize.x + padding.x + fontSize * 0.3f + whatSize.x;
            // (what doesn't fit is left out, rather than running off the view)
            if (x + width > right)
                break;
            const auto boxMin = ImVec2(x, top + padding.y * 0.5f);
            const auto boxMax = ImVec2(x + keysSize.x + padding.x, max.y - padding.y * 0.5f);
            drawList->AddRectFilled(boxMin, boxMax, IM_COL32(70, 70, 70, 255), 4.0f);
            drawList->AddText(ImVec2(x + padding.x / 2, top + padding.y), ImGui::GetColorU32(ImGuiCol_Text),
                              hint.keys.c_str());
            drawList->AddText(ImVec2(boxMax.x + fontSize * 0.3f, top + padding.y), ImGui::GetColorU32(ImGuiCol_Text),
                              hint.what.data(), hint.what.data() + hint.what.size());
            x += width + fontSize;
        }
        return top;
    }

    bool GameView::IsMouseOverOverlay() const
    {
        return IsOverOverlay(ImGui::GetIO().MousePos);
    }

    bool GameView::IsOverOverlay(ImVec2 point) const
    {
        return std::ranges::any_of(overlay, [&](const std::pair<ImVec2, ImVec2>& rect) {
            return point.x >= rect.first.x && point.y >= rect.first.y && point.x < rect.second.x &&
                   point.y < rect.second.y;
        });
    }

    void GameView::LockMouse()
    {
        auto windowPosition = UIManager::Get().GetWindowPosition();

        POINT cursorPosition;
        GetCursorPos(&cursorPosition);

        ImVec2 viewportCenter = ImVec2(glm::floor(windowPosition.x + GetPosition().x + GetSize().x / 2),
                                       glm::floor(windowPosition.y + GetPosition().y + GetSize().y / 2));
        ImGui::GetIO().MousePosPrev += ImVec2(viewportCenter.x - cursorPosition.x, viewportCenter.y - cursorPosition.y);
        SetCursorPos(static_cast<int32_t>(viewportCenter.x), static_cast<int32_t>(viewportCenter.y));
    }

    void GameView::Initialize()
    {
        // (Layout places it)
        Events::RegisterListener(EventType::OnCameraChanged, [&]() {
            if (UIManager::Get().IsControllableCameraModeSelected())
            {
                SetHasFocus(false);
                flyingWhileHeld = false;
            }
        });

        RegisterCommands();
    }

    void GameView::RegisterCommands()
    {
        AreaMap::RegisterCommands();

        using Commands::Bit;
        using Commands::Context;

        const auto notInDemo = []() -> std::optional<std::string> {
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return "Load a demo first";
            return std::nullopt;
        };

        Commands::Register({
            .id = "fly.toggle",
            .label = "Fly the free camera",
            .category = "Fly",
            .defaults = {ImGuiKey_F},
            .contexts = Bit(Context::Fly) | Bit(Context::Global),
            .unavailable =
                [notInDemo]() -> std::optional<std::string> {
                if (auto reason = notInDemo())
                    return reason;
                if (!UIManager::Get().IsControllableCameraModeSelected())
                    return "Only the free and bone cameras fly";
                return std::nullopt;
            },
            .run =
                [this] {
                    // while the right mouse button flies, F keeps flying after it is let go
                    if (flyingWhileHeld)
                    {
                        flyingWhileHeld = false;
                        return;
                    }
                    SetHasFocus(!HasFocus());
                    if (!HasFocus())
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
                },
            .keywords = "free camera move mouse look",
        });
        Commands::Register({
            .id = "fly.stop",
            .label = "Stop flying",
            .category = "Fly",
            .defaults = {ImGuiKey_Escape},
            .contexts = Bit(Context::Fly),
            .run =
                [this] {
                    SetHasFocus(false);
                    flyingWhileHeld = false;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
                },
        });

        // Held while flying; in the orbit camera while the right mouse button orbits over the view. Modifiers only
        // change the speed, so Ctrl+D is flying right slowly.
        const auto canMove = []() -> std::optional<std::string> {
            const auto mode = Components::CameraManager::Get().GetActiveCamera()->GetMode();
            if (UIManager::Get().IsFlying() ||
                (mode == Components::Camera::Mode::Orbit && ImGui::IsMouseDown(ImGuiMouseButton_Right)))
                return std::nullopt;
            return "Fly first: F, or hold the right mouse button in the view";
        };
        const std::array<std::tuple<std::string_view, std::string_view, ImGuiKey>, 9> moves = {{
            {"fly.forward", "Forward", ImGuiKey_W},
            {"fly.backward", "Backward", ImGuiKey_S},
            {"fly.left", "Left", ImGuiKey_A},
            {"fly.right", "Right", ImGuiKey_D},
            {"fly.up", "Up", ImGuiKey_E},
            {"fly.down", "Down", ImGuiKey_Q},
            {"fly.faster", "Faster (hold)", ImGuiKey_LeftShift},
            {"fly.slower", "Slower (hold)", ImGuiKey_LeftCtrl},
            {"fly.reset", "Level the camera, FOV 90", ImGuiKey_MouseMiddle},
        }};
        for (const auto& [id, label, key] : moves)
        {
            Commands::Register({
                .id = id,
                .label = label,
                .category = "Fly",
                .defaults = {key},
                .contexts = Bit(Context::Fly) | Bit(Context::Viewport),
                .held = true,
                .unavailable = canMove,
            });
        }

        const std::array<std::tuple<std::string_view, std::string_view, ImGuiKey, Components::Camera::Mode>, 6> modes = {{
            {"camera.firstPerson", "First person camera", ImGuiKey_1, Components::Camera::Mode::FirstPerson},
            {"camera.thirdPerson", "Third person camera", ImGuiKey_2, Components::Camera::Mode::ThirdPerson},
            {"camera.free", "Free camera", ImGuiKey_3, Components::Camera::Mode::Free},
            {"camera.orbit", "Orbit camera", ImGuiKey_4, Components::Camera::Mode::Orbit},
            {"camera.dolly", "Dolly camera (campath)", ImGuiKey_5, Components::Camera::Mode::Dolly},
            {"camera.bone", "Bone camera", ImGuiKey_6, Components::Camera::Mode::Bone},
        }};
        for (const auto& [id, label, key, mode] : modes)
        {
            Commands::Register({
                .id = id,
                .label = label,
                .category = "Camera",
                .defaults = {key},
                .contexts = Bit(Context::Fly) | Bit(Context::Global),
                .unavailable = notInDemo,
                .run = [mode] { Components::CameraManager::Get().SetActiveCamera(mode); },
            });
        }

        Commands::Register({
            .id = "orbit.rotate",
            .label = "Orbit (hold; with Shift, pan)",
            .category = "Camera",
            .defaults = {ImGuiKey_MouseRight},
            .held = true,
            .unavailable =
                [] {
                    return Components::CameraManager::Get().GetActiveCamera()->GetMode() == Components::Camera::Mode::Orbit
                               ? std::nullopt
                               : std::optional<std::string>("Only in the orbit camera");
                },
        });
        Commands::Register({
            .id = "orbit.reset",
            .label = "Reset the orbit camera",
            .category = "Camera",
            .defaults = {ImGuiKey_F4},
            .held = true,
            .unavailable =
                [] {
                    return Components::CameraManager::Get().GetActiveCamera()->GetMode() == Components::Camera::Mode::Orbit
                               ? std::nullopt
                               : std::optional<std::string>("Only in the orbit camera");
                },
        });
    }

    void GameView::DrawTopBar()
    {
        auto& cameraManager = Components::CameraManager::Get();
        auto& currentCamera = cameraManager.GetActiveCamera();

        const auto PADDING = 10;
        ImGui::SetCursorPos(ImVec2(PADDING, PADDING));

        // (not yet while a capture holds at its start behind the loading screen)
        const auto& capture = Components::CaptureManager::Get();
        if (capture.IsCapturing() && !capture.IsRestarting())
        {
            ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(PADDING, PADDING * 0.5f));
            ImGui::PushFont(UIManager::Get().GetBoldFont());
            ImGui::TextColored(Theme::Problem(), ICON_FA_CIRCLE " Recording...");
            ImGui::PopFont();
            return;
        }
        
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);

        if (ImGui::BeginCombo("##gameViewCameraCombo",
                              cameraManager.GetCameraModeLabel(currentCamera->GetMode()).data()))
        {
            for (auto cameraMode : cameraManager.GetCameraModes())
            {
                bool isSelected = currentCamera->GetMode() == cameraMode;
                if (ImGui::Selectable(cameraManager.GetCameraModeLabel(cameraMode).data(),
                                      currentCamera->GetMode() == cameraMode))
                {
                    cameraManager.SetActiveCamera(cameraMode);
                }

                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    // Safe frames over the image (View > Show safe frames): what stays clear of a screen's edges, 90% of it for action
    // and 80% for titles, and its middle; only the editor shows them, not the recording
    void DrawSafeFrames(ImVec2 min, ImVec2 max)
    {
        auto drawList = ImGui::GetWindowDrawList();
        const auto size = max - min;
        for (const auto [share, alpha] : {std::pair(0.9f, 120), std::pair(0.8f, 90)})
        {
            const auto margin = size * (1.0f - share) * 0.5f;
            drawList->AddRect(ImFloor(min + margin), ImFloor(max - margin), IM_COL32(255, 255, 255, alpha));
        }
        const auto middle = ImFloor((min + max) * 0.5f);
        const auto arm = ImGui::GetFontSize() * 0.6f;
        drawList->AddLine(middle - ImVec2(arm, 0), middle + ImVec2(arm + 1, 0), IM_COL32(255, 255, 255, 120));
        drawList->AddLine(middle - ImVec2(0, arm), middle + ImVec2(0, arm + 1), IM_COL32(255, 255, 255, 120));
    }

    void GameView::DrawRenderSizeSwitch()
    {
        auto& captureManager = Components::CaptureManager::Get();
        const auto label = captureManager.GetRestartLabel();
        const auto screen = captureManager.GetRestartScreen();
        captureManager.OnRestartScreenDrawn();

        const auto position = ImGui::GetCursorScreenPos();
        const auto drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(position, position + textureSize, IM_COL32(18, 18, 18, 255));
        ImGui::Dummy(textureSize);

        ImGui::PushFont(UIManager::Get().GetBoldFont());
        const auto y = (textureSize.y - ImGui::GetTextLineHeight()) / 2.0f;
        ImGui::SetCursorPos(ImVec2((textureSize.x - ImGui::CalcTextSize(label.c_str()).x) / 2.0f, y));
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopFont();

        // under it, in a row: the ring, what is waited for and the time once it's a while
        constexpr float TAU = 6.2831853f;
        const auto fontSize = ImGui::GetFontSize();
        const auto ringSide = fontSize * 1.25f;
        const auto gap = fontSize * 0.5f;
        const auto rowY = y + fontSize * 1.8f;
        const auto lineWidth = ImGui::CalcTextSize(screen.line.c_str()).x;
        const auto timeWidth = screen.time.empty() ? 0.0f : ImGui::CalcTextSize(screen.time.c_str()).x + gap * 2.0f;
        const auto rowX = (textureSize.x - ringSide - gap - lineWidth - timeWidth) / 2.0f;
        const auto textY = rowY + (ringSide - ImGui::GetTextLineHeight()) / 2.0f;

        const auto& palette = Theme::Current();
        const ImVec2 ring = position + ImVec2(rowX + ringSide / 2.0f, rowY + ringSide / 2.0f);
        const auto radius = ringSide * 9.0f / 24.0f;
        const auto thickness = ringSide * 3.0f / 24.0f;
        const auto start = Components::RestartLogic::GetRingTurn(GetTickCount64()) * TAU - TAU / 4.0f;
        drawList->AddCircle(ring, radius, Theme::U32(palette.raised), 32, thickness);
        drawList->PathArcTo(ring, radius, start, start + Components::RestartLogic::RING_ARC * TAU, 16);
        drawList->PathStroke(Theme::U32(palette.accent), ImDrawFlags_None, thickness);
        D3D9::SetRestartRing(ring.x, ring.y, radius, thickness, palette.raised.rgb, palette.accent.rgb);

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::SetCursorPos(ImVec2(rowX + ringSide + gap, textY));
        ImGui::TextUnformatted(screen.line.c_str());
        if (!screen.time.empty())
        {
            ImGui::SetCursorPos(ImVec2(rowX + ringSide + gap + lineWidth + gap * 2.0f, textY));
            ImGui::TextUnformatted(screen.time.c_str());
        }
        if (!screen.note.empty())
        {
            const auto note = screen.note.data();
            const auto noteEnd = note + screen.note.size();
            ImGui::SetCursorPos(
                ImVec2((textureSize.x - ImGui::CalcTextSize(note, noteEnd).x) / 2.0f, rowY + ringSide + gap));
            ImGui::TextUnformatted(note, noteEnd);
        }
        ImGui::PopStyleColor();
    }

    // A right-click on the picture let go at once without moving the mouse opens the view's menu for what is under it,
    // in the free and orbit cameras; held, moved or with a key pressed meanwhile, it flies the free camera (or orbits).
    // Returns whether a press became a hold this frame: at once where there is no menu (the bone camera, off the
    // picture), else once it is one, so a click doesn't fly (which holds the mouse in the middle, turning the camera).
    bool GameView::HandleRightClick(bool pressed)
    {
        constexpr double TAP_SECONDS = 0.3;
        constexpr float TAP_PIXELS = 6.0f;
        const auto mode = Components::CameraManager::Get().GetActiveCamera()->GetMode();
        const auto mouse = ImGui::GetIO().MousePos;
        if (pressed)
        {
            const bool overPicture = mouse.x >= viewportPosition.x && mouse.y >= viewportPosition.y &&
                                     mouse.x < viewportPosition.x + viewportSize.x &&
                                     mouse.y < viewportPosition.y + viewportSize.y && !IsMouseOverOverlay();
            // (not while flying already, which has the mouse)
            const bool menu = overPicture && !UIManager::Get().IsFlying() &&
                              ((mode == Components::Camera::Mode::Free || mode == Components::Camera::Mode::Orbit) &&
                                   Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo ||
                               Mod::GetGameInterface()->IsHostingLiveGame());
            const auto [origin, direction] = GFX::GetViewRay({mouse.x, mouse.y});
            const auto gizmo = GFX::GetGizmoState();
            rightPress = RightPress{ImGui::GetTime(), mouse, 0.0f,  gizmo.hoveredActor, origin,
                                    direction,        menu,  !menu, gizmo.hoveredPoint, gizmo.hoveredLine};
            return !menu;
        }
        if (!rightPress)
            return false;
        const auto delta = ImGui::GetIO().MouseDelta;
        rightPress->moved += std::sqrt(delta.x * delta.x + delta.y * delta.y);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            // (in a live game no menu: a right-click is an actor there)
            if (rightPress->menu && !rightPress->held && Mod::GetGameInterface()->IsHostingLiveGame())
                Components::LiveActors::AddNew(
                    std::array{(rightPress->position.x - viewportPosition.x) / viewportSize.x * 2.0f - 1.0f,
                               1.0f - (rightPress->position.y - viewportPosition.y) / viewportSize.y * 2.0f});
            else if (rightPress->menu && !rightPress->held)
                ViewMenu::Open(rightPress->actor, {rightPress->position.x, rightPress->position.y}, rightPress->origin,
                               rightPress->direction, rightPress->point, rightPress->line);
            rightPress.reset();
            return false;
        }
        if (rightPress->held)
            return false;
        // a key (W to fly forward, Shift to go faster...) makes it a hold too
        bool keyDown = false;
        for (auto key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END && !keyDown;
             key = static_cast<ImGuiKey>(key + 1))
            keyDown = !ImGui::IsMouseKey(key) && ImGui::IsKeyDown(key);
        rightPress->held =
            ImGui::GetTime() - rightPress->time >= TAP_SECONDS || rightPress->moved >= TAP_PIXELS || keyDown;
        return rightPress->held;
    }

    void GameView::Render()
    {
        wasGizmoButtonClickedThisFrame = false;

        // where the layout puts it (its splitters size it)
        const auto rect = Layout::GetView();
        SetPosition(rect.Min);
        SetSize(rect.GetSize());
        ImGui::SetNextWindowPos(GetPosition());
        ImGui::SetNextWindowSize(GetSize());

        if (HasFocus() && UIManager::Get().IsControllableCameraModeSelected())
            LockMouse();

        const auto windowPadding = ImGui::GetStyle().WindowPadding;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize;
        // stays behind the tool windows (Preferences, Player Loadouts...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("GameView", NULL, flags);

        // F and Escape (the fly commands) give the free camera the keyboard and mouse and take them back; holding the
        // right mouse button in the view flies until it is let go. The other cameras have them while the view has
        // the focus, which the right mouse button gives it too, so it orbits at once.
        const auto isGameFocused = D3D9::FindWindowHandle() == GetForegroundWindow();
        const bool viewHovered = ImGui::IsWindowHovered();
        const bool rightClickedView = viewHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        const bool rightHeld = HandleRightClick(rightClickedView);
        if (!isGameFocused || (flyingWhileHeld && !ImGui::IsMouseDown(ImGuiMouseButton_Right)))
        {
            if (HasFocus() && UIManager::Get().IsControllableCameraModeSelected())
                ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
            SetHasFocus(false);
            flyingWhileHeld = false;
        }
        else if (UIManager::Get().IsControllableCameraModeSelected())
        {
            // (a press flies once it is held; a click opens the view's menu instead)
            if (rightHeld && !HasFocus() && Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
            {
                SetHasFocus(true);
                flyingWhileHeld = true;
            }
        }
        else
        {
            // (until something else is clicked)
            if (rightClickedView)
                ImGui::SetWindowFocus();
            SetHasFocus(ImGui::IsWindowFocused());
        }

        auto topBarHeight = 0.0f;
        if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
        {
            DrawTopBar();
            topBarHeight = ImGui::GetItemRectSize().y + 30;
        }

        auto viewportSize = ImGui::GetContentRegionMax();
        viewportSize.y -= topBarHeight;

        auto newTextureSize = ClampImage(viewportSize);
        if (textureSize.x != newTextureSize.x || textureSize.y != newTextureSize.y)
        {
            textureSize = newTextureSize;
            D3D9::CreateTexture(texture, textureSize);
        }

        // the game shows black frames while it restarts its renderer (a capture's render size, Shadow quality), and
        // ReShade then loads its effects, see DrawRenderSizeSwitch
        const auto& captureManager = Components::CaptureManager::Get();
        const auto switchingRenderSize = captureManager.IsRestarting();

        // only update the game view if we're not rewinding to reduce "glitchiness"
        // otherwise, for a brief second, you'd see the first frame of the demo
        if (!Components::Rewinding::IsRewinding() && !switchingRenderSize)
        {
            if (!D3D9::CaptureBackBuffer(texture))
            {
                throw std::exception("Failed to capture game view");
            }
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

        ImGui::SetCursorPos(
            ImVec2((viewportSize.x - textureSize.x) / 2.0f, (viewportSize.y - textureSize.y) / 2.0f + topBarHeight));
        ImGui::BeginChildFrame(ImGui::GetID("gameViewportInternal"), textureSize,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMouseInputs);
        this->viewportPosition = ImGui::GetCurrentWindow()->DC.CursorPos;
        this->viewportSize = textureSize;
        D3D9::SetGameViewRect({static_cast<LONG>(viewportPosition.x), static_cast<LONG>(viewportPosition.y),
                               static_cast<LONG>(viewportPosition.x + textureSize.x),
                               static_cast<LONG>(viewportPosition.y + textureSize.y)});

        if (switchingRenderSize)
        {
            DrawRenderSizeSwitch();
        }
        else if (captureManager.IsCapturing() && captureManager.MultiPassEnabled())
        {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.05f);
            ImGui::Image((void*)texture, textureSize);
            ImGui::PopStyleVar();

            const char* label = "PREVIEW IS DIMMED DURING MULTIPASS RECORDING";
            ImGui::PushFont(UIManager::Get().GetBoldFont());
            ImGui::SetCursorPos(ImVec2((textureSize.x - ImGui::CalcTextSize(label).x) / 2.0f,
                                       (textureSize.y - ImGui::GetTextLineHeight()) / 2.0f));
            ImGui::Text(label);
            ImGui::PopFont();
        }
        else
        {
            ImGui::Image((void*)texture, textureSize); 
        }

        Events::Invoke(EventType::OnRenderGameView);

        // Over the image, so in its window, which takes no mouse input: the mouse over it is this window's, so what
        // can be clicked there (the gizmo's buttons, the messages' Undo, "? all shortcuts") is hit-tested by hand
        overlay.clear();
        if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
        {
            if (PreferencesConfiguration::Get().showSafeFrames && !switchingRenderSize)
                DrawSafeFrames(viewportPosition, viewportPosition + this->viewportSize);
            DrawGizmoControls(viewHovered);
        }

        auto messagesBottom = viewportPosition.y + this->viewportSize.y;
        if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
        {
            messagesBottom = DrawModeStrip(viewportPosition, viewportPosition + this->viewportSize, viewHovered);
            overlay.emplace_back(ImVec2(viewportPosition.x, messagesBottom), viewportPosition + this->viewportSize);
        }
        // the note when the game's effects run out: some of what plays didn't spawn
        static Components::QualityLogic::EffectsNote effectsNote;
        if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo &&
            Components::QualityLogic::ShouldNoteRefusedEffects(
                effectsNote, Mod::GetGameInterface()->CountRefusedEffects(), GetTickCount64()))
            Toasts::Show(std::format("Some effects didn't play at {}: the game's 1,024 are all in use",
                                     Toasts::FormatTime(Components::Playback::GetTimelineTick())));
        const auto messages = Toasts::Draw(
            viewportPosition,
            ImVec2(viewportPosition.x + this->viewportSize.x, messagesBottom - ImGui::GetFontSize() * 0.5f),
            viewHovered);
        overlay.insert(overlay.end(), messages.begin(), messages.end());
        if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
            PutGizmoAwayOnClick();
        // while path points are placed: by the mouse, how long the stretch to it takes, and what the clicks do
        if (const auto preview = GFX::GraphicsManager::Get().GetPlacingPreview())
        {
            auto* drawList = ImGui::GetWindowDrawList();
            const auto chip = [&](ImVec2 at, const std::string& text) {
                const auto size = ImGui::CalcTextSize(text.c_str());
                const auto padding = ImVec2(6, 3);
                drawList->AddRectFilled(at, ImVec2(at.x + size.x + padding.x * 2, at.y + size.y + padding.y * 2),
                                        IM_COL32(0, 0, 0, 200), 2.0f);
                drawList->AddText(ImVec2(at.x + padding.x, at.y + padding.y), IM_COL32_WHITE, text.c_str());
            };
            // (below the mouse, or above it near the view's bottom; left of it near its right edge)
            const auto below = preview->mouse.y + ImGui::GetFontSize() * 4.0f < messagesBottom;
            const auto widest =
                ImGui::CalcTextSize("Click: point · Double-click: last · Esc: done · 1 2 3: Walk Run Sprint").x + 12;
            const auto right = viewportPosition.x + this->viewportSize.x;
            const auto at =
                ImVec2(preview->mouse.x + 18 + widest > right ? preview->mouse.x - 18 - widest : preview->mouse.x + 18,
                       below ? preview->mouse.y + 14 : preview->mouse.y - ImGui::GetFontSize() * 3.6f);
            if (!preview->gait.empty())
                chip(at, std::format("+{:.1f} s · {}", preview->seconds, preview->gait));
            chip(ImVec2(at.x, at.y + ImGui::GetFontSize() * 1.6f),
                 "Click: point · Double-click: last · Esc: done · 1 2 3: Walk Run Sprint");
        }
        ImGui::EndChildFrame();

        ImGui::PopStyleVar();

        // (padded as windows are, not as the view)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, windowPadding);
        ViewMenu::Draw();
        AreaMap::Draw();
        ImGui::PopStyleVar();

        ImGui::End();

        ImGui::PopStyleVar();
    }

    void GameView::Release()
    {
        if (texture != NULL)
        {
            textureSize = ImVec2(0, 0);

            texture->Release();
            texture = NULL;
        }
    }
}  // namespace IWXMVM::UI
