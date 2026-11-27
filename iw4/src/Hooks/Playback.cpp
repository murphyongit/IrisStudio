#include "StdInclude.hpp"
#include "Playback.hpp"

#include "Components/Playback.hpp"
#include "Components/Rewinding.hpp"
#include "Events.hpp"
#include "Utilities/HookManager.hpp"

#include "../Addresses.hpp"
#include "../Structures.hpp"

namespace IWXMVM::IW4::Hooks::Playback
{
    namespace
    {
        std::atomic<std::uint32_t> messageHeaderSize = 9;
    }

    std::uint32_t GetMessageHeaderSize()
    {
        return messageHeaderSize.load();
    }

    // The "demo" command. On the main thread, for the console's command and the Demo tab's PLAY alike.
    typedef void(__cdecl* CL_PlayDemo_f_t)();
    CL_PlayDemo_f_t CL_PlayDemo_f_Trampoline;
    void __cdecl CL_PlayDemo_f_Hook()
    {
        Events::Invoke(EventType::PreDemoLoad);
        messageHeaderSize = 9;
        CL_PlayDemo_f_Trampoline();
        Events::Invoke(EventType::PostDemoLoad);
    }

    // The game's time for a frame, from the real time gone by (Com_Frame: msec = Com_TimeScaleMsec(msec), at
    // 0x47DF84). The game's own answer is never under 1; a pause needs 0.
    typedef std::int32_t(__cdecl* Com_TimeScaleMsec_t)(std::int32_t msec);
    Com_TimeScaleMsec_t Com_TimeScaleMsec_Trampoline;
    std::int32_t __cdecl Com_TimeScaleMsec_Hook(std::int32_t msec)
    {
        msec = Com_TimeScaleMsec_Trampoline(msec);
        return Components::Playback::CalculatePlaybackDelta(msec);
    }

    // What a playing demo is read through (clc.demoread is set to this by the demo command, 0x5A883E; it only
    // passes on to FS_Read).
    typedef int(__cdecl* CL_DemoRead_t)(void* buffer, int len, int file);
    CL_DemoRead_t CL_DemoRead_Trampoline;
    int __cdecl CL_DemoRead_Hook(void* buffer, int len, int file)
    {
        const auto clc = Structures::GetClientConnection();
        // (the file given is not looked at: the game's function reads clc.demofile whatever it is given)
        if (!clc->demoplaying)
            return CL_DemoRead_Trampoline(buffer, len, file);

        const auto result = Components::Rewinding::FS_Read(buffer, len);
        if (result == -1)
            return CL_DemoRead_Trampoline(buffer, len, file);

        // the byte that says what kind of message comes next: kind 2 has one more number before its length
        if (len == 1 && result == 1)
            messageHeaderSize = *static_cast<const std::uint8_t*>(buffer) == 2 ? 13 : 9;

        return result;
    }

    void Install()
    {
        HookManager::CreateHook(Addresses::CL_PlayDemo_f, (std::uintptr_t)CL_PlayDemo_f_Hook,
                                (std::uintptr_t*)&CL_PlayDemo_f_Trampoline);
        HookManager::CreateHook(Addresses::Com_TimeScaleMsec, (std::uintptr_t)Com_TimeScaleMsec_Hook,
                                (std::uintptr_t*)&Com_TimeScaleMsec_Trampoline);
        HookManager::CreateHook(Addresses::CL_DemoRead, (std::uintptr_t)CL_DemoRead_Hook,
                                (std::uintptr_t*)&CL_DemoRead_Trampoline);
    }
}  // namespace IWXMVM::IW4::Hooks::Playback
