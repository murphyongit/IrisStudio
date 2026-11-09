#pragma once
#include "UI/UIComponent.hpp"
#include "UI/PanelLogic.hpp"

namespace IWXMVM::UI
{
    using PanelLogic::Page;
    using PanelLogic::Tab;

    // The side panel: its five tabs in the order of the work (Demo, Camera, Look, Scene, Record), the pages of the
    // Demo and Scene tabs, the path of what the tab shows ("Reznov > Clip > ai_walk_f"), and the tab's window under
    // them. It follows what is picked in the view, on the timeline or in a list to the tab that shows it.
    class PrimaryTabs : public UIComponent
    {
       public:
        void Initialize() final;
        void Render() final;
        void Release() final;

        // the path shown over the tab last, empty when none was (for the harness)
        const std::string& GetShownPath() const
        {
            return shownPath;
        }

       private:
        void RenderSelectedTab();
        void FollowPicks();
        void DrawPages();
        void DrawPath();

        PanelLogic::Follower timelineFollower;
        PanelLogic::Follower actorsFollower;
        bool wasInDemo = false;
        std::string shownPath;
    };

    // What a tab shows outside a demo: that it needs one, and a button to the demos
    void DrawInaccessibleTabWarning();
}  // namespace IWXMVM::UI
