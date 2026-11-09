#include "StdInclude.hpp"
#include "Playback.hpp"

#include "Components/DemoModLogic.hpp"
#include "Components/Playback.hpp"
#include "Components/Rewinding.hpp"
#include "Utilities/HookManager.hpp"
#include "Utilities/Patches.hpp"
#include "Events.hpp"
#include "../Addresses.hpp"
#include "../CampaignMaps.hpp"
#include "../ContentPacks.hpp"
#include "../DemoScanner.hpp"
#include "../Functions.hpp"
#include "../Structures.hpp"
#include "Rendering.hpp"

namespace IWXMVM::T5::Hooks::Playback
{
    std::string GetDemoRefusal(const std::filesystem::path& demoPath)
    {
        const auto info = DemoScanner::ReadInfo(demoPath);
        if (!info)
            return {};

        const auto fs_game = Functions::FindDvar("fs_game");
        const std::string loadedFsGame = fs_game && fs_game->current.string ? fs_game->current.string : "";
        const auto demoFsGame = Components::DemoModLogic::GetInfoValue(info->systemInfo, "fs_game");
        auto refusal = Components::DemoModLogic::GetRefusal(demoFsGame, loadedFsGame);
        if (!refusal.empty())
        {
            LOG_WARN("Not playing {0}, whose fs_game is '{1}' with '{2}' loaded: {3}", demoPath.filename().string(),
                     demoFsGame, loadedFsGame, refusal);
            return refusal;
        }

        // a map the game folder doesn't have, which ended the game as the film loaded it
        const std::string map(Components::DemoModLogic::GetInfoValue(info->serverInfo, "mapname"));
        refusal = CampaignMaps::GetMissingMapRefusal(map);
        if (!refusal.empty())
            LOG_WARN("Not playing {0}, recorded on {1}: {2}", demoPath.filename().string(), map, refusal);
        return refusal;
    }

    // The demo command's arguments and Com_Printf, from the calls CL_PlayDemo_f starts with (Sys_GetValue(4), the
    // command's arguments) and prints its usage with
    struct CmdArgs
    {
        std::int32_t nesting;
        std::int32_t perNesting[32];  // local client and controller numbers and more
        std::int32_t argc[8];         // +0x84
        const char** argv[8];         // +0xA4
    };
    static_assert(offsetof(CmdArgs, argc) == 0x84 && offsetof(CmdArgs, argv) == 0xA4);
    typedef CmdArgs*(__cdecl* Sys_GetValue_t)(std::int32_t valueIndex);
    Sys_GetValue_t Sys_GetValue = nullptr;
    typedef void(__cdecl* Com_Printf_t)(std::int32_t channel, const char* format, ...);
    Com_Printf_t Com_Printf = nullptr;
    // the channel CL_PlayDemo_f prints on
    constexpr std::int32_t CON_CHANNEL_CLIENT = 14;

    std::uintptr_t FindCallTarget(std::uintptr_t callAddress)
    {
        if (*reinterpret_cast<const std::uint8_t*>(callAddress) != 0xE8)
            return 0;
        return callAddress + 5 + *reinterpret_cast<const std::int32_t*>(callAddress + 1);
    }

    // the demo the command plays, as it finds it: in fs_homepath's demos folder, then fs_basepath's, with the
    // extension added when it lacks it
    std::optional<std::filesystem::path> FindCommandDemo()
    {
        if (!Sys_GetValue)
            return std::nullopt;
        const auto args = Sys_GetValue(4);
        if (!args || args->nesting < 0 || args->nesting >= 8 || args->argc[args->nesting] < 2)
            return std::nullopt;

        std::string name = args->argv[args->nesting][1];
        constexpr std::string_view EXTENSION = ".dm_8";
        if (!name.ends_with(EXTENSION))
            name += EXTENSION;
        const auto path = Functions::GetFilePath(name);
        if (path.empty())
            return std::nullopt;
        return path;
    }

    typedef void(__cdecl* CL_PlayDemo_f_t)();
    CL_PlayDemo_f_t CL_PlayDemo_f_Trampoline;
    void __cdecl CL_PlayDemo_f_Hook()
    {
        // A demo recorded with another mod (or with one, when none is loaded) is refused before the game reads it:
        // the game's own refusal ended the game once its dialog was closed
        if (const auto demoPath = FindCommandDemo())
        {
            if (const auto refusal = GetDemoRefusal(*demoPath); !refusal.empty())
            {
                if (Com_Printf)
                    Com_Printf(CON_CHANNEL_CLIENT, "^1%s didn't load.^7 %s\n", demoPath->filename().string().c_str(),
                               refusal.c_str());
                return;
            }
        }

        // what the file system mounts for the film's map as it starts again (CampaignMaps)
        if (const auto demoPath = FindCommandDemo())
            if (const auto info = DemoScanner::ReadInfo(*demoPath))
                CampaignMaps::PrepareMapLoad(
                    std::string(Components::DemoModLogic::GetInfoValue(info->serverInfo, "mapname")));

        // for the game console's demo command and the Demo tab's PLAY (which queues one), on the main thread
        Events::Invoke(EventType::PreDemoLoad);
        ContentPacks::UnloadCharacterZonesForDemo();

        CL_PlayDemo_f_Trampoline();

        Events::Invoke(EventType::PostDemoLoad);
    }

    std::int32_t __cdecl Com_TimeScaleMsec_Internal(std::int32_t msec)
    {
        // runs on the main thread once a frame
        Hooks::Rendering::RunMainThreadFrame();

        return Components::Playback::CalculatePlaybackDelta(msec);
    }

    // While a server runs, the game moves a frame on by com_maxFrameTime (100 ms) at most however long it took; without
    // one, and so while a demo plays, by up to 5 s. The frames that stall as a demo loads (shaders compiled the first
    // time things are drawn) or catches up after a skip then played seconds of it at once: the client went through a
    // hundred snapshots in one frame, which the client scripts don't take, and a skip landed
    // seconds late. So a demo's frames are held to com_maxFrameTime as well, before the timescale is applied (playing
    // it faster stays as fast).
    std::atomic<std::uint32_t> limitedFrames = 0;
    std::atomic<std::uint64_t> limitedMilliseconds = 0;

    std::int32_t __cdecl LimitFrameTime(std::int32_t msec)
    {
#ifdef IWXMVM_T5_HARNESS
        // IWXMVM_NO_FRAME_LIMIT leaves the frames as long as the game has them, for comparing (tools/t5-harness)
        static const bool limited = GetEnvironmentVariableA("IWXMVM_NO_FRAME_LIMIT", nullptr, 0) == 0;
        if (!limited)
            return msec;
#endif
        if (!Structures::GetClientConnection()->demoplaying)
            return msec;
        static const auto maxFrameTime = Functions::FindDvar("com_maxFrameTime");
        if (!maxFrameTime || msec <= maxFrameTime->current.integer)
            return msec;
        ++limitedFrames;
        limitedMilliseconds += msec - maxFrameTime->current.integer;
        return maxFrameTime->current.integer;
    }

    FrameLimitStats GetFrameLimitStats()
    {
        return {limitedFrames.load(), limitedMilliseconds.load()};
    }

    // usercall: msec is passed and returned in eax
    std::uintptr_t Com_TimeScaleMsec_Trampoline;
    void __declspec(naked) Com_TimeScaleMsec_Hook()
    {
        __asm
        {
            push ecx
            push edx
            push eax
            call LimitFrameTime
            add esp, 4
            pop edx
            pop ecx

            call Com_TimeScaleMsec_Trampoline

            push ecx
            push edx
            push eax
            call Com_TimeScaleMsec_Internal
            add esp, 4
            pop edx
            pop ecx
            ret
        }
    }

    typedef int(__cdecl* FS_Read_t)(void* buffer, int len, int f);
    FS_Read_t FS_Read_Trampoline;
    int __cdecl FS_Read_Hook(void* buffer, int len, int f)
    {
        const auto clc = Structures::GetClientConnection();
        if (!clc->demoplaying || f != clc->demofile)
        {
            return FS_Read_Trampoline(buffer, len, f);
        }

        auto result = Components::Rewinding::FS_Read(buffer, len);
        if (result == -1)
        {
            return FS_Read_Trampoline(buffer, len, f);
        }

        return result;
    }

    // Info strings are "\key1\value1\key2\value2..."
    // Plutonium uses its own protocol number, so this tells demos recorded on Steam apart from Plutonium's
    constexpr std::string_view STEAM_PROTOCOL = "2117";

    bool IsSteamDemo()
    {
        const auto serverInfo = Structures::GetConfigString(Structures::CS_SERVERINFO);
        return Components::DemoModLogic::GetInfoValue(serverInfo, "protocol") == STEAM_PROTOCOL;
    }

    // Plutonium servers send a list of strings in front of the client number at the end of the gamestate, and
    // Plutonium redirects the MSG_ReadLong call that reads the client number to a function that reads that list first.
    // Demos recorded on Steam don't have the list, so on Plutonium the client number is read from the wrong bytes
    // ("CL_ParseGamestate: bad clientNum") and the demo never starts. Those are read the way the game reads them.
    typedef std::int32_t(__cdecl* MSG_ReadLong_t)(void* msg);
    MSG_ReadLong_t ReadClientNum_Original;
    std::optional<Patches::Patch<5>> readClientNumCall;

    std::int32_t __cdecl CL_ParseGamestate_ReadClientNum_Hook(void* msg)
    {
        if (Structures::GetClientConnection()->demoplaying && IsSteamDemo())
        {
            const auto MSG_ReadLong = reinterpret_cast<MSG_ReadLong_t>(GetGameAddresses().MSG_ReadLong());
            return MSG_ReadLong(msg);
        }

        return ReadClientNum_Original(msg);
    }

    void RedirectReadClientNum()
    {
        // the call goes to MSG_ReadLong, or to Plutonium's replacement; on Steam there is nothing to redirect
        const auto callAddress = GetGameAddresses().CL_ParseGamestate_ReadClientNum();
        const auto callOffset = *reinterpret_cast<std::int32_t*>(callAddress + 1);
        ReadClientNum_Original = reinterpret_cast<MSG_ReadLong_t>(callAddress + 5 + callOffset);
        if (reinterpret_cast<std::uintptr_t>(ReadClientNum_Original) == GetGameAddresses().MSG_ReadLong())
            return;

        std::array<std::uint8_t, 5> call{0xE8};
        const auto hookOffset =
            static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(CL_ParseGamestate_ReadClientNum_Hook) -
                                      (callAddress + 5));
        std::memcpy(&call[1], &hookOffset, sizeof(hookOffset));

        // reverted when the mod is unloaded
        readClientNumCall.emplace(callAddress, call, Patches::PatchApplySetting::Immediately);
    }

    // A film's server can hand its dvars on in config strings (23 up), and the client sets all of them again each time
    // one changes (CG_ConfigStringModified). So a timescale the film's player used came in at its moment and with every
    // rewind past it, over the Speed. The Speed is the user's alone while a film plays.
    // (Plutonium redirects this call to a function of its own, which enters the game's setter past its first
    // instructions, so that setter can't be hooked.)
    typedef void(__cdecl* SetServerDvar_t)(const char* name, const char* value);
    SetServerDvar_t SetServerDvar_Original;
    std::optional<Patches::Patch<5>> setServerDvarCall;

    void __cdecl SetServerDvar_Hook(const char* name, const char* value)
    {
        if (Structures::GetClientConnection()->demoplaying && _stricmp(name, "timescale") == 0)
        {
            LOG_DEBUG("The film sets timescale to {}: left out", value);
            return;
        }
        SetServerDvar_Original(name, value);
    }

    void RedirectSetServerDvar()
    {
        const auto callAddress = GetGameAddresses().CG_ParseServerDvars_SetDvar();
        SetServerDvar_Original = reinterpret_cast<SetServerDvar_t>(FindCallTarget(callAddress));
        if (!SetServerDvar_Original)
        {
            LOG_ERROR("The call that sets a server's dvars isn't the code expected: a film can change the Speed");
            return;
        }

        std::array<std::uint8_t, 5> call{0xE8};
        const auto hookOffset = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(SetServerDvar_Hook) -
                                                          (callAddress + 5));
        std::memcpy(&call[1], &hookOffset, sizeof(hookOffset));

        // reverted when the mod is unloaded
        setServerDvarCall.emplace(callAddress, call, Patches::PatchApplySetting::Immediately);
    }

    bool IsPlaylistEntryAvailable(int entry)
    {
        typedef bool(__cdecl * LiveStorage_DoWeHavePlaylists_t)();
        const auto LiveStorage_DoWeHavePlaylists =
            reinterpret_cast<LiveStorage_DoWeHavePlaylists_t>(GetGameAddresses().LiveStorage_DoWeHavePlaylists());
        if (!LiveStorage_DoWeHavePlaylists())
            return false;

        const auto playlistDvar = Functions::FindDvar("playlist");
        if (!playlistDvar)
            return false;

        // like the game, fall back to the first playlist for playlist ids that don't exist
        const auto playlists = Structures::GetPlaylists();
        auto playlist = playlistDvar->current.integer;
        if (playlist < 0 || playlist >= Structures::MAX_PLAYLISTS || playlists[playlist].entryCount == 0)
            playlist = 1;

        return entry >= 0 && entry < playlists[playlist].entryCount;
    }

    // Demos recorded in playlist games (matchmaking and ranked dedicated servers) set up the rules of the playlist entry
    // they were recorded in when they start, like joining that match would. Without the downloaded playlists (Steam's
    // offline mode), or with playlists that no longer have that entry, the game falls back to an empty entry and drops
    // to the menu with "Unknown gametype name '' in playlist.". Plutonium has playlists of its own, so for Steam demos
    // the entry would be an unrelated one. The rules configure the match a server runs, which playback doesn't need, so
    // they are skipped in both cases.
    typedef void(__cdecl* Playlist_SetupRules_t)(int localClientNum, int entry);
    Playlist_SetupRules_t Playlist_SetupRules_Trampoline;
    void __cdecl Playlist_SetupRules_Hook(int localClientNum, int entry)
    {
        // the game returns right away for this one itself
        constexpr int NO_ENTRY = 0xFF;

        if (Structures::GetClientConnection()->demoplaying && entry != NO_ENTRY)
        {
            // the client number read is only redirected on Plutonium
            if (readClientNumCall && IsSteamDemo())
            {
                LOG_INFO("Skipping the rules of playlist entry {0}, which belongs to Steam's playlists", entry);
                return;
            }

            if (!IsPlaylistEntryAvailable(entry))
            {
                LOG_INFO("Skipping the rules of playlist entry {0}, which isn't loaded", entry);
                return;
            }
        }

        Playlist_SetupRules_Trampoline(localClientNum, entry);
    }

    void Install()
    {
        // (CL_PlayDemo_f: sub esp, 128h; push 4; call Sys_GetValue; ... push 0Eh; call Com_Printf)
        const auto playDemo = GetGameAddresses().CL_PlayDemo_f();
        Sys_GetValue = reinterpret_cast<Sys_GetValue_t>(FindCallTarget(playDemo + 0x8));
        Com_Printf = reinterpret_cast<Com_Printf_t>(FindCallTarget(playDemo + 0x4A));
        if (!Sys_GetValue || !Com_Printf)
            LOG_ERROR("CL_PlayDemo_f isn't the code expected: the demo command doesn't check the mod of demos");

        HookManager::CreateHook(GetGameAddresses().CL_PlayDemo_f(), (std::uintptr_t)CL_PlayDemo_f_Hook,
                                (std::uintptr_t*)&CL_PlayDemo_f_Trampoline);

        HookManager::CreateHook(GetGameAddresses().Com_TimeScaleMsec(), (std::uintptr_t)Com_TimeScaleMsec_Hook,
                                &Com_TimeScaleMsec_Trampoline);

        HookManager::CreateHook(GetGameAddresses().FS_Read(), (std::uintptr_t)FS_Read_Hook,
                                (std::uintptr_t*)&FS_Read_Trampoline);

        HookManager::CreateHook(GetGameAddresses().Playlist_SetupRules(), (std::uintptr_t)Playlist_SetupRules_Hook,
                                (std::uintptr_t*)&Playlist_SetupRules_Trampoline);

        RedirectReadClientNum();
        RedirectSetServerDvar();
    }
}  // namespace IWXMVM::T5::Hooks::Playback
