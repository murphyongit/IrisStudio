#pragma once
#include "UI/UIComponent.hpp"

namespace IWXMVM::UI
{
    class GameView : public UIComponent
    {
       public:
        void Render() final;
        void Release() final;

        ImVec2 GetViewportPosition()
        {
            return viewportPosition;
        }

        ImVec2 GetViewportSize()
        {
            return viewportSize;
        }

        // whether the mouse is over what is drawn over the picture (the gizmo's buttons at its top left, the mode
        // strip and the messages along its bottom), which takes its own clicks, so the view's picking and gizmo leave
        // them
        bool IsMouseOverOverlay() const;
        bool IsOverOverlay(ImVec2 point) const;

       private:
        void Initialize() final;
        void DrawTopBar();

        void LockMouse();

        void RegisterCommands();
        void DrawGizmoControls(bool viewHovered);
        void PutGizmoAwayOnClick();
        float DrawModeStrip(ImVec2 min, ImVec2 max, bool viewHovered);

        // a placeholder for the black frames the game shows while it switches render sizes for a capture
        void DrawRenderSizeSwitch();

        IDirect3DTexture9* texture = NULL;
        ImVec2 textureSize = ImVec2(0, 0);

        ImVec2 viewportPosition = ImVec2(0, 0);
        ImVec2 viewportSize = ImVec2(0, 0);

        // flying while the right mouse button is held in the view, until it is let go
        bool flyingWhileHeld = false;

        // a right-click in the view, which opens the view's menu (UI::ViewMenu) if let go at once without moving, else
        // flies (see HandleRightClick)
        struct RightPress
        {
            double time = 0.0;
            ImVec2 position;
            float moved = 0.0f;  // pixels the mouse moved meanwhile
            std::optional<std::uint32_t> actor;
            glm::vec3 origin{}, direction{};
            bool menu = false;  // it can open the menu there
            bool held = false;  // it is a hold, not a click
            // the selected actor's path point or line under the mouse, whose menu it opens
            std::optional<std::size_t> point;
            std::optional<std::size_t> line;
        };
        std::optional<RightPress> rightPress;
        bool HandleRightClick(bool pressed);

        // where the gizmo's buttons, the strip and the messages were drawn last, as min and max
        std::vector<std::pair<ImVec2, ImVec2>> overlay;
    };
}  // namespace IWXMVM::UI