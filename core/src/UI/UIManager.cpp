#include "StdInclude.hpp"
#include "UIManager.hpp"

#include "UI/Theme.hpp"

#include "UIComponent.hpp"
#include "Utilities/HookManager.hpp"
#include "Mod.hpp"
#include "Version.hpp"
#include "Events.hpp"
#include "Resources.hpp"
#include "Input.hpp"
#include "Components/CameraManager.hpp"
#include "Utilities/MathUtils.hpp"
#include "UI/TaskbarProgress.hpp"
#include "UI/Toasts.hpp"
#include "Components/UndoHistory.hpp"
#include "Components/KeyframeManager.hpp"
#include "UI/TimelineSelection.hpp"
#include "UI/Layout.hpp"
#include "UI/GettingStarted.hpp"
#include "UI/ActorActions.hpp"
#include "UI/InputRouting.hpp"
#include "Graphics/Graphics.hpp"
#include "Configuration/PreferencesConfiguration.hpp"

#include <imgui_internal.h>

namespace IWXMVM::UI
{
    void UIManager::ShutdownImGui()
    {
        LOG_DEBUG("Shutting down ImGui");

        HookManager::Unhook(Mod::GetGameInterface()->GetWndProc());
        Mod::GetGameInterface()->SetMouseMode(Types::MouseMode::Passthrough);

        for (const auto& component : uiComponents)
        {
            component->Release();
        }
        uiComponentsInitialized = false;

        TaskbarProgress::Shutdown();

        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        isInitialized = false;
    }

    // The layout and the mouse are in window coordinates. A back buffer of another size, when a capture renders above
    // the desktop's resolution, is scaled into the window, so the overlay is drawn at the back buffer's scale.
    void ScaleDrawDataToBackBuffer(ImDrawData* drawData)
    {
        IDirect3DSurface9* backBuffer = nullptr;
        if (drawData->DisplaySize.x <= 0 || drawData->DisplaySize.y <= 0 ||
            FAILED(D3D9::GetDevice()->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
            return;

        D3DSURFACE_DESC desc{};
        const auto result = backBuffer->GetDesc(&desc);
        backBuffer->Release();
        const ImVec2 scale(desc.Width / drawData->DisplaySize.x, desc.Height / drawData->DisplaySize.y);
        if (FAILED(result) || (scale.x == 1.0f && scale.y == 1.0f))
            return;

        for (int i = 0; i < drawData->CmdListsCount; i++)
        {
            for (auto& vertex : drawData->CmdLists[i]->VtxBuffer)
            {
                vertex.pos.x *= scale.x;
                vertex.pos.y *= scale.y;
            }
        }
        drawData->ScaleClipRects(scale);
        drawData->DisplaySize = ImVec2(drawData->DisplaySize.x * scale.x, drawData->DisplaySize.y * scale.y);
    }

    // whether a mouse button is pressed and let go (or let go and pressed) in the input events since the last frame
    bool HasButtonChangedTwice()
    {
        int changed = 0;
        for (const auto& event : ImGui::GetCurrentContext()->InputEventsQueue)
        {
            if (event.Type != ImGuiInputEventType_MouseButton)
                continue;
            const int bit = 1 << event.MouseButton.Button;
            if (changed & bit)
                return true;
            changed |= bit;
        }
        return false;
    }

    void UIManager::RunImGuiFrame()
    {
        try
        {
            // a UI scale chosen, or a window of another width: the fonts again at their new size, before the frame
            // (the backend makes the font texture again in its NewFrame)
            if (const auto size = GetWantedFontSize(); size != fontSize)
            {
                LoadFonts(size);
                ImGui_ImplDX9_InvalidateDeviceObjects();
            }

            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            // The input events of a frame aren't spread over frames (see Initialize), so a button pressed and let go
            // before a frame came would never be seen down: a click was lost when the frames stopped about as long as
            // it lasts (in T5 the demo's end rewinds it to a second before, and the render thread, which draws the UI,
            // waits about 100 ms for the main thread to catch up). Such a frame takes the events up to the second
            // change of the button, and the next frame the rest.
            ImGui::GetIO().ConfigInputTrickleEventQueue = HasButtonChangedTwice();
            ImGui::NewFrame();
            ImGui::GetIO().ConfigInputTrickleEventQueue = false;

            Input::UpdateState(ImGui::GetIO());

            if (!uiComponentsInitialized)
            {
                RegisterCommands();
                for (const auto& component : GetUIComponents())
                    component->Initialize();
                uiComponentsInitialized = true;
            }

            // the keys pressed since last frame, before anything is drawn
            Commands::Dispatch(GetActiveContexts());
            GettingStarted::Update();
            ActorActions::Update();
            GetUIComponent<DemoLoader>(Component::DemoLoader)->Update();

            if (!hideOverlay)
            {
                Layout::Update();
                GetUIComponent(Component::Background)->Render();
                GetUIComponent(Component::MenuBar)->Render();
                GetUIComponent(Component::GameView)->Render();
                GetUIComponent(Component::PrimaryTabs)->Render();
                GetUIComponent(Component::Timeline)->Render();
                Layout::DrawSplitters();
                GetUIComponent(Component::ControlsMenu)->Render();
                GetUIComponent(Component::Preferences)->Render();
                GetUIComponent(Component::Credits)->Render();
                GetUIComponent(Component::Shortcuts)->Render();
            }

            // (with the developer tools on, in Preferences)
            const bool developer = PreferencesConfiguration::Get().developerTools;
            if (showImGuiDemo && developer)
            {
                ImGui::ShowDemoWindow(&showImGuiDemo);
            }

            if (showDebugPanel && developer)
            {
                GetUIComponent(Component::DebugPanel)->Render();
            }

            GetUIComponent<DemoLoader>(Component::DemoLoader)->Send();
            Events::Invoke(EventType::OnFrame);

            ImGui::EndFrame();
            ImGui::Render();
            ScaleDrawDataToBackBuffer(ImGui::GetDrawData());
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        }
        catch (std::exception& e)
        {
            LOG_CRITICAL("An exception occurred while rendering the {0} user interface: {1}",
                         ModName(Mod::GetGameInterface()->GetGame()), e.what());
            __debugbreak();
        }
        catch (...)
        {
            const auto message = std::format("An error occurred while rendering the {} user interface",
                                             ModName(Mod::GetGameInterface()->GetGame()));
            LOG_CRITICAL("{}", message);

            // TODO: panic function
            MessageBox(NULL, message.c_str(), "FATAL ERROR", MB_OK);
        }
    }

    typedef LRESULT(__stdcall* GameWndProc_t)(HWND hWnd, UINT Msg, unsigned int wParam, LPARAM lParam);
    GameWndProc_t GameWndProc_Trampoline;

    namespace
    {
        // CoD games tell the key that opens the console (the one below Escape) by its scan code, whatever the layout
        constexpr LPARAM CONSOLE_KEY_SCAN_CODE = 0x29;
        constexpr std::uint32_t MOUSE_BUTTONS = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2;

        // the mouse buttons down as of the last mouse message, and those of them the game was told of
        std::uint32_t buttonsDown = 0;
        std::uint32_t gameButtons = 0;
        // keys whose press was kept from the game, by virtual key, so that their repeats are too
        std::array<bool, 256> keysHeldBack{};

        // The key of a press the backend has just given ImGui: its last event, as it gives the modifiers first. None
        // for a repeat, which ImGui isn't given again while it has the key down.
        ImGuiKey GetPressedKey(int eventsBefore)
        {
            const auto& queue = ImGui::GetCurrentContext()->InputEventsQueue;
            for (int i = queue.Size - 1; i >= eventsBefore; --i)
            {
                const auto& event = queue[i];
                if (event.Type == ImGuiInputEventType_Key && event.Key.Down &&
                    (event.Key.Key < ImGuiKey_ReservedForModCtrl || event.Key.Key > ImGuiKey_ReservedForModSuper))
                    return event.Key.Key;
            }
            return ImGuiKey_None;
        }

        // the modifiers held, as the backend tells ImGui of them
        ImGuiKeyChord GetModifiers()
        {
            ImGuiKeyChord modifiers = 0;
            for (const auto [vk, modifier] : {std::pair(VK_CONTROL, ImGuiMod_Ctrl), std::pair(VK_SHIFT, ImGuiMod_Shift),
                                              std::pair(VK_MENU, ImGuiMod_Alt), std::pair(VK_APPS, ImGuiMod_Super)})
            {
                if (GetKeyState(vk) & 0x8000)
                    modifiers |= modifier;
            }
            return modifiers;
        }

        // Whether a window message reaches the game, which turns key presses, the wheel and mouse buttons into binds
        // (see InputRouting). Mouse messages tell it which buttons are down: those it isn't to know of are left out.
        bool ReachesGame(UINT message, WPARAM& wParam, LPARAM lParam, int eventsBefore)
        {
            static const UINT MSH_MOUSEWHEEL = RegisterWindowMessageA("MSWHEEL_ROLLMSG");
            const auto& io = ImGui::GetIO();
            const auto& game = Mod::GetGameInterface();
            const bool gameTakesKeys = game->IsGameUITakingKeys();
            const InputRouting::State state = {!UIManager::Get().IsOverlayHidden(), gameTakesKeys,
                                               game->GetGameState() == Types::GameState::InDemo,
                                               gameTakesKeys && !game->IsConsoleOpen()};
            // a text field, a control being dragged or a popup of the editor
            const bool editorHasKeyboard = state.editorShown && (io.WantTextInput || io.WantCaptureKeyboard ||
                                                                 ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));

            if (message == MSH_MOUSEWHEEL && MSH_MOUSEWHEEL)
                return InputRouting::MouseReachesGame(state);
            switch (message)
            {
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                {
                    // (the game's window procedure turns Alt+Enter into a switch to full screen, never a bind)
                    if (message == WM_SYSKEYDOWN && wParam == VK_RETURN)
                        return true;
                    const auto vk = wParam & 0xFF;
                    const bool repeat = (lParam & (1 << 30)) != 0;
                    const auto key = GetPressedKey(eventsBefore);
                    const bool used =
                        editorHasKeyboard || (key != ImGuiKey_None && Commands::UsesPress(key, GetModifiers(), repeat));
                    const auto routed = ((lParam >> 16) & 0xFF) == CONSOLE_KEY_SCAN_CODE ? InputRouting::Key::Console
                                        : vk == VK_ESCAPE                                ? InputRouting::Key::Escape
                                                                                         : InputRouting::Key::Other;
                    const bool reaches =
                        !(repeat && keysHeldBack[vk]) && InputRouting::KeyPressReachesGame(state, used, routed);
                    if (!repeat)
                        keysHeldBack[vk] = !reaches;
                    return reaches;
                }
                case WM_KEYUP:
                case WM_SYSKEYUP:
                    keysHeldBack[wParam & 0xFF] = false;
                    return true;
                case WM_CHAR:
                case WM_SYSCHAR:
                case WM_DEADCHAR:
                case WM_SYSDEADCHAR:
                    return InputRouting::CharacterReachesGame(state, editorHasKeyboard);
                case WM_MOUSEWHEEL:
                case WM_MOUSEHWHEEL:
                    return InputRouting::MouseReachesGame(state);
                case WM_MOUSEMOVE:
                case WM_LBUTTONDOWN:
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN:
                case WM_RBUTTONUP:
                case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDOWN:
                case WM_MBUTTONUP:
                case WM_MBUTTONDBLCLK:
                case WM_XBUTTONDOWN:
                case WM_XBUTTONUP:
                case WM_XBUTTONDBLCLK:
                {
                    const auto down = static_cast<std::uint32_t>(wParam) & MOUSE_BUTTONS;
                    gameButtons = InputRouting::GameButtons(gameButtons, buttonsDown, down,
                                                            InputRouting::MouseReachesGame(state));
                    buttonsDown = down;
                    wParam = (wParam & ~static_cast<WPARAM>(MOUSE_BUTTONS)) | gameButtons;
                    return true;
                }
                default:
                    return true;
            }
        }
    }  // namespace

    HRESULT __stdcall ImGuiWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        auto& uiManager = UIManager::Get();
        auto& gameView = uiManager.GetUIComponent(UI::Component::GameView);
        if (gameView->HasFocus() && uiManager.IsControllableCameraModeSelected())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        }

        if (!uiManager.IsOverlayHidden())
        {
            // Make sure the system cursor wasnt hidden by some game logic
            ShowCursor(TRUE);
        }

        const auto eventsBefore = ImGui::GetCurrentContext()->InputEventsQueue.Size;
        if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
        {
            return true;
        }

        if (!ReachesGame(uMsg, wParam, lParam, eventsBefore))
            return 0;
        return GameWndProc_Trampoline(hWnd, uMsg, wParam, lParam);
    }

    void UIManager::SelectTab(Tab tab, Page page)
    {
        // leaving the Record tab ends its preview of a pass
        if (selectedTab == Tab::Record && tab != Tab::Record)
        {
            const auto captureMenu = GetUIComponent<CaptureMenu>(Component::CaptureMenu);
            if (captureMenu->GetDisplayPassIndex().has_value())
                captureMenu->ResetPassPreview();
        }
        selectedTab = tab;
        if (page != Page::None && PanelLogic::GetTab(page) == tab)
            pages[static_cast<std::size_t>(tab)] = page;
    }

    void UIManager::ShowPicked(PanelLogic::Kind kind)
    {
        const std::array kinds = {kind};
        ShowPicked(kinds);
    }

    void UIManager::ShowPicked(std::span<const PanelLogic::Kind> kinds)
    {
        const auto place = PanelLogic::GetPlace(kinds);
        if (!place || Components::CaptureManager::Get().IsCapturing())
            return;
        SelectTab(place->tab, place->page);
        if (const auto page = PanelLogic::GetActorPage(kinds))
            GetUIComponent<ActorsMenu>(Component::ActorsMenu)->ShowPage(*page);
    }

    void UIManager::ToggleOverlay()
    {
        hideOverlay = !hideOverlay;
        Mod::GetGameInterface()->SetMouseMode(hideOverlay ? Types::MouseMode::Passthrough : Types::MouseMode::Capture);
    }

    bool UIManager::IsControllableCameraModeSelected()
    {
        auto mode = Components::CameraManager::Get().GetActiveCamera()->GetMode();
        return mode == Components::Camera::Mode::Free || mode == Components::Camera::Mode::Bone;
    }

    bool UIManager::IsFlying()
    {
        return GetUIComponent(Component::GameView)->HasFocus() && IsControllableCameraModeSelected();
    }

    Commands::ContextMask UIManager::GetActiveContexts()
    {
        using Commands::Bit;
        using Commands::Context;

        Commands::ContextMask active = Bit(Context::Global);
        if (ImGui::GetIO().WantTextInput || Mod::GetGameInterface()->IsConsoleOpen())
            active |= Bit(Context::Typing);
        if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup))
            active |= Bit(Context::Popup);
        if (Components::CaptureManager::Get().IsCapturing())
            active |= Bit(Context::Capturing);
        if (IsFlying())
            active |= Bit(Context::Fly);
        if (GFX::GraphicsManager::Get().IsPlacingPathPoints())
            active |= Bit(Context::Placing);
        // the editor hidden keeps keys from it as recording does: only F1 (and Escape while recording) reach it
        if (hideOverlay)
            return active | Bit(Context::Capturing);

        // where the mouse is, or which of the layout's windows has the focus when it is over none of them
        static const std::array<std::pair<std::string_view, Context>, 10> WINDOWS = {{
            {"GameView", Context::Viewport},
            {"Timeline", Context::Timeline},
            {"PrimaryTabs", Context::Panel},
            {"Demos", Context::Panel},
            {"Kills", Context::Panel},
            {"Camera", Context::Panel},
            {"Visuals", Context::Panel},
            {"Actors", Context::Panel},
            {"Capture", Context::Panel},
            {"Players", Context::Panel},
        }};
        auto contextOf = [](ImGuiWindow* window) -> std::optional<Context> {
            if (!window)
                return std::nullopt;
            const std::string_view name = window->RootWindow->Name;
            for (const auto& [windowName, context] : WINDOWS)
            {
                if (windowName == name)
                    return context;
            }
            return std::nullopt;
        };
        const auto& g = *ImGui::GetCurrentContext();
        auto context = contextOf(g.HoveredWindow);
        if (!context)
            context = contextOf(g.NavWindow);
        if (context)
            active |= Bit(*context);
        return active;
    }

    namespace
    {
        // What Delete and Ctrl+D act on: what is selected on the timeline when the mouse is over it, else what the
        // Scene tab's actors have selected, else what is selected on the timeline
        enum class EditTarget
        {
            None,
            Timeline,
            Actors,
        };

        EditTarget GetEditTarget(bool remove)
        {
            const bool timeline = TimelineSelection::Count() > 0;
            const auto actorsMenu = UIManager::Get().GetUIComponent<ActorsMenu>(Component::ActorsMenu);
            const bool actors = !actorsMenu->GetEditUnavailableReason(remove).has_value();
            if (timeline && Commands::IsActive(Commands::Context::Timeline))
                return EditTarget::Timeline;
            if (actors)
                return EditTarget::Actors;
            return timeline ? EditTarget::Timeline : EditTarget::None;
        }

        std::optional<std::string> WhyCantEdit(bool remove)
        {
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return "Load a demo first";
            if (GetEditTarget(remove) != EditTarget::None)
                return std::nullopt;
            if (UIManager::Get().IsShown(Page::Actors))
                return UIManager::Get().GetUIComponent<ActorsMenu>(Component::ActorsMenu)->GetEditUnavailableReason(remove);
            return "Select something on the timeline, or an actor on the Scene tab";
        }

        // not while a drag is still changing something: it ends as a step of its own (typing keeps keys from
        // commands anyway)
        std::optional<std::string> WhyCantUndo(const std::optional<std::string>& label, bool redo)
        {
            if (!label)
                return redo ? "Nothing to redo" : "Nothing to undo";
            if (Components::KeyframeManager::Get().AreKeyframesBeingModified() || TimelineSelection::IsMoving() ||
                GFX::GraphicsManager::Get().WasObjectDraggedThisFrame() ||
                ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                return "Finish the drag first";
            return std::nullopt;
        }
    }  // namespace

    void UIManager::RegisterCommands()
    {
        using Commands::Bit;
        using Commands::Context;

        Commands::Register({
            .id = "view.toggleOverlay",
            .label = "Show or hide the editor",
            .category = "View",
            .defaults = {ImGuiKey_F1},
            .contexts = Bit(Context::Capturing) | Bit(Context::Fly) | Bit(Context::Global),
            .run = [this] { ToggleOverlay(); },
            .keywords = "ui overlay hide interface",
        });
        GettingStarted::RegisterCommands();

        const auto developerOnly = []() -> std::optional<std::string> {
            if (!PreferencesConfiguration::Get().developerTools)
                return "Turn on the developer tools in File > Preferences first";
            return std::nullopt;
        };
        Commands::Register({
            .id = "developer.imguiDemo",
            .label = "ImGui demo",
            .category = "Developer",
            .defaults = {ImGuiKey_F3},
            .unavailable = developerOnly,
            .run = [this] { ToggleImGuiDemo(); },
        });
        Commands::Register({
            .id = "developer.debugPanel",
            .label = "Debug panel",
            .category = "Developer",
            .unavailable = developerOnly,
            .run = [this] { ToggleDebugPanel(); },
        });

        // the UI's size (see LayoutMath), saved at once
        const auto scaleTo = [](float scale) {
            Layout::SetScale(scale);
            Toasts::Show(std::format("UI at {:.0f}%", PreferencesConfiguration::Get().uiScale * 100.0f));
        };
        Commands::Register({
            .id = "view.biggerUI",
            .label = "Bigger UI",
            .category = "View",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_Equal},
            .unavailable = []() -> std::optional<std::string> {
                if (PreferencesConfiguration::Get().uiScale >= LayoutMath::SCALES.back())
                    return "The UI is as big as it gets";
                return std::nullopt;
            },
            .run = [scaleTo] { scaleTo(LayoutMath::StepScale(PreferencesConfiguration::Get().uiScale, 1)); },
            .keywords = "ui scale zoom font size larger",
        });
        Commands::Register({
            .id = "view.smallerUI",
            .label = "Smaller UI",
            .category = "View",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_Minus},
            .unavailable = []() -> std::optional<std::string> {
                if (PreferencesConfiguration::Get().uiScale <= LayoutMath::SCALES.front())
                    return "The UI is as small as it gets";
                return std::nullopt;
            },
            .run = [scaleTo] { scaleTo(LayoutMath::StepScale(PreferencesConfiguration::Get().uiScale, -1)); },
            .keywords = "ui scale zoom font size",
        });
        Commands::Register({
            .id = "view.resetUI",
            .label = "UI at 100%",
            .category = "View",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_0},
            .run = [scaleTo] { scaleTo(1.0f); },
            .keywords = "ui scale zoom font size reset",
        });

        // what the view shows besides the game
        const std::array<
            std::tuple<std::string_view, std::string_view, bool PreferencesConfiguration::*, std::string_view>, 5>
            shows = {{
                {"view.campath", "Show the campath", &PreferencesConfiguration::showCampath, "nodes path line dolly"},
                {"view.actorRings", "Show rings under actors", &PreferencesConfiguration::showActorRings,
                 "actors circles"},
                {"view.actorPaths", "Show other actors' paths", &PreferencesConfiguration::showActorPaths,
                 "actors paths points lines"},
                {"view.safeFrames", "Show safe frames", &PreferencesConfiguration::showSafeFrames,
                 "title action safe area guides"},
                {"view.keyHints", "Show keys in the mode strip", &PreferencesConfiguration::showKeybindHints,
                 "hints shortcuts strip"},
            }};
        for (const auto& [id, label, show, keywords] : shows)
        {
            Commands::Register({
                .id = id,
                .label = label,
                .category = "View",
                .run =
                    [show] {
                        auto& preferences = PreferencesConfiguration::Get();
                        preferences.*show = !(preferences.*show);
                        Configuration::Get().Write(true);
                    },
                .keywords = keywords,
            });
        }

        const std::array<std::tuple<std::string_view, std::string_view, Layout::Preset, std::string_view>, 3> layouts =
            {{
                {"view.layoutDefault", "Default layout", Layout::Preset::Default, "reset splitters panel timeline"},
                {"view.layoutEditing", "Layout for editing (a tall timeline)", Layout::Preset::Editing,
                 "splitters panel timeline big"},
                {"view.layoutWatching", "Layout for watching (a big view)", Layout::Preset::Watching,
                 "splitters panel timeline small"},
            }};
        for (const auto& [id, label, preset, keywords] : layouts)
        {
            Commands::Register({
                .id = id,
                .label = label,
                .category = "View",
                .run = [preset] { Layout::Apply(preset); },
                .keywords = keywords,
            });
        }

        Commands::Register({
            .id = "edit.undo",
            .label = "Undo",
            .category = "Edit",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_Z},
            .unavailable = [] { return WhyCantUndo(Components::UndoHistory::Get().GetUndoLabel(), false); },
            .run =
                [] {
                    if (const auto label = Components::UndoHistory::Get().Undo())
                        Toasts::Show("Undid " + *label);
                },
            .keywords = "back revert",
        });
        Commands::Register({
            .id = "edit.redo",
            .label = "Redo",
            .category = "Edit",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z},
            .unavailable = [] { return WhyCantUndo(Components::UndoHistory::Get().GetRedoLabel(), true); },
            .run =
                [] {
                    if (const auto label = Components::UndoHistory::Get().Redo())
                        Toasts::Show("Redid " + *label);
                },
        });
        Commands::Register({
            .id = "edit.delete",
            .label = "Delete",
            .category = "Edit",
            .defaults = {ImGuiKey_Delete},
            .unavailable = [] { return WhyCantEdit(true); },
            .run =
                [] {
                    if (GetEditTarget(true) == EditTarget::Timeline)
                        TimelineSelection::Delete();
                    else
                        UIManager::Get().GetUIComponent<ActorsMenu>(Component::ActorsMenu)->DeleteOrDuplicateSelected(true);
                },
            .keywords = "remove erase",
        });
        Commands::Register({
            .id = "edit.duplicate",
            .label = "Duplicate",
            .category = "Edit",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_D},
            .unavailable = [] { return WhyCantEdit(false); },
            .run =
                [] {
                    if (GetEditTarget(false) == EditTarget::Timeline)
                        TimelineSelection::Duplicate();
                    else
                        UIManager::Get()
                            .GetUIComponent<ActorsMenu>(Component::ActorsMenu)
                            ->DeleteOrDuplicateSelected(false);
                },
            .keywords = "copy clone",
        });
    }

    ImVec2 UIManager::GetWindowSize()
    {
        RECT windowRect = {};
        GetWindowRect(windowHandle, &windowRect);
        ImVec2 windowSize = {static_cast<float>(windowRect.right - windowRect.left),
                             static_cast<float>(windowRect.bottom - windowRect.top)};
        return windowSize;
    }

    ImVec2 UIManager::GetWindowPosition()
    {
        RECT windowRect = {};
        GetWindowRect(windowHandle, &windowRect);
        ImVec2 windowSize = {static_cast<float>(windowRect.left), static_cast<float>(windowRect.top)};
        return windowSize;
    }

    float UIManager::GetWantedFontSize()
    {
        return LayoutMath::GetFontSize(GetWindowSize().x, PreferencesConfiguration::Get().uiScale);
    }

    void SetImGuiStyle(float fontSize)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.FramePadding = {fontSize * 0.28f, fontSize * 0.28f};
        style.WindowPadding = {16, 16};
        // the palette of the preferences (UI/Theme.hpp)
        Theme::Apply();
    }

    INCBIN_EXTERN(RUBIK_FONT);
    INCBIN_EXTERN(RUBIK_BOLD_FONT);
    INCBIN_EXTERN(FA_ICONS_FONT);

    void UIManager::ApplyPalette()
    {
        Theme::Apply();
    }

    void UIManager::LoadFonts(float size)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();

        auto RegisterFont = [&](const char* name, const uint8_t* data, size_t dataSize) {
            ImFontConfig fontConfig;
            fontConfig.FontDataOwnedByAtlas = false;
            fontConfig.SizePixels = size;
            strcpy_s(fontConfig.Name, name);
            io.Fonts->AddFontFromMemoryTTF((void*)data, dataSize, size, &fontConfig);
            LOG_DEBUG("Registered font {0} with size {1}", name, size);

            auto iconFontSize = size * 2.0f / 3.0f;
            static const ImWchar icons_ranges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
            ImFontConfig icons_config;
            icons_config.FontDataOwnedByAtlas = false;
            icons_config.MergeMode = true;
            icons_config.PixelSnapH = true;
            icons_config.GlyphMinAdvanceX = iconFontSize;
            io.Fonts->AddFontFromMemoryTTF((void*)FA_ICONS_FONT_data, FA_ICONS_FONT_size, iconFontSize, &icons_config,
                                           icons_ranges);
        };

        RegisterFont("RubikRegular", RUBIK_FONT_data, RUBIK_FONT_size);
        RegisterFont("RubikBold", RUBIK_BOLD_FONT_data, RUBIK_BOLD_FONT_size);

        SetImGuiStyle(size);
        fontSize = size;
    }

    void UIManager::Initialize(IDirect3DDevice9* device, HWND hwnd)
    {
        try
        {
            LOG_DEBUG("Initializing ImGui...");

            LOG_DEBUG("Creating ImGui context");
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();

            ImGui::StyleColorsDark();

            if (hwnd == nullptr)
            {
                hwnd = D3D9::FindWindowHandle();
            }
            this->windowHandle = hwnd;
            
            LOG_DEBUG("Initializing ImGui_ImplWin32 with HWND {0:x}", (std::uintptr_t)hwnd);
            ImGui_ImplWin32_Init(hwnd);

            LOG_DEBUG("Initializing ImGui_ImplDX9 with D3D9 Device {0:x}", (std::uintptr_t)device);
            ImGui_ImplDX9_Init(device);

            LOG_DEBUG("Hooking WndProc at {0:x}", Mod::GetGameInterface()->GetWndProc());

            HookManager::CreateHook(Mod::GetGameInterface()->GetWndProc(), (uintptr_t)ImGuiWndProc, (uintptr_t*)&GameWndProc_Trampoline);

            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = NULL;
            // io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            LoadFonts(GetWantedFontSize());

            ImGui::GetIO().ConfigInputTrickleEventQueue = false;

            Mod::GetGameInterface()->SetMouseMode(Types::MouseMode::Capture);
            isInitialized = true;
            ++initializeCount;

            Components::CaptureManager::Get().Initialize();
            TaskbarProgress::Initialize(hwnd);

            LOG_INFO("Initialized UI");
        }
        catch (...)
        {
            throw std::runtime_error("Failed to initialize UI");
            // TODO: panic
        }
    }
}  // namespace IWXMVM::UI
