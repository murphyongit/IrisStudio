#pragma once
#include "StdInclude.hpp"

#include "UIComponent.hpp"
#include "Commands.hpp"

#include "Components/Background.hpp"
#include "Components/CaptureMenu.hpp"
#include "Components/DebugPanel.hpp"
#include "Components/DemoLoader.hpp"
#include "Components/GameView.hpp"
#include "Components/MenuBar.hpp"
#include "Components/PrimaryTabs.hpp"
#include "Components/VisualsMenu.hpp"
#include "Components/KillsMenu.hpp"
#include "Components/CameraMenu.hpp"
#include "Components/Timeline.hpp"
#include "Components/ControlsMenu.hpp"
#include "Components/Preferences.hpp"
#include "Components/PlayerAnimationUI.hpp"
#include "Components/LoadoutsUI.hpp"
#include "Components/ActorsMenu.hpp"
#include "Components/Credits.hpp"
#include "Components/ShortcutsWindow.hpp"
#include "Components/PlayersMenu.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace IWXMVM::UI
{
    namespace Component
    {
        enum Component
        {
            Background = 0,
            MenuBar,
            GameView,
            PrimaryTabs,
            DemoLoader,
            CameraMenu,
            VisualsMenu,
            CaptureMenu,
            Timeline,
            DebugPanel,
            ControlsMenu,
            Preferences,
            PlayerAnimation,
            Credits,
            KillsMenu,
            Loadouts,
            ActorsMenu,
            Shortcuts,
            PlayersMenu,

            Count,
        };
    }

    class UIManager
    {
       public:
        static UIManager& Get()
        {
            static UIManager instance;
            return instance;
        }

        UIManager(UIManager const&) = delete;
        void operator=(UIManager const&) = delete;

        void Initialize(IDirect3DDevice9* device, HWND hwnd = nullptr);
        void ShutdownImGui();
        void RunImGuiFrame();

        bool IsControllableCameraModeSelected();
        // whether the free (or bone) camera has the keyboard and mouse: F, or the right mouse button held in the view
        bool IsFlying();

        // the input contexts of this frame, from last frame's state (see Commands)
        Commands::ContextMask GetActiveContexts();

        ImVec2 GetWindowSize();
        ImVec2 GetWindowPosition();

        bool IsInitialized() const
        {
            return isInitialized;
        }
        // how many times the UI has started: once more after each time the game made its device again (a vid_restart,
        // a mod loaded)
        std::uint32_t GetInitializeCount() const
        {
            return initializeCount;
        }

        WNDPROC GetOriginalGameWndProc() const
        {
            return originalGameWndProc;
        }

        std::unique_ptr<UIComponent> const& GetUIComponent(Component::Component component) const
        {
            return uiComponents[component];
        }

        // (by value: a reference to the cast's result outlived it)
        template <typename T>
        T* GetUIComponent(Component::Component component) const
        {
            return dynamic_cast<T*>(uiComponents[component].get());
        }

        std::array<std::unique_ptr<UIComponent>, Component::Count> const& GetUIComponents() const
        {
            return uiComponents;
        }

        // a tab of the side panel and, for the Demo and Scene tabs, a page (else the one it showed last)
        void SelectTab(Tab tab, Page page = Page::None);

        Tab GetSelectedTab() const
        {
            return selectedTab;
        }
        // the page a tab shows (Page::None for those without pages), and the page its tab shows from now on
        Page GetPage(Tab tab) const
        {
            return pages[static_cast<std::size_t>(tab)];
        }
        void SetPage(Page page)
        {
            if (page != Page::None)
                pages[static_cast<std::size_t>(PanelLogic::GetTab(page))] = page;
        }
        // what was picked in the view (an actor, a node) or clicked on the timeline: its tab (and a clip's or event's
        // page) opens at once, even when it was picked already
        void ShowPicked(PanelLogic::Kind kind);
        void ShowPicked(std::span<const PanelLogic::Kind> kinds);

        // whether the panel shows a page (the Scene tab's actors...)
        bool IsShown(Page page) const
        {
            return selectedTab == PanelLogic::GetTab(page) && GetPage(selectedTab) == page;
        }

        void ToggleOverlay();

        bool IsOverlayHidden()
        {
            return hideOverlay;
        }

        void ToggleImGuiDemo()
        {
            showImGuiDemo = !showImGuiDemo;
        }
        bool IsImGuiDemoShown() const
        {
            return showImGuiDemo;
        }

        void ToggleDebugPanel()
        {
            showDebugPanel = !showDebugPanel;
        }
        bool IsDebugPanelShown() const
        {
            return showDebugPanel;
        }

        // the palette of the preferences onto the style again, once it changed
        void ApplyPalette();

        ImFont* GetBoldFont()
		{
			return ImGui::GetIO().Fonts->Fonts[1];
		}

       private:
        UIManager()
        {
        }

        void RegisterCommands();

        // the font's size for the window's width and the UI scale (see LayoutMath), and the fonts at a size
        float GetWantedFontSize();
        void LoadFonts(float size);
        float fontSize = 0.0f;

        std::array<std::unique_ptr<UIComponent>, Component::Count> uiComponents = {
            std::make_unique<Background>(),      std::make_unique<MenuBar>(),      std::make_unique<GameView>(),
            std::make_unique<PrimaryTabs>(),     std::make_unique<DemoLoader>(),   std::make_unique<CameraMenu>(),
            std::make_unique<VisualsMenu>(),     std::make_unique<CaptureMenu>(),  std::make_unique<Timeline>(),
            std::make_unique<DebugPanel>(),      std::make_unique<ControlsMenu>(), std::make_unique<Preferences>(),
            std::make_unique<PlayerAnimation>(), std::make_unique<Credits>(),      std::make_unique<KillsMenu>(),
            std::make_unique<Loadouts>(),        std::make_unique<ActorsMenu>(),   std::make_unique<ShortcutsWindow>(),
            std::make_unique<PlayersMenu>(),
        };

        Tab selectedTab = Tab::Demo;
        std::array<Page, static_cast<std::size_t>(Tab::Count)> pages = {Page::Browse, Page::None, Page::None,
                                                                        Page::Actors, Page::None};
        bool isInitialized = false;
        std::uint32_t initializeCount = 0;
        bool uiComponentsInitialized = false;

        bool hideOverlay = false;
        bool showImGuiDemo = false;
        bool showDebugPanel = false;

        WNDPROC originalGameWndProc = nullptr;

        HWND windowHandle = NULL;
    };
}  // namespace IWXMVM::UI