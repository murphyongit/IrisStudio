#pragma once
#include "UI/UIComponent.hpp"

namespace IWXMVM::UI
{
    // The kills of the demo, found when it loads, to jump to and to set capture ranges from
    class KillsMenu : public UIComponent
    {
       public:
        void Render() final;
        void Release() final;

       private:
        void Initialize() final;
        void RenderFilters();
        void RenderList();
    };
}  // namespace IWXMVM::UI
