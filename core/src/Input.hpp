#pragma once
#include "StdInclude.hpp"

namespace IWXMVM
{
    class Input
    {
       public:
        // The raw state of keys and mouse buttons, for what the UI does with the mouse itself. Keyboard shortcuts are
        // commands (UI/Commands.hpp), which know about flying, typing and popups.

        // a press, once: held keys only repeat with repeat
        static bool KeyDown(ImGuiKey key, bool repeat = false);
        static bool KeyUp(ImGuiKey key);
        static bool KeyHeld(ImGuiKey key);

        static ImVec2 GetMouseDelta();
        static float GetScrollDelta();
        static void UpdateState(ImGuiIO& io);

        static float GetDeltaTime();

       private:
        static inline float mouseWheelDelta;
    };
}  // namespace IWXMVM