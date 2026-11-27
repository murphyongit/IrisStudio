#include "StdInclude.hpp"
#include "Playback.hpp"

#include "../Structures.hpp"
#include "../Addresses.hpp"

#include "Components/Playback.hpp"
#include "Components/Rewinding.hpp"
#include "Components/CaptureManager.hpp"
#include "Mod.hpp"
#include "Utilities/HookManager.hpp"
#include "Events.hpp"

namespace IWXMVM::IW5::Hooks::Playback
{

    typedef void (*CL_Demo_Play_t)();
    CL_Demo_Play_t CL_Demo_Play_Trampoline = nullptr;
    void CL_Demo_Play_Hook()
    {
        CL_Demo_Play_Trampoline();

        Events::Invoke(EventType::PostDemoLoad);
        Events::Invoke(EventType::OnDemoBoundsDetermined);
    }


    // The client's realtime, 4 bytes before where clientStatic_t has it (see IW5Interface::GetPlaybackDataAddresses)
    std::int32_t& Realtime()
    {
        return *(&Structures::GetClientStatic()->realtime - 1);
    }

    // The editor jumps forward in a demo by adding to the client's realtime. Each time the game has read a snapshot it
    // checks, in its next frame, that its clock agrees with that snapshot (CL_AdjustTimeDelta, 0x4851F0 in 1.9.461),
    // and when the two are more than half a second apart it sets its clock back to the snapshot: the jump is undone.
    // That check is pending in the frame after any snapshot, and always after a rewind, which reads the demo's first
    // snapshots again. So a rewind went to the demo's start and stayed there. The check is dropped for a frame in
    // which the realtime has jumped.
    void KeepJump(const char* how, std::int32_t by)
    {
        auto cl = Structures::GetClientActive();
        if (cl->newSnapshots)
            LOG_DEBUG("Kept a jump of {} ms ({}) from being undone by the game's clock check", by, how);
        cl->newSnapshots = 0;
    }

    // How far the demo is past its last whole millisecond, from 0 to under 1. The game's clock counts whole
    // milliseconds, so at a timescale under about 0.1 it moves on only every few frames, and the picture moved in
    // steps: 20 a second at 0.02. The frames in between are given this, and CG_SetFrameInterpolation's hook adds it to
    // how far the game is between its two snapshots, which is what places the players and the view.
    std::atomic<float> partOfMillisecond = 0.0f;

    void UpdatePartOfMillisecond(std::int32_t delta, bool smooth)
    {
        static LARGE_INTEGER frequency = [] {
            LARGE_INTEGER value;
            QueryPerformanceFrequency(&value);
            return value;
        }();
        static LARGE_INTEGER last{};

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double frameMs =
            last.QuadPart ? static_cast<double>(now.QuadPart - last.QuadPart) * 1000.0 / frequency.QuadPart : 0.0;
        last = now;

        if (!smooth)
        {
            partOfMillisecond = 0.0f;
            return;
        }
        if (delta > 0)
        {
            // a whole millisecond (or more) was just given to the game
            partOfMillisecond = 0.0f;
            return;
        }
        if (Components::Playback::IsPaused() || Components::Playback::IsGameFrozen())
            return;  // held where it is

        const auto timescale = Mod::GetGameInterface()->GetDvar("timescale");
        if (!timescale.has_value())
            return;
        const auto next = partOfMillisecond.load() +
                          static_cast<float>(std::min(frameMs, 100.0)) * timescale.value().value->floating_point;
        partOfMillisecond = std::min(next, 0.98f);
    }

    typedef int32_t(__cdecl *Com_TimeScaleMsec_t)(int32_t msec);
    Com_TimeScaleMsec_t Com_TimeScaleMsec_Trampoline = nullptr;
    int32_t Com_TimeScaleMsec_Hook(std::int32_t msec)
    {
        static std::int32_t expectedRealtime = 0;
        const bool inDemo = Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;

        // a jump the editor made since the last frame (a click on the timeline, from its own thread)
        if (inDemo && expectedRealtime && Realtime() - expectedRealtime > 250)
            KeepJump("timeline", Realtime() - expectedRealtime);

        msec = Com_TimeScaleMsec_Trampoline(msec);
        const auto before = Realtime();
        const auto delta = Components::Playback::CalculatePlaybackDelta(msec);

        // the jump back to where the player clicked, after a rewind
        if (inDemo && Realtime() - before > 250)
            KeepJump("after a rewind", Realtime() - before);

        const bool smooth = inDemo && !Components::CaptureManager::Get().IsCapturing() &&
                            !Components::Rewinding::IsRewinding() && delta <= 1;
        UpdatePartOfMillisecond(delta, smooth);

        // (the game adds what is returned to its realtime in this frame)
        expectedRealtime = Realtime() + delta;
        return delta;
    }

    typedef void(__cdecl* CG_SetFrameInterpolation_t)();
    CG_SetFrameInterpolation_t CG_SetFrameInterpolation_Trampoline = nullptr;
    void CG_SetFrameInterpolation_Hook()
    {
        CG_SetFrameInterpolation_Trampoline();

        const float part = partOfMillisecond.load();
        if (part <= 0.0f)
            return;

        // snapshot_s's serverTime (the game reads it there itself: mov eax, [snap + 0x3314])
        constexpr std::size_t SNAPSHOT_SERVER_TIME = 0x3314;
        auto cg = Structures::GetClientGlobals();
        if (!cg->snap || !cg->nextSnap || cg->snap == cg->nextSnap)
            return;
        const auto from = *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const char*>(cg->snap) +
                                                                  SNAPSHOT_SERVER_TIME);
        const auto to = *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const char*>(cg->nextSnap) +
                                                                SNAPSHOT_SERVER_TIME);
        // (the game holds this at 1 while its own theater jumps about, and never below 0)
        if (to <= from || cg->frameInterpolation < 0.0f || cg->frameInterpolation >= 1.0f)
            return;
        cg->frameInterpolation += part / static_cast<float>(to - from);
    }

    typedef int (*CL_Demo_ReadInternal_t)(void* handle, void* buffer, int len);
    CL_Demo_ReadInternal_t CL_Demo_ReadInternal_Trampoline;
    int CL_Demo_ReadInternal_Hook(void* handle, void* buffer, int len)
    {
        const auto clc = Structures::GetClientConnection();
        if (handle != clc->clientDemo.demoFileHandle)
        {
            return CL_Demo_ReadInternal_Trampoline(handle, buffer, len);
        }

        auto result = Components::Rewinding::FS_Read(buffer, len);
        if (result == -1)
        {
            return CL_Demo_ReadInternal_Trampoline(handle, buffer, len);
        }

        return result;
    }

    typedef int (*CL_Demo_SeekFile_t)(void*, int, int);
    CL_Demo_SeekFile_t CL_Demo_SeekFile_Trampoline;
    int CL_Demo_SeekFile_Hook(void* handle, int offset, int origin)
    {
        const auto clc = Structures::GetClientConnection();
        if (handle != clc->clientDemo.demoFileHandle)
        {
            return CL_Demo_SeekFile_Trampoline(handle, offset, origin);
        }

        auto convertedOrigin = (origin + 1) % 3;

        auto result = Components::Rewinding::FS_Seek(offset, convertedOrigin);
        if (result == -1)
        {
            return CL_Demo_SeekFile_Trampoline(handle, offset, origin);
        }

        return result;
    }

    void Install()
    {
        HookManager::CreateHook(GetGameAddresses().CL_Demo_Play(), (std::uintptr_t)CL_Demo_Play_Hook,
                                (uintptr_t*)&CL_Demo_Play_Trampoline);

        HookManager::CreateHook(GetGameAddresses().Com_TimeScaleMsec(), (std::uintptr_t)Com_TimeScaleMsec_Hook,
                                (uintptr_t *)&Com_TimeScaleMsec_Trampoline);

        HookManager::CreateHook(GetGameAddresses().CG_SetFrameInterpolation(),
                                (std::uintptr_t)CG_SetFrameInterpolation_Hook,
                                (uintptr_t*)&CG_SetFrameInterpolation_Trampoline);

        HookManager::CreateHook(GetGameAddresses().CL_Demo_ReadInternal(), (std::uintptr_t)CL_Demo_ReadInternal_Hook,
                                (uintptr_t*)&CL_Demo_ReadInternal_Trampoline);

        HookManager::CreateHook(GetGameAddresses().CL_Demo_SeekFile(), (std::uintptr_t)CL_Demo_SeekFile_Hook,
                                (uintptr_t*)&CL_Demo_SeekFile_Trampoline);
    }
}  // namespace IWXMVM::IW5::Hooks::Playback
