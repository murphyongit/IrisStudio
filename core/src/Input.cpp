#include "StdInclude.hpp"
#include "Input.hpp"

#include "Mod.hpp"

namespace IWXMVM
{
    const std::map<ImGuiKey, ImGuiMouseButton> mouseButtonMap = {{ImGuiKey_MouseLeft, ImGuiMouseButton_Left},
                                                                 {ImGuiKey_MouseRight, ImGuiMouseButton_Right},
                                                                 {ImGuiKey_MouseMiddle, ImGuiMouseButton_Middle}};

    bool IsMouseButton(ImGuiKey key)
    {
        return key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY;
    }

    // keys typed into a text field aren't binds
    bool IsTyping(ImGuiKey key)
    {
        return !IsMouseButton(key) && ImGui::GetIO().WantTextInput;
    }

    bool Input::KeyDown(ImGuiKey key, bool repeat)
    {
        // (a bind without a key)
        if (key == ImGuiKey_None || Mod::GetGameInterface()->IsConsoleOpen() || IsTyping(key))
            return false;

        if (IsMouseButton(key))
            return ImGui::IsMouseClicked(mouseButtonMap.at(key));
        return ImGui::IsKeyPressed(key, repeat);
    }

    bool Input::KeyUp(ImGuiKey key)
    {
        if (key == ImGuiKey_None || Mod::GetGameInterface()->IsConsoleOpen() || IsTyping(key))
            return false;

        if (IsMouseButton(key))
            return ImGui::IsMouseReleased(mouseButtonMap.at(key));
        return ImGui::IsKeyReleased(key);
    }

    bool Input::KeyHeld(ImGuiKey key)
    {
        if (key == ImGuiKey_None || Mod::GetGameInterface()->IsConsoleOpen() || IsTyping(key))
            return false;

        if (IsMouseButton(key))
            return ImGui::IsMouseDown(mouseButtonMap.at(key));
        return ImGui::IsKeyDown(key);
    }

    ImVec2 Input::GetMouseDelta()
    {
        if (Mod::GetGameInterface()->IsConsoleOpen())
            return ImVec2(0, 0);

        ImGuiIO& io = ImGui::GetIO();
        return io.MouseDelta;
    }

    float Input::GetScrollDelta()
    {
        if (Mod::GetGameInterface()->IsConsoleOpen())
            return 0;

        return mouseWheelDelta;
    }

    void Input::UpdateState(ImGuiIO& io)
    {
        mouseWheelDelta = io.MouseWheel;
    }

    float Input::GetDeltaTime()
    {
        ImGuiIO& io = ImGui::GetIO();
        return io.DeltaTime;
    }
}  // namespace IWXMVM