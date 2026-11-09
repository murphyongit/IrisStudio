#pragma once
#include "UI/UIComponent.hpp"

namespace IWXMVM::UI
{
    // The shortcuts sheet (?): every command with a key, by where it works, made from the commands; and the command
    // palette (Ctrl+K), which finds a command by name or other words for it and runs it
    class ShortcutsWindow : public UIComponent
    {
       public:
        void Render() final;
        void Release() final;

        void ToggleVisibility()
        {
            visible = !visible;
        }

        void OpenPalette()
        {
            openPalette = true;
        }

       private:
        void Initialize() final;
        void RenderSheet();
        void RenderPalette();

        bool visible = false;
        bool openPalette = false;
        std::string query;
        std::size_t highlighted = 0;
    };
}  // namespace IWXMVM::UI
