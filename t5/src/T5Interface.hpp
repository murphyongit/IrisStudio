#pragma once
#include "StdInclude.hpp"
#include "GameInterface.hpp"

#include "Structures.hpp"
#include "Functions.hpp"
#include "Hooks.hpp"
#include "Events.hpp"
#include "DemoParser.hpp"
#include "DemoScanner.hpp"
#include "ContentPacks.hpp"
#include "PoolGrowth.hpp"
#include "CampaignMaps.hpp"
#include "Sky.hpp"
#include "Surfaces.hpp"
#include "Textures.hpp"
#include "Actors.hpp"
#include "AnimationPose.hpp"
#include "Bones.hpp"
#include "WeaponRig.hpp"
#include "LiveActors.hpp"
#include "Hooks/Camera.hpp"
#include "Hooks/HUD.hpp"
#include "Hooks/Rendering.hpp"
#include "Hooks/Effects.hpp"
#include "Hooks/Playback.hpp"
#include "Hooks/PlayerAnimation.hpp"
#include "Addresses.hpp"
#include "Patches.hpp"
#include "Components/Rewinding.hpp"
#include "Components/DemoModLogic.hpp"
#ifdef IWXMVM_T5_HARNESS
#include "Harness.hpp"
#include "Diagnostics.hpp"
#endif

#include "glm/vec3.hpp"
#include "glm/gtc/type_ptr.hpp"

namespace IWXMVM::T5
{
    class T5Interface : public GameInterface
    {
       public:
        T5Interface() : GameInterface(Types::Game::T5)
        {
        }

        void ExecuteNewServerCommands() final
        {
            const auto clc = Structures::GetClientConnection();
            const auto cgs = Structures::GetClientGlobalsStatic();
            if (!cgs)
                return;

            const auto oldServerCommandSequence = cgs->serverCommandSequence;
            const auto newServerCommandSequence = clc->serverCommandSequence;

            // check if the command string backlog is equal to or greater than half the size of command string buffer (128 / 2 = 64)
            if (oldServerCommandSequence > 0 &&
                oldServerCommandSequence + std::ssize(clc->serverCommands) / 2 <= newServerCommandSequence)
            {
#ifdef IWXMVM_T5_HARNESS
                Diagnostics::OnEarlyServerCommands(oldServerCommandSequence, newServerCommandSequence);
#endif
                for (auto i = oldServerCommandSequence + 1; i <= newServerCommandSequence; ++i)
                {
                    if (static constexpr auto configString = 'd'; clc->serverCommands[i & 127][0] != configString)
                    {
                        // erase server commands that do not modify the gamestate strings
                        clc->serverCommands[i & 127][0] = '\0';
                    }
                }

                Functions::CG_ExecuteNewServerCommands(0, newServerCommandSequence);

                for (auto i = oldServerCommandSequence + 1; i <= newServerCommandSequence; ++i)
                {
                    // erase server commands to prevent double processing; shouldn't be necessary but just to be sure
                    clc->serverCommands[i & 127][0] = '\0';
                }
            }
        }

        void InstallHooksAndPatches() final
        {
            // The pools grow once the hooks are in, as a zone's load at the menu can hold that up (see PoolGrowth)
            PoolGrowth::Install();
            Hooks::Install();
            Patches::GetGamePatches();
            PoolGrowth::StartGrowing();
        }

        void SetupEventListeners() final
        {
            // Unlike IW5, Plutonium's wndproc and raw input registration are left in place: Plutonium T5 hooks
            // IN_MouseMove to feed in the raw input it collects there, so removing them stops the mouse entirely.
            // Our hook on the game's own wndproc still runs, since Plutonium passes messages on to it.

            Events::RegisterListener(EventType::PostDemoLoad, DemoParser::Run);
            // after DemoParser::Run, which finds where the demo's timeline starts
            Events::RegisterListener(EventType::PostDemoLoad, [&]() {
                const auto demoStartTime = DemoParser::GetDemoTickRange().first;
                if (demoStartTime > 0)
                    DemoScanner::Start(GetDemoInfo().path, demoStartTime);
            });
            Bones::Install();

            Events::RegisterListener(EventType::OnCameraChanged, Hooks::Camera::OnCameraChanged);

            // rewinding resets cg_thirdPerson and friends, which hides the player model in the free camera
            Events::RegisterListener(EventType::OnRewindCompleted, Hooks::Camera::OnCameraChanged);

            // A rewind puts back the configstrings of the demo's start without the client hearing of it, so the vision
            // stayed the one it had: a film recorded from the countdown before the match (multiplayer's black and
            // white mpIntro) showed the map's own after a jump back to it. The client sets the visions of the
            // configstrings put back, as it does as a map starts; the skip that follows the rewind plays the changes
            // after that
            Events::RegisterListener(EventType::OnRewindCompleted, [] {
                if (Structures::GetClientGlobals())
                    Functions::CG_ApplyVisionConfigStrings(0);
            });

            Events::RegisterListener(EventType::PostDemoLoad, [&]() {
                if (auto sv_cheats = Functions::FindDvar("sv_cheats"))
                    sv_cheats->current.enabled = true;
            });

            // the game can still ask to hide the cursor while the overlay has the mouse
            Events::RegisterListener(EventType::OnFrame, [&]() {
                if (systemCursor.forced)
                    *reinterpret_cast<std::int32_t*>(GetGameAddresses().systemCursorVisible()) = 1;
            });
        }

        // The actors' DObjs are the game's, but their trees have animations made up by the mod and their magazines
        // are models copied by it, so the main thread, which builds and draws them, stops doing so and leaves those to
        // the process, as it does the copies of a pack's headers, which its zone points to until the map changes.
        void PrepareUnload() final
        {
            PoolGrowth::Stop();
#ifdef IWXMVM_T5_HARNESS
            if (Harness::SkipsPrepareUnload())
                return;
#endif
            // (the game's own size of the sun's shadow map from the next restart of the renderer on)
            Hooks::Rendering::SetSunShadowMapSize(Hooks::Rendering::GAME_SUN_SHADOW_MAP_SIZE, false);
            const auto prepared = Hooks::Rendering::RunOnMainThread(
                [] {
                    Actors::Unload();
                    ContentPacks::Unload();
                },
                std::chrono::seconds(10));
            if (prepared)
                LOG_INFO("Stopped the actors and left them and the content pack to the game");
            else
                LOG_ERROR("The game's thread didn't run in time to stop the actors before unloading");
        }

        IDirect3DDevice9* GetGameDevicePtr() const final
        {
            return **(IDirect3DDevice9***)GetGameAddresses().d3d9DevicePointer();
        }

        uintptr_t GetWndProc() final
        {
            return (uintptr_t)GetGameAddresses().MainWndProc();
        }

        void SetMouseMode(Types::MouseMode mode) final
        {
            if (mode == Types::MouseMode::Capture)
            {
                Patches::GetGamePatches().IN_Frame.Apply();
                DetachPlutoniumInput();
                ForceSystemCursor(true);
            }
            else
            {
                Patches::GetGamePatches().IN_Frame.Revert();
                AttachPlutoniumInput();
                ForceSystemCursor(false);
            }
        }

        // T5's render thread sets the cursor's show count every frame to what the game last asked for, and the game
        // hides the cursor while it has the mouse, e.g. during demo playback. Core shows the cursor again on every
        // window message while the overlay is up, so without this the cursor blinks.
        struct SystemCursor
        {
            bool forced = false;
            std::int32_t gameValue = 1;
        } systemCursor;

        void ForceSystemCursor(bool force)
        {
            auto& visible = *reinterpret_cast<std::int32_t*>(GetGameAddresses().systemCursorVisible());
            if (force && !systemCursor.forced)
            {
                systemCursor = {true, visible};
                visible = 1;
            }
            else if (!force && systemCursor.forced)
            {
                visible = systemCursor.gameValue;
                systemCursor.forced = false;
            }
        }

        // Plutonium T5 subclasses the game window and feeds raw mouse input into IN_MouseMove. While the overlay has
        // the mouse, that keeps grabbing the cursor, so it is detached then and restored when the game gets the
        // mouse back.
        struct PlutoniumInput
        {
            bool detached = false;
            HWND window = nullptr;
            LONG_PTR wndProc = 0;
            std::vector<RAWINPUTDEVICE> mouseDevices;
        } plutoniumInput;

        void DetachPlutoniumInput()
        {
            if (plutoniumInput.detached)
                return;

            const HWND window = FindGameWindow();
            if (!window)
                return;

            const LONG_PTR wndProc = GetWindowLongPtr(window, GWLP_WNDPROC);
            if (!wndProc || static_cast<uintptr_t>(wndProc) == GetWndProc())
                return;

            UINT count = 0;
            GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
            std::vector<RAWINPUTDEVICE> devices(count);
            if (count && GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1)
                count = 0;
            devices.resize(count);

            plutoniumInput.mouseDevices.clear();
            for (const auto& device : devices)
            {
                constexpr USHORT HID_USAGE_PAGE_GENERIC = 1;
                constexpr USHORT HID_USAGE_GENERIC_MOUSE = 2;
                if (device.usUsagePage == HID_USAGE_PAGE_GENERIC && device.usUsage == HID_USAGE_GENERIC_MOUSE)
                    plutoniumInput.mouseDevices.push_back(device);
            }

            SetWindowLongPtr(window, GWLP_WNDPROC, static_cast<LONG_PTR>(GetWndProc()));

            for (auto device : plutoniumInput.mouseDevices)
            {
                device.dwFlags = RIDEV_REMOVE;
                device.hwndTarget = nullptr;
                RegisterRawInputDevices(&device, 1, sizeof(device));
            }

            plutoniumInput = {true, window, wndProc, plutoniumInput.mouseDevices};
            LOG_DEBUG("Detached Plutonium input (wndproc {0:X}, {1} raw mouse device(s))", wndProc,
                      plutoniumInput.mouseDevices.size());
        }

        void AttachPlutoniumInput()
        {
            if (!plutoniumInput.detached)
                return;

            plutoniumInput.detached = false;

            // the window may have been recreated, in which case Plutonium has set up the new one itself
            if (!IsWindow(plutoniumInput.window) ||
                static_cast<uintptr_t>(GetWindowLongPtr(plutoniumInput.window, GWLP_WNDPROC)) != GetWndProc())
                return;

            SetWindowLongPtr(plutoniumInput.window, GWLP_WNDPROC, plutoniumInput.wndProc);

            if (!plutoniumInput.mouseDevices.empty())
            {
                RegisterRawInputDevices(plutoniumInput.mouseDevices.data(),
                                        static_cast<UINT>(plutoniumInput.mouseDevices.size()), sizeof(RAWINPUTDEVICE));
            }

            LOG_DEBUG("Restored Plutonium input");
        }

        Types::GameState GetGameState() final
        {
            // cl_ingame is set every frame from connectionState == CA_ACTIVE
            const auto cl_ingame = Functions::FindDvar("cl_ingame");
            if (!cl_ingame || !cl_ingame->current.enabled)
                return Types::GameState::MainMenu;

            if (Structures::GetClientConnection()->demoplaying)
                return Types::GameState::InDemo;

            return Types::GameState::InGame;
        }

        void InitializeGameAddresses() final
        {
            char mainModuleName[MAX_PATH];
            GetModuleFileName(NULL, mainModuleName, MAX_PATH);

            const bool isRunningPluto =
                std::string(mainModuleName).find("plutonium-bootstrapper-win32.exe") != std::string::npos;

            MODULEINFO moduleInfo{};
            GetModuleInformation(GetCurrentProcess(), GetModuleHandle(NULL), &moduleInfo, sizeof(moduleInfo));

            LOG_DEBUG("Main module: {0} (base {1}, size {2:X})", mainModuleName, moduleInfo.lpBaseOfDll,
                      moduleInfo.SizeOfImage);
            LOG_DEBUG("Using Plutonium: {}", isRunningPluto);

            // The mod may be injected right after the process starts. Plutonium maps the game into its bootstrapper
            // some time after that, so scanning too early would read memory that isn't the game yet.
            WaitForGameWindow();

            try
            {
                GetGameAddresses();
            }
            catch (std::exception& ex)
            {
                LOG_ERROR("Failed to find required signature: {}", ex.what());
                MessageBoxA(NULL,
                            "It seems like you are running an unsupported game version.\n\nT5iris supports Call of "
                            "Duty: Black Ops multiplayer (BlackOpsMP.exe, latest Steam version) and Plutonium T5 "
                            "multiplayer. Singleplayer and Zombies (BlackOps.exe) are not supported.",
                            "Unsupported game version", MB_OK);
                ExitProcess(0);
            }

            // core reads the game's device right away when hooking D3D9
            WaitForGameDevice();
        }

        static constexpr auto GAME_WINDOW_CLASS = "CoDBlackOps";

        // The game registers this window class itself, so its window existing in this process means the game code
        // is mapped and running.
        static HWND FindGameWindow()
        {
            struct WindowSearch
            {
                DWORD processId;
                HWND window;
            } search{GetCurrentProcessId(), nullptr};

            EnumWindows(
                [](HWND hwnd, LPARAM lParam) -> BOOL {
                    auto search = reinterpret_cast<WindowSearch*>(lParam);

                    DWORD processId = 0;
                    GetWindowThreadProcessId(hwnd, &processId);
                    if (processId != search->processId)
                        return TRUE;

                    char className[64]{};
                    if (GetClassNameA(hwnd, className, sizeof(className)) && strcmp(className, GAME_WINDOW_CLASS) == 0)
                    {
                        search->window = hwnd;
                        return FALSE;
                    }

                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&search));

            return search.window;
        }

        void WaitForGameWindow()
        {
            if (FindGameWindow())
                return;

            LOG_INFO("Waiting for the game window...");
            while (!FindGameWindow())
                Sleep(100);
        }

        void WaitForGameDevice()
        {
            if (GetGameDevicePtr())
                return;

            LOG_INFO("Waiting for the game to create its D3D9 device...");
            while (!GetGameDevicePtr())
                Sleep(100);
        }

        std::string cachedServerName;
        std::string cachedDemoPath;
        Types::DemoInfo demoInfo{};

        Types::DemoInfo GetDemoInfo() final
        {
            const std::string serverName = Structures::GetClientStatic()->servername;
            const std::string tempDirectoryPrefix = std::string(DEMO_TEMP_DIRECTORY) + "/";

            demoInfo.name = serverName.starts_with(tempDirectoryPrefix) ? serverName.substr(tempDirectoryPrefix.size())
                                                                        : serverName;

            // this gets called every frame, so avoid hitting the file system each time
            if (serverName != cachedServerName || cachedDemoPath.empty())
            {
                cachedServerName = serverName;

                std::string demoName = serverName;
                demoName += (demoName.ends_with(GetDemoExtension())) ? "" : GetDemoExtension();
                cachedDemoPath = Functions::GetFilePath(demoName);
            }
            demoInfo.path = cachedDemoPath;

            auto [demoStartTick, demoEndTick] = DemoParser::GetDemoTickRange();

            const auto serverTime = Structures::GetClientActive()->serverTime;
            if (serverTime > demoStartTick && serverTime < demoEndTick && !Components::Rewinding::IsRewinding())
            {
                demoInfo.gameTick = serverTime - demoStartTick;
            }
            demoInfo.endTick = demoEndTick - demoStartTick;

            return demoInfo;
        }

        std::string_view GetDemoExtension() final
        {
            return {".dm_8"};
        }

        std::string_view GetClientName() final
        {
            static const bool plutonium =
                _stricmp(std::filesystem::path(PathUtils::GetCurrentExecutablePath()).filename().string().c_str(),
                         "plutonium-bootstrapper-win32.exe") == 0;
            return plutonium ? "Plutonium T5" : "";
        }

        // On Plutonium the executable's folder is Plutonium's bin folder, and files the mod kept there were gone at the
        // next launch (config.json with the demo search paths, keyframes). Plutonium keeps the player's own files
        // (configs, demos) in fs_homepath, storage\t5, so the mod's files go there as well. Steam keeps the default.
        std::filesystem::path GetUserDirectory() final
        {
            static const auto userDirectory = [] {
                const auto exePath = std::filesystem::path(PathUtils::GetCurrentExecutablePath());
                if (_stricmp(exePath.filename().string().c_str(), "plutonium-bootstrapper-win32.exe") != 0)
                    return exePath.parent_path();

                const auto fs_homepath = Functions::FindDvar("fs_homepath");
                if (!fs_homepath || !fs_homepath->current.string || !*fs_homepath->current.string)
                {
                    LOG_ERROR("fs_homepath isn't set, keeping the mod's files next to the executable");
                    return exePath.parent_path();
                }

                return std::filesystem::path(fs_homepath->current.string);
            }();

            return userDirectory;
        }

        std::filesystem::path GetDemoDirectory()
        {
            // the demo command looks in fs_homepath first, see Functions::GetFilePath
            return std::filesystem::path(GetDvar("fs_homepath")->value->string) / "demos";
        }

        // On the render thread (the Demo tab's PLAY). PreDemoLoad is raised as the demo command starts, on the main
        // thread (see Hooks::Playback): its listeners free the actors' models the main thread draws, close the demo
        // file it reads and clear what it uses (raised here, the main thread crashed drawing freed actors).
        void PlayDemo(std::filesystem::path demoPath) final
        {
            const auto demoDirectory = GetDemoDirectory();
            playDemoError.clear();

            try
            {
                LOG_INFO("Playing demo {0}", demoPath.string());

                if (!std::filesystem::exists(demoPath) || !std::filesystem::is_regular_file(demoPath))
                {
                    playDemoError = "the file isn't there";
                    return;
                }

                // (the game's own refusal ended the game)
                if (auto refusal = Hooks::Playback::GetDemoRefusal(demoPath); !refusal.empty())
                {
                    playDemoError = std::move(refusal);
                    return;
                }

                const auto tempDemoDirectory = demoDirectory / DEMO_TEMP_DIRECTORY;
                if (!std::filesystem::exists(tempDemoDirectory))
                    std::filesystem::create_directories(tempDemoDirectory);

                const auto targetPath = tempDemoDirectory / demoPath.filename();
                if (std::filesystem::exists(targetPath) && std::filesystem::is_regular_file(targetPath))
                    std::filesystem::remove(targetPath);

                std::filesystem::copy(demoPath, targetPath);

                Functions::Cbuf_AddText(
                    std::format(R"(demo "{0}/{1}")", DEMO_TEMP_DIRECTORY, targetPath.filename().string()));
            }
            catch (std::filesystem::filesystem_error& e)
            {
                LOG_ERROR("Failed to play demo file {0}: {1}", demoPath.string(), e.what());
                playDemoError = e.code().message();
            }
        }

        std::string GetPlayDemoError() final
        {
            return playDemoError;
        }

        std::optional<std::string> GetLoadedFsGame() final
        {
            const auto fs_game = Functions::FindDvar("fs_game");
            return fs_game && fs_game->current.string ? fs_game->current.string : "";
        }

        std::optional<std::string> GetDemoFsGame(const std::filesystem::path& demoPath) final
        {
            const auto info = DemoScanner::ReadInfo(demoPath);
            if (!info)
                return std::nullopt;
            return std::string(Components::DemoModLogic::GetInfoValue(info->systemInfo, "fs_game"));
        }

        // the game's file system reads mods/<name> under both
        std::vector<std::filesystem::path> GetModFolders() final
        {
            std::vector<std::filesystem::path> folders;
            for (const auto name : {"fs_homepath", "fs_basepath"})
            {
                const auto dvar = Functions::FindDvar(name);
                if (dvar && dvar->current.string && *dvar->current.string)
                    folders.push_back(std::filesystem::path(dvar->current.string) / "mods");
            }
            return folders;
        }

        // Plutonium's console command, at the menu (checked 2026-11-01: about 7 s, the renderer made again)
        void LoadMod(std::string_view fsGame) final
        {
            LOG_INFO("Loading mod '{}'", fsGame);
            Functions::Cbuf_AddText(std::format(R"(loadmod "{}")", fsGame));
        }

        std::optional<std::uint32_t> GetMainThreadFrames() final
        {
            return Hooks::Rendering::GetMainThreadFrames();
        }

        // reading it at the menu: from about 0.15 s after the demo command until the demo plays
        std::optional<bool> IsReadingDemo() final
        {
            const auto cl_ingame = Functions::FindDvar("cl_ingame");
            const auto connection = Structures::GetClientConnection();
            return connection && connection->demoplaying && !(cl_ingame && cl_ingame->current.enabled);
        }

        void Disconnect() final
        {
            Functions::Cbuf_AddText("disconnect");
        }

        void Quit() final
        {
            Functions::Cbuf_AddText("quit");
        }

        // Core restarts the renderer when it hooks D3D9 without ReShade, so that the game creates its device and depth
        // buffer again through core's hooks. T5's vid_restart only resets the device, so the depth buffer isn't
        // intercepted either way, and on the Steam version the restart froze the game for about ten seconds.
        void Vid_Restart() final
        {
        }

        bool IsPlayingDemo() final
        {
            return Structures::GetClientConnection()->demoplaying != 0;
        }

        std::optional<Types::DrawnView> GetDrawnView() final
        {
            return Hooks::Camera::GetDrawnView();
        }

        std::shared_ptr<const Types::LevelAreas> GetLevelAreas() final
        {
            return CampaignMaps::GetLevelAreas();
        }

        glm::vec3 GetPlayerPosition() final
        {
            const auto cg = Structures::GetClientGlobals();
            if (!cg)
                return glm::vec3(0.0f);
            const auto& origin = cg->predictedPlayerState.origin;
            return glm::vec3(origin[0], origin[1], origin[2]);
        }

        void MovePlayer(const glm::vec3& eye, float yaw) final
        {
            // (a toggle, and the player state says which way it stands)
            constexpr std::int32_t PM_NOCLIP = 2;
            const auto cg = Structures::GetClientGlobals();
            if (cg && cg->predictedPlayerState.pmType != PM_NOCLIP)
                Functions::Cbuf_AddText("noclip");
            Functions::Cbuf_AddText(std::format("setviewpos {} {} {} {} 0", eye.x, eye.y, eye.z, yaw));
        }

        bool IsHostingLiveGame() final
        {
            return LiveActors::IsLive();
        }

        bool IsConsoleOpen() final
        {
            constexpr std::int32_t KEYCATCH_CONSOLE = 1;
            return (*Structures::GetKeyCatchers() & KEYCATCH_CONSOLE) != 0;
        }

        // CL_KeyEvent gives keys to these instead of the binds: the console, a menu (the main menu has one, a demo
        // none) and the chat field
        bool IsGameUITakingKeys() final
        {
            constexpr std::int32_t KEYCATCH_CONSOLE = 1;
            constexpr std::int32_t KEYCATCH_UI = 0x10;
            constexpr std::int32_t KEYCATCH_MESSAGE = 0x20;
            return (*Structures::GetKeyCatchers() & (KEYCATCH_CONSOLE | KEYCATCH_UI | KEYCATCH_MESSAGE)) != 0;
        }

        std::optional<Types::Dvar> GetDvar(const std::string_view name) final
        {
            // T5 has no r_znear dvar. 4 is the IW3 default; the T5 near plane hasn't been verified yet.
            if (name.compare("r_znear") == 0)
            {
                static float znear = 4.0f;
                return Types::Dvar{name, (Types::Dvar::Value*)&znear};
            }

            // Core turns r_smp_backend off when its UI starts. T5 has to keep it on: rendering on the main thread
            // deadlocks inside D3D9 (seen even when the game is started with r_smp_backend 0), so hand core a
            // stand-in. The render thread also owns the game window, so core's UI and the window messages it handles
            // still share a thread; only the camera has to be handed over to the main thread (see Hooks/Camera.cpp).
            if (name.compare("r_smp_backend") == 0)
            {
                static std::int32_t smpBackend = 0;
                return Types::Dvar{name, (Types::Dvar::Value*)&smpBackend};
            }

            const auto gameDvar = Functions::FindDvar(name);

            if (!gameDvar)
                return std::nullopt;

            Types::Dvar dvar;
            dvar.name = gameDvar->name;
            dvar.value = (Types::Dvar::Value*)&gameDvar->current;

            return dvar;
        }

        void SetFov(float fov) final
        {
            SetDvarFloat("cg_fov", fov);
        }

        // Dvar helpers. Not every IW3 dvar exists on T5, and some have different types, so every access checks
        // that the dvar exists and has the expected type before touching its value. Setters only write (and mark
        // the dvar as modified) when the value changes, since core applies the HUD settings every frame.
        Structures::dvar_t* FindDvarOfType(std::string_view name, Structures::dvarType_t type)
        {
            const auto dvar = Functions::FindDvar(name);
            return (dvar && dvar->type == type) ? dvar : nullptr;
        }

        static bool IsFloatVectorType(std::int32_t type)
        {
            return type == Structures::DVAR_TYPE_FLOAT_2 || type == Structures::DVAR_TYPE_FLOAT_3 ||
                   type == Structures::DVAR_TYPE_FLOAT_4 || type == Structures::DVAR_TYPE_LINEAR_COLOR_RGB;
        }

        // which of a dvar's values to read
        using DvarField = Structures::DvarValue Structures::dvar_t::*;

        bool GetDvarBool(std::string_view name, bool fallback, DvarField field = &Structures::dvar_t::current)
        {
            const auto dvar = FindDvarOfType(name, Structures::DVAR_TYPE_BOOL);
            return dvar ? (dvar->*field).enabled : fallback;
        }

        void SetDvarBool(std::string_view name, bool value)
        {
            auto dvar = FindDvarOfType(name, Structures::DVAR_TYPE_BOOL);
            if (dvar && dvar->current.enabled != value)
            {
                dvar->current.enabled = value;
                dvar->modified = true;
            }
        }

        float GetDvarFloat(std::string_view name, float fallback, DvarField field = &Structures::dvar_t::current)
        {
            const auto dvar = FindDvarOfType(name, Structures::DVAR_TYPE_FLOAT);
            return dvar ? (dvar->*field).value : fallback;
        }

        void SetDvarFloat(std::string_view name, float value)
        {
            auto dvar = FindDvarOfType(name, Structures::DVAR_TYPE_FLOAT);
            if (dvar && dvar->current.value != value)
            {
                dvar->current.value = value;
                dvar->modified = true;
            }
        }

        glm::vec3 GetDvarVec3(std::string_view name, glm::vec3 fallback, DvarField field = &Structures::dvar_t::current)
        {
            const auto dvar = Functions::FindDvar(name);
            if (!dvar || !IsFloatVectorType(dvar->type))
                return fallback;

            return glm::make_vec3((dvar->*field).vector);
        }

        void SetDvarVec3(std::string_view name, glm::vec3 value)
        {
            const auto dvar = Functions::FindDvar(name);
            if (!dvar || !IsFloatVectorType(dvar->type) || glm::make_vec3(dvar->current.vector) == value)
                return;

            for (int i = 0; i < 3; ++i)
                dvar->current.vector[i] = value[i];
            dvar->modified = true;
        }

        glm::vec3 GetDvarColor(std::string_view name, glm::vec3 fallback)
        {
            const auto dvar = FindDvarOfType(name, Structures::DVAR_TYPE_COLOR);
            if (!dvar)
                return fallback;

            return glm::vec3(dvar->current.color[0], dvar->current.color[1], dvar->current.color[2]) / 255.0f;
        }

        void SetDvarColor(std::string_view name, glm::vec3 value)
        {
            auto dvar = FindDvarOfType(name, Structures::DVAR_TYPE_COLOR);
            if (!dvar)
                return;

            for (int i = 0; i < 3; ++i)
            {
                const auto component = static_cast<std::uint8_t>(std::clamp(value[i], 0.0f, 1.0f) * 255);
                if (dvar->current.color[i] != component)
                {
                    dvar->current.color[i] = component;
                    dvar->modified = true;
                }
            }
        }

        Types::Sun GetSun() final
        {
            return ReadSun(&Structures::dvar_t::current);
        }

        Types::Sun ReadSun(DvarField field)
        {
            Types::Sun sun;
            sun.color = GetDvarVec3("r_lightTweakSunColor", glm::vec3(1.0f), field);
            sun.direction = GetDvarVec3("r_lightTweakSunDirection", glm::vec3(0.0f), field);
            sun.brightness = GetDvarFloat("r_lightTweakSunLight", 1.0f, field);
            return sun;
        }

        // Each world's load (0x719F90) copies its own sun (GfxWorld.sunParse) into the r_lightTweakSun* dvars and makes
        // that their reset value (0x719D40), so reset is the map's whatever keys or the tab did to them after. The
        // game never writes the depth of field's and film's tweak dvars: theirs are the same on every map.
        std::optional<Types::MapVisuals> GetMapVisuals() final
        {
            constexpr DvarField RESET = &Structures::dvar_t::reset;
            return Types::MapVisuals{ReadSun(RESET), ReadDof(RESET), ReadFilmtweaks(RESET)};
        }

        void SetSun(Types::Sun sun) final
        {
            SetDvarVec3("r_lightTweakSunColor", sun.color);
            SetDvarVec3("r_lightTweakSunDirection", sun.direction);
            SetDvarFloat("r_lightTweakSunLight", sun.brightness);
        }

        Types::DoF GetDof() final
        {
            return ReadDof(&Structures::dvar_t::current);
        }

        Types::DoF ReadDof(DvarField field)
        {
            Types::DoF dof = {
                GetDvarBool("r_dof_tweak", false, field) && GetDvarBool("r_dof_enable", false, field),
                GetDvarFloat("r_dof_farBlur", 0.0f, field),
                GetDvarFloat("r_dof_farStart", 0.0f, field),
                GetDvarFloat("r_dof_farEnd", 0.0f, field),
                GetDvarFloat("r_dof_nearBlur", 0.0f, field),
                GetDvarFloat("r_dof_nearStart", 0.0f, field),
                GetDvarFloat("r_dof_nearEnd", 0.0f, field),
                GetDvarFloat("r_dof_bias", 0.0f, field),
            };

            return dof;
        }

        void SetDof(Types::DoF dof) final
        {
            SetDvarBool("r_dof_tweak", dof.enabled);
            SetDvarBool("r_dof_enable", dof.enabled);

            SetDvarFloat("r_dof_farBlur", dof.farBlur);
            SetDvarFloat("r_dof_farStart", dof.farStart);
            SetDvarFloat("r_dof_farEnd", dof.farEnd);
            SetDvarFloat("r_dof_nearBlur", dof.nearBlur);
            SetDvarFloat("r_dof_nearStart", dof.nearStart);
            SetDvarFloat("r_dof_nearEnd", dof.nearEnd);
            SetDvarFloat("r_dof_bias", dof.bias);
        }

        Types::Filmtweaks GetFilmtweaks() final
        {
            return ReadFilmtweaks(&Structures::dvar_t::current);
        }

        Types::Filmtweaks ReadFilmtweaks(DvarField field)
        {
            // T5's contrast is per range (r_filmTweakContrast is a vec3), so the single contrast value stays neutral
            Types::Filmtweaks filmtweaks = {
                GetDvarBool("r_filmUseTweaks", false, field) && GetDvarBool("r_filmTweakEnable", false, field),
                GetDvarFloat("r_filmTweakBrightness", 0.0f, field),
                1.0f,
                GetDvarFloat("r_filmTweakDesaturation", 0.2f, field),
                GetDvarVec3("r_filmTweakLightTint", glm::vec3(1.0f), field),
                GetDvarVec3("r_filmTweakDarkTint", glm::vec3(1.0f), field),
                GetDvarBool("r_filmTweakInvert", false, field),
            };

            filmtweaks.contrastRanges = GetDvarVec3("r_filmTweakContrast", glm::vec3(1.0f), field);
            filmtweaks.saturationRanges = GetDvarVec3("r_filmTweakSaturation", glm::vec3(1.0f), field);
            filmtweaks.hueRanges = GetDvarVec3("r_filmTweakHue", glm::vec3(0.0f), field);
            filmtweaks.colorTemperatureRanges = GetDvarVec3("r_filmTweakColorTemp", glm::vec3(6500.0f), field);
            filmtweaks.bleachRanges = GetDvarVec3("r_filmTweakBleach", glm::vec3(0.0f), field);
            filmtweaks.tintMid = GetDvarVec3("r_filmTweakMidTint", glm::vec3(1.0f), field);
            filmtweaks.midStart = GetDvarFloat("r_filmTweakMidStart", 0.25f, field);
            filmtweaks.midEnd = GetDvarFloat("r_filmTweakMidEnd", 0.75f, field);
            filmtweaks.darkFeather = GetDvarFloat("r_filmTweakDarkFeather", 0.0f, field);
            filmtweaks.lightFeather = GetDvarFloat("r_filmTweakLightFeather", 0.0f, field);

            return filmtweaks;
        }

        void SetFilmtweaks(Types::Filmtweaks filmtweaks) final
        {
            SetDvarBool("r_filmUseTweaks", filmtweaks.enabled);
            SetDvarBool("r_filmTweakEnable", filmtweaks.enabled);
            SetDvarFloat("r_filmTweakBrightness", filmtweaks.brightness);
            SetDvarFloat("r_filmTweakDesaturation", filmtweaks.desaturation);
            SetDvarVec3("r_filmTweakLightTint", filmtweaks.tintLight);
            SetDvarVec3("r_filmTweakDarkTint", filmtweaks.tintDark);
            SetDvarBool("r_filmTweakInvert", filmtweaks.invert);

            SetDvarVec3("r_filmTweakContrast", filmtweaks.contrastRanges);
            SetDvarVec3("r_filmTweakSaturation", filmtweaks.saturationRanges);
            SetDvarVec3("r_filmTweakHue", filmtweaks.hueRanges);
            SetDvarVec3("r_filmTweakColorTemp", filmtweaks.colorTemperatureRanges);
            SetDvarVec3("r_filmTweakBleach", filmtweaks.bleachRanges);
            SetDvarVec3("r_filmTweakMidTint", filmtweaks.tintMid);
            SetDvarFloat("r_filmTweakMidStart", filmtweaks.midStart);
            SetDvarFloat("r_filmTweakMidEnd", filmtweaks.midEnd);
            SetDvarFloat("r_filmTweakDarkFeather", filmtweaks.darkFeather);
            SetDvarFloat("r_filmTweakLightFeather", filmtweaks.lightFeather);
        }

        Types::Features GetSupportedFeatures() final
        {
            return static_cast<Types::Features>(Types::Features_FilmtweakRanges | Types::Features_ChangeAnimations |
                                                Types::Features_DemoEvents | Types::Features_RenderSize |
                                                Types::Features_ChangeLoadouts | Types::Features_Actors |
                                                Types::Features_ShadowQuality | Types::Features_SurfaceDepth |
                                                Types::Features_TexturePack | Types::Features_Sky);
        }

        void LoadContentPack(const std::string& name) final
        {
            ContentPacks::RequestLoad(name);
        }

        // vid_restart resets the device at the new size, see Hooks::Rendering
        bool SetRenderSize(std::optional<std::pair<std::int32_t, std::int32_t>> size) final
        {
            return Hooks::Rendering::SetRenderSizeOverride(size, FindGameWindow());
        }

        std::optional<std::int32_t> GetSunShadowMapSize() final
        {
            return Hooks::Rendering::GetSunShadowMapSize();
        }

        bool SetSunShadowMapSize(std::int32_t size, bool restart) final
        {
            Hooks::Rendering::SetSunShadowMapSize(size, restart);
            return true;
        }

        std::uint32_t CountRefusedEffects() final
        {
            return Hooks::Effects::CountRefused();
        }

        std::optional<Types::MapEffects> GetMapEffects() final
        {
            return CampaignMaps::GetMapEffects();
        }

        void SetHiddenMapEffects(const std::string& level, const std::set<std::string>& definitions) final
        {
            CampaignMaps::SetHiddenMapEffects(level, definitions);
        }

        std::vector<Types::NormalMap> GetMapNormalMaps() final
        {
            return Surfaces::GetMapNormalMaps();
        }

        std::vector<Types::SkyImage> GetSkyImages() final
        {
            return Sky::GetSkyImages();
        }

        std::optional<Types::PackImages> GetPackImages() final
        {
            std::optional<Types::PackImages> images;
            if (!Hooks::Rendering::RunOnMainThread([&] { images = Textures::GetPackImages(); },
                                                   std::chrono::seconds(5)))
                return std::nullopt;
            return images;
        }

        std::string GetLoadedZonesKey() final
        {
            std::string key;
            for (const auto& zone : ContentPacks::GetLoadedZones() | std::views::keys)
                key += zone + ";";
            return key;
        }

        std::shared_ptr<const Types::DemoEvents> GetDemoEvents() final
        {
            return DemoScanner::GetResult();
        }

        Types::HudInfo GetHudInfo() final
        {
            Types::HudInfo hudInfo = {
                GetDvarBool("cg_draw2D", true),
                Hooks::HUD::showPlayerHUD,
                Hooks::HUD::showShellshock,
                GetDvarBool("ui_drawCrosshair", true),
                Hooks::HUD::showScore,
                Hooks::HUD::showIconsAndText,
                Hooks::HUD::showBloodOverlay,
                GetKillfeedVisible(),
                GetDvarColor("g_TeamColor_Allies", glm::vec3(1.0f)),
                GetDvarColor("g_TeamColor_Axis", glm::vec3(1.0f)),
            };

            return hudInfo;
        }

        void SetHudInfo(Types::HudInfo hudInfo) final
        {
            SetDvarBool("cg_draw2D", hudInfo.show2DElements);

            Hooks::HUD::showPlayerHUD = hudInfo.showPlayerHUD;
            SetDvarFloat("cg_overheadRankSize", hudInfo.showPlayerHUD ? 0.5f : 0.0f);
            SetDvarFloat("cg_overheadNamesSize", hudInfo.showPlayerHUD ? 0.5f : 0.0f);
            SetDvarFloat("cg_overheadIconSize", hudInfo.showPlayerHUD ? 0.7f : 0.0f);

            Hooks::HUD::showShellshock = hudInfo.showShellshock;
            Hooks::Camera::ApplyShellshock();
            SetDvarBool("ui_drawCrosshair", hudInfo.showCrosshair);
            SetKillfeedVisible(hudInfo.showKillfeed);

            Hooks::HUD::showScore = hudInfo.showScore;
            Hooks::HUD::showIconsAndText = hudInfo.showIconsAndText;

            Hooks::HUD::showBloodOverlay = hudInfo.showBloodOverlay;
            auto& patches = Patches::GetGamePatches();
            for (auto patch : {&patches.CG_DrawFlashDamage, &patches.CG_DrawDamageDirectionIndicators})
            {
                if (hudInfo.showBloodOverlay)
                    patch->Revert();
                else
                    patch->Apply();
            }

            SetDvarColor("g_TeamColor_Allies", hudInfo.killfeedTeam1Color);
            SetDvarColor("g_TeamColor_Axis", hudInfo.killfeedTeam2Color);
        }

        // ui_hud_obituaries is created at runtime, so handle both a bool and a string dvar
        bool GetKillfeedVisible()
        {
            const auto dvar = Functions::FindDvar("ui_hud_obituaries");
            if (!dvar)
                return true;

            if (dvar->type == Structures::DVAR_TYPE_BOOL)
                return dvar->current.enabled;
            if (dvar->type == Structures::DVAR_TYPE_STRING && dvar->current.string)
                return dvar->current.string[0] == '1';

            return true;
        }

        void SetKillfeedVisible(bool visible)
        {
            if (GetKillfeedVisible() == visible)
                return;

            const auto dvar = Functions::FindDvar("ui_hud_obituaries");
            if (!dvar)
                return;

            if (dvar->type == Structures::DVAR_TYPE_BOOL)
            {
                dvar->current.enabled = visible;
                dvar->modified = true;
            }
            else if (dvar->type == Structures::DVAR_TYPE_STRING)
            {
                // let the game manage the string's memory
                Functions::Cbuf_AddText(std::format("set ui_hud_obituaries {}", visible ? 1 : 0));
            }
        }

        // Core indexes this list by entity number without bounds checks, so it always holds every entity
        std::vector<Types::Entity> GetEntities() final
        {
            std::vector<Types::Entity> entities;
            entities.reserve(Structures::MAX_GENTITIES);

            const auto cg_entities = Structures::GetEntities();

            for (int i = 0; i < static_cast<int>(Structures::MAX_GENTITIES); i++)
            {
                if (!cg_entities)
                {
                    entities.push_back({.id = i, .type = Types::EntityType::Unsupported, .clientNum = -1});
                    continue;
                }

                const auto& cent = cg_entities[i];
                entities.push_back(Types::Entity{
                    .id = i,
                    .type = ToEntityType(cent.nextState.eType),
                    .clientNum = cent.nextState.clientNum,
                    .isValid = (cent.flags & Structures::CENTITY_IN_SNAPSHOT) != 0,
                });
            }

            return entities;
        }

        static Types::EntityType ToEntityType(Structures::entityType_t eType)
        {
            switch (eType)
            {
                case Structures::ET_PLAYER:
                    return Types::EntityType::Player;
                case Structures::ET_PLAYER_CORPSE:
                    return Types::EntityType::Corpse;
                case Structures::ET_ITEM:
                    return Types::EntityType::Item;
                case Structures::ET_MISSILE:
                    return Types::EntityType::Missile;
                case Structures::ET_HELICOPTER:
                    return Types::EntityType::Helicopter;
                default:
                    return Types::EntityType::Unsupported;
            }
        }

        std::optional<Types::AnimationPose> GetAnimationPose(std::uint32_t animation, float time) final
        {
            return AnimationPose::GetPose(animation, time);
        }

        std::optional<Types::AnimationPose> GetAnimationPoseByName(std::string_view animation, std::string_view model,
                                                                   float time) final
        {
            return AnimationPose::GetPose(animation, model, time);
        }

        std::optional<Types::WeaponRig> GetWeaponRig(std::string_view weapon) final
        {
            return WeaponRig::GetWeaponRig(weapon);
        }

        std::vector<std::string> GetModelBones(std::string_view model) final
        {
            return AnimationPose::GetModelBones(model);
        }

        Types::BoneData GetBoneData(int32_t entityId, const std::string& name) final
        {
            return Bones::GetBoneData(entityId, name);
        }

        std::optional<Types::TargetBone> GetTargetBone(const Types::BoneCameraTarget& target,
                                                       const std::string& name) final
        {
            return Bones::GetTargetBone(target, name);
        }

        std::vector<std::string> GetTargetBones(const Types::BoneCameraTarget& target) final
        {
            return Bones::GetTargetBones(target);
        }

        constexpr std::vector<std::string> GetSupportedBoneNames() final
        {
            return {
                "tag_weapon",    "tag_weapon_left", "tag_weapon_right", "tag_flash",     "tag_clip",
                "tag_brass",     "j_head",          "j_mainroot",       "j_wrist_le",    "j_wrist_ri",
                "j_shoulder_le", "j_shoulder_ri",   "j_ankle_le",       "j_ankle_ri",    "tag_origin",
            };
        }

        void CL_FirstSnapshot() final
        {
            Patches::GetGamePatches().Con_TimeJumped.Apply();

            Functions::CL_FirstSnapshot(0);

            Patches::GetGamePatches().Con_TimeJumped.Revert();
        }

        void ResetClientData(int serverTime) final
        {
            // before anything goes: what a client script started later in the demo goes with the rewind
            CampaignMaps::NotifyClientScriptsOfRewind();

            // The rewind clears the entities next, which would leak what the client keeps for them: ragdolls, physics
            // objects and the memory of their effects. Once enough has leaked, physics effects such as shell casings
            // stop spawning, so it is freed the way the game does when an entity goes away.
            if (auto entities = Structures::GetEntities())
            {
                for (std::size_t i = 0; i < Structures::MAX_GENTITIES; i++)
                    Functions::CG_ResetEntity(0, &entities[i]);
            }

            // what effects left behind would be from later in the demo: the impact marks (blood, bullet holes) and the
            // effects actors started; looping effects the demo spawns again are killed once it has
            Functions::FX_ClearMarks(0);
            Actors::StopEffects();
            Hooks::Effects::OnRewind();

            auto snapshots = Structures::GetClientSnapshots();
            for (int i = 0; i < Structures::PACKET_BACKUP; i++)
                snapshots[i].valid = 0;

            auto cl = Structures::GetClientActive();
            cl->snap.serverTime = serverTime;
            cl->serverTime = 0;
            cl->oldServerTime = 0;
            cl->oldFrameServerTime = 0;
            cl->serverTimeDelta = 0;
            cl->oldSnapServerTime = 0;

            auto cls = Structures::GetClientStatic();
            cls->realtime = 0;

            if (auto cgs = Structures::GetClientGlobalsStatic())
                cgs->processedSnapshotNum = 0;

            if (auto cg = Structures::GetClientGlobals())
            {
                cg->latestSnapshotNum = 0;
                cg->latestSnapshotTime = 0;
                cg->snap = nullptr;
                cg->nextSnap = nullptr;
                // The first person gun dips after a landing by how long ago it was, which isn't checked for a landing
                // still to come, so after rewinding past one it was moved away until time caught up with it
                cg->landTime = 0;
            }
            // the same for the players' IK: a gun's change still to come held a player's left hand off its grip
            Hooks::PlayerAnimation::ForgetIkTimes();
        }

        Types::PlaybackData GetPlaybackDataAddresses() const final
        {
            // stands in for anything that isn't mapped yet, or isn't allocated while no map is loaded
            static std::int32_t dummyValue = 0;
            const auto dummyAddress = reinterpret_cast<uintptr_t>(&dummyValue);

            auto cl = Structures::GetClientActive();
            auto clc = Structures::GetClientConnection();
            auto cgs = Structures::GetClientGlobalsStatic();
            auto cls = Structures::GetClientStatic();
            auto entities = Structures::GetEntities();

            return Types::PlaybackData{
                .cl =
                    {
                        .snap_serverTime = reinterpret_cast<uintptr_t>(&cl->snap.serverTime),
                        .serverTime = reinterpret_cast<uintptr_t>(&cl->serverTime),
                        .parseEntitiesNum = reinterpret_cast<uintptr_t>(&cl->parseEntitiesNum),
                        .parseClientsNum = reinterpret_cast<uintptr_t>(&cl->parseClientsNum),
                    },
                .clc =
                    {
                        .serverCommandSequence = reinterpret_cast<uintptr_t>(&clc->serverCommandSequence),
                        .lastExecutedServerCommand = reinterpret_cast<uintptr_t>(&clc->lastExecutedServerCommand),
                        .serverCommands = {.address = reinterpret_cast<uintptr_t>(&clc->serverCommands),
                                           .size = sizeof(clc->serverCommands)},
                        .serverConfigDataSequence = 0,
                    },
                .cgs =
                    {
                        .serverCommandSequence =
                            cgs ? reinterpret_cast<uintptr_t>(&cgs->serverCommandSequence) : dummyAddress,
                    },
                .cls =
                    {
                        .realtime = reinterpret_cast<uintptr_t>(&cls->realtime),
                    },
                // TODO: compass actors are not mapped yet
                .s_compassActors = {.address = dummyAddress, .size = 0},
                .teamChatMsgs = {.address = 0, .size = 0},
                .cg_entities = {.address = entities ? reinterpret_cast<uintptr_t>(entities) : dummyAddress,
                                .size = entities ? static_cast<uint32_t>(Structures::MAX_GENTITIES *
                                                                         Structures::CENTITY_SIZE)
                                                 : 0},
                // TODO: client info is not mapped yet
                .clientInfo = {.address = dummyAddress, .size = 0},
                .gameState = {.address = reinterpret_cast<uintptr_t>(Structures::GetGameState()),
                              .size = sizeof(Structures::gameState_t)},
                .killfeed = 0,
            };
        }

       private:
        // why the last PlayDemo sent no demo command (see GetPlayDemoError)
        std::string playDemoError;
    };
}  // namespace IWXMVM::T5
