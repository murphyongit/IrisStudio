// Test harness for driving the game from scripts, built only with /p:T5Harness=true (see tools/t5-harness). It runs
// the lines of commands.txt, then those appended to live.txt, from the folder in IWXMVM_HARNESS_DIR, and logs to
// log.txt there.
#include "StdInclude.hpp"
#include <Psapi.h>
#include <numeric>
#include "Harness.hpp"
#include "MemoryWatch.hpp"
#include "Components/DemoModLogic.hpp"
#include "Hooks/Camera.hpp"
#include "Hooks/Effects.hpp"
#include "CampaignMaps.hpp"
#include "MapMenu.hpp"
#include "LiveActors.hpp"
#include "LiveGame.hpp"
#include "PoolGrowth.hpp"

#include "Mod.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "Components/GripLogic.hpp"
#include "Components/BulletTime.hpp"
#include "Components/FrozenSpans.hpp"
#include "Components/CameraManager.hpp"
#include "Components/Playback.hpp"
#include "Components/Rewinding.hpp"
#include "Components/Rendering.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/PlayerAnimation.hpp"
#include "Components/PlayerLookLogic.hpp"
#include "Hooks/PlayerAnimation.hpp"
#include "Components/LiveActors.hpp"
#include "Components/LiveGame.hpp"
#include "Components/Loadouts.hpp"
#include "Components/KeyframeSerializer.hpp"
#include "Configuration/Configuration.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Events.hpp"
#include "D3D9.hpp"
#include "Components/FreeCamera.hpp"
#include "UI/UIManager.hpp"
#include "UI/Layout.hpp"
#include "UI/GettingStarted.hpp"
#include "UI/ViewMenu.hpp"
#include "Utilities/PathUtils.hpp"
#include "imgui_internal.h"
#include "Addresses.hpp"
#include "Functions.hpp"
#include "Structures.hpp"
#include "Hooks/Playback.hpp"
#include "Hooks/Rendering.hpp"
#include "ContentPacks.hpp"
#include "Actors.hpp"
#include "Lights.hpp"
#include "Bones.hpp"
#include "ModelLighting.hpp"
#include "UI/AreaMap.hpp"
#include "Components/LightLogic.hpp"
#include "Components/Lights.hpp"
#include "AnimationPose.hpp"
#include "WeaponRig.hpp"
#include "AnimationStore.hpp"
#include "Components/Actors.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/AnimationPackLogic.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/PropLogic.hpp"
#include "Components/Props.hpp"
#include "Components/UndoHistory.hpp"
#include "Components/VisualsLogic.hpp"
#include "Components/QualityLogic.hpp"
#include "Components/ReShadeKeys.hpp"
#include "Components/ReShadeLogic.hpp"
#include "Components/SurfaceDepth.hpp"
#include "Components/SurfaceLogic.hpp"
#include "Components/TexturePack.hpp"
#include "Surfaces.hpp"
#include "Components/RuntimeProperties.hpp"
#include "ReShade.hpp"
#include "UI/Commands.hpp"
#include "UI/Toasts.hpp"
#include "UI/TimelineSelection.hpp"
#include "UI/Components/GameView.hpp"
#include "UI/Components/ReloadStrip.hpp"
#include "UI/Components/Timeline.hpp"
#include "UI/Components/TimelineGraph.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "Components/Markers.hpp"
#include "Components/PoseLogic.hpp"
#include "Components/AnimationCatalogLogic.hpp"
#include "Components/ReloadProfiles.hpp"
#include "Components/PoseSignatures.hpp"
#include "UI/ActorActions.hpp"
#include "Graphics/GizmoState.hpp"
#include "Diagnostics.hpp"
#include "Hooks/ScriptMemory.hpp"
#include "Hooks/EntityClients.hpp"
#include "UI/Components/DemoLoader.hpp"
#include "UI/Components/ActorsMenu.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::T5::Harness
{
    std::filesystem::path directory;
    std::ofstream logFile;

    // the Visuals tab's settings as "visuals mark" found them
    std::optional<Components::VisualConfiguration::Settings> markedVisuals;

    void Log(const std::string& text)
    {
        const auto cg = Structures::GetClientGlobals();
        logFile << std::format("t {} {}\n", cg ? cg->time : 0, text) << std::flush;
    }

    // the lines of commands.txt, then the lines appended to live.txt as they come, until "quit"
    std::string ReadShared(const std::filesystem::path& path)
    {
        const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return {};
        std::string content(GetFileSize(file, nullptr), '\0');
        DWORD read = 0;
        if (!ReadFile(file, content.data(), static_cast<DWORD>(content.size()), &read, nullptr))
            read = 0;
        CloseHandle(file);
        content.resize(read);
        std::erase(content, '\r');
        return content;
    }

    // "eject" unloads the mod with the harness: the line of live.txt to go on from is left in the game's environment,
    // where the harness, injected again, finds it, and then leaves out commands.txt and adds to log.txt
    constexpr auto RESUME_VARIABLE = "IWXMVM_HARNESS_RESUME";
    std::atomic<bool> ejecting = false;
    bool skipPrepareUnload = false;

    bool SkipsPrepareUnload()
    {
        return skipPrepareUnload;
    }

    // A reference to the DLL for each of the harness's threads, which end with FreeLibraryAndExitThread, so the mod
    // isn't unloaded under them
    HMODULE ReferenceModule()
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ReferenceModule),
                           &module);
        return module;
    }

    class CommandSource
    {
       public:
        // from that line of live.txt on, leaving out commands.txt
        explicit CommandSource(std::optional<std::size_t> resume)
        {
            if (resume)
            {
                initial = {""};
                nextLive = *resume;
            }
        }

        std::size_t GetNextLive() const
        {
            return nextLive;
        }

        bool Next(std::string& command)
        {
            while (true)
            {
                if (initial.empty())
                {
                    std::ifstream file(directory / "commands.txt");
                    for (std::string line; std::getline(file, line);)
                        initial.push_back(line);
                    initial.push_back("");
                }
                if (nextInitial + 1 < initial.size())
                    command = initial[nextInitial++];
                else
                {
                    // shared for writing, as send.ps1 appends to it while this reads
                    std::vector<std::string> lines;
                    std::istringstream live(ReadShared(directory / "live.txt"));
                    for (std::string line; std::getline(live, line);)
                        lines.push_back(line);
                    if (lines.size() <= nextLive)
                    {
                        Sleep(20);
                        continue;
                    }
                    command = lines[nextLive++];
                }
                if (!command.empty())
                    return command != "quit";
            }
        }

       private:
        std::vector<std::string> initial;
        std::size_t nextInitial = 0;
        std::size_t nextLive = 0;
    };

    // the menus checked so far and whether they were last shown, by address; written from the game's thread
    constexpr std::size_t MENU_SLOTS = 1024;
    std::array<std::atomic<const Structures::menuDef_t*>, MENU_SLOTS> menus{};
    std::array<std::atomic<bool>, MENU_SLOTS> menuVisible{};

    void OnMenuChecked(const Structures::menuDef_t* menu, bool visible)
    {
        for (std::size_t i = (reinterpret_cast<std::uintptr_t>(menu) >> 4) % MENU_SLOTS, n = 0; n < MENU_SLOTS;
             i = (i + 1) % MENU_SLOTS, n++)
        {
            const Structures::menuDef_t* expected = nullptr;
            if (menus[i].load() == menu || menus[i].compare_exchange_strong(expected, menu) || expected == menu)
            {
                menuVisible[i] = visible;
                return;
            }
        }
    }

    HWND GetGameWindow()
    {
        return FindWindowA("CoDBlackOps", nullptr);
    }

    // The overlay doesn't react to synthetic mouse input from outside the game, so the harness posts the window
    // messages itself. They are spaced a frame or more apart, as a press and release handled in one frame are no click.
    void PostInput(UINT message, WPARAM wParam, LPARAM lParam)
    {
        PostMessageA(GetGameWindow(), message, wParam, lParam);
        Sleep(40);
    }

    // ImGui's backend drops the mouse when the real cursor isn't over the window, so the cursor is moved there too
    void PostMouse(UINT message, int x, int y, WPARAM buttons = 0)
    {
        POINT screen{x, y};
        ClientToScreen(GetGameWindow(), &screen);
        SetCursorPos(screen.x, screen.y);
        PostInput(message, buttons, MAKELPARAM(x, y));
    }

    // actions for the render thread, which owns the D3D device and draws the overlay
    std::mutex renderThreadMutex;
    std::vector<std::function<void()>> renderThreadActions;

    void RunOnRenderThread(std::function<void()> action);

    // Where a mouse command acts: <x> <y> in client coordinates, or the middle of an overlay window by its name
    // ("Timeline", "GameView", "Demos")
    std::optional<POINT> ParseMousePoint(std::istringstream& args)
    {
        std::string first;
        args >> first;
        if (!first.empty() && (std::isdigit(static_cast<unsigned char>(first[0])) || first[0] == '-'))
        {
            int y = 0;
            args >> y;
            return POINT{std::atoi(first.c_str()), y};
        }
        std::optional<POINT> middle;
        RunOnRenderThread([&] {
            if (const auto window = ImGui::FindWindowByName(first.c_str()))
            {
                const auto point = window->Pos + window->Size * 0.5f;
                middle = POINT{static_cast<int>(point.x), static_cast<int>(point.y)};
            }
        });
        return middle;
    }

    // A property by the name the harness gives it: a Types::KeyframeablePropertyType's, or "ReShade:<id>" for one of
    // ReShade's (Components::RuntimeProperties)
    std::optional<Types::KeyframeablePropertyType> ParsePropertyType(const std::string& text)
    {
        if (text.starts_with("ReShade:"))
            return Components::RuntimeProperties::FindById(text.substr(8));
        if (text.starts_with("Light:"))
            return Components::RuntimeProperties::FindById(text.substr(6));
        return magic_enum::enum_cast<Types::KeyframeablePropertyType>(text);
    }

    void RunOnRenderThread(std::function<void()> action)
    {
        std::atomic<bool> done = false;
        {
            std::lock_guard lock(renderThreadMutex);
            renderThreadActions.push_back([&] {
                action();
                done = true;
            });
        }
        while (!done)
            Sleep(5);
    }

    // actions for the main thread, for game functions that need the game's thread-local storage
    std::mutex mainThreadMutex;
    std::vector<std::function<void()>> mainThreadActions;

    void RunOnMainThread(std::function<void()> action)
    {
        std::atomic<bool> done = false;
        {
            std::lock_guard lock(mainThreadMutex);
            mainThreadActions.push_back([&] {
                action();
                done = true;
            });
        }
        while (!done)
            Sleep(5);
    }

    // Frames each thread has run, written to heartbeat.txt twice a second so send.ps1 can tell a stalled or frozen game
    // from a slow command
    std::atomic<std::uint32_t> mainThreadFrames = 0;
    std::atomic<std::uint32_t> renderThreadFrames = 0;

    void WriteHeartbeats()
    {
        std::thread([module = ReferenceModule()] {
            while (!ejecting)
            {
                const auto cg = Structures::GetClientGlobals();
                {
                    std::ofstream file(directory / "heartbeat.tmp", std::ios::trunc);
                    file << std::format("{} {} {} {}", GetTickCount64(), mainThreadFrames.load(),
                                        renderThreadFrames.load(), cg ? cg->time : 0);
                }
                std::error_code error;
                std::filesystem::rename(directory / "heartbeat.tmp", directory / "heartbeat.txt", error);
                Sleep(500);
            }
            FreeLibraryAndExitThread(module, 0);
        }).detach();
    }

    // when the last frames of each thread started, for the "frametimes" command
    struct FrameTimes
    {
        static constexpr std::size_t SIZE = 4096;
        std::array<std::int64_t, SIZE> times{};
        std::atomic<std::uint32_t> count = 0;

        void Record()
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            times[count % SIZE] = now.QuadPart;
            ++count;
        }

        // the intervals in ms since the given count
        std::vector<double> Since(std::uint32_t start) const
        {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            std::vector<double> intervals;
            for (auto i = start + 1; i < count && i - start < SIZE; ++i)
                intervals.push_back((times[i % SIZE] - times[(i - 1) % SIZE]) * 1000.0 / frequency.QuadPart);
            return intervals;
        }
    };
    FrameTimes mainFrameTimes, renderFrameTimes;

    // What CL_KeyEvent, which runs the game's binds, was given since the "gamekeys reset", by the game's key number:
    // presses (with repeats) and releases, for telling whether keys the overlay used reached the game
    std::array<std::atomic<std::uint32_t>, 512> gameKeyPresses{}, gameKeyReleases{};
    using CL_KeyEvent_t = void(__cdecl*)(int localClientNum, int key, int down, unsigned int time);
    CL_KeyEvent_t CL_KeyEvent_Trampoline = nullptr;

    void __cdecl CL_KeyEvent_Hook(int localClientNum, int key, int down, unsigned int time)
    {
        if (key >= 0 && key < static_cast<int>(gameKeyPresses.size()))
            ++(down ? gameKeyPresses : gameKeyReleases)[key];
        CL_KeyEvent_Trampoline(localClientNum, key, down, time);
    }

    // the game's number of a key: a character key is its lowercase character
    std::optional<int> ParseGameKey(const std::string& name)
    {
        static const std::map<std::string, int> named = {
            {"tab", 9},        {"enter", 13},    {"escape", 27},      {"space", 32},
            {"console", 0x7E}, {"mouse1", 0xC8}, {"mouse2", 0xC9},    {"mouse3", 0xCA},
            {"mouse4", 0xCB},  {"mouse5", 0xCC}, {"wheeldown", 0xCD}, {"wheelup", 0xCE},
        };
        if (const auto it = named.find(name); it != named.end())
            return it->second;
        if (name.size() == 1)
            return static_cast<int>(std::tolower(static_cast<unsigned char>(name[0])));
        if (name.size() > 1 && name[0] == '#')
            return std::atoi(name.c_str() + 1);
        return std::nullopt;
    }

    // where the view was at the start of each main thread frame, while "movetest" runs
    std::atomic<bool> tracingView = false;
    std::vector<std::pair<std::int64_t, std::array<float, 3>>> viewTrace;

    // the actor the harness added last, which actor commands take as "last", and the one before it ("previous")
    std::uint32_t lastActorId = 0;
    std::uint32_t previousActorId = 0;

    std::uint32_t ParseActorId(const std::string& text)
    {
        if (text == "last")
            return lastActorId;
        if (text == "previous")
            return previousActorId;
        // (the one added last by any means, e.g. the UI's menus)
        if (text == "newest")
        {
            const auto actors = Components::Actors::GetActors();
            return actors.empty() ? 0 : std::ranges::max(actors, {}, &Types::Actor::id).id;
        }
        // (and the first of a scene loaded with the demo)
        if (text == "oldest")
        {
            const auto actors = Components::Actors::GetActors();
            return actors.empty() ? 0 : std::ranges::min(actors, {}, &Types::Actor::id).id;
        }
        if (text == "second")
        {
            auto actors = Components::Actors::GetActors();
            std::ranges::sort(actors, {}, &Types::Actor::id);
            return actors.size() < 2 ? 0 : actors[1].id;
        }
        try
        {
            return static_cast<std::uint32_t>(std::stoul(text));
        }
        catch (const std::exception&)
        {
            return 0;
        }
    }

    // bone positions of actors recorded by snap, by label: the bones and where each was
    std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::optional<glm::vec3>>>> snaps;
    // which way actors' bodies faced and they were drawn, recorded by facecheck, by label
    std::map<std::string, std::pair<float, float>> facings;
    // and on the render thread: frames without game view focus, without W held, with no time step
    std::atomic<std::uint32_t> renderTraced = 0, renderUnfocused = 0, renderNoKey = 0, renderNoTime = 0;

    // stallmain: how many more frames of a demo the main thread sleeps in, and how long
    std::atomic<int> stallFrames = 0, stallMilliseconds = 0;
    // strayclient: the entity whose local client is written once a demo waits for its first snapshot, and the value
    std::atomic<int> strayEntity = -1, strayValue = 0;
    // (written 20 frames into the wait, well after the load's frame set the entities up)
    int strayFrames = 0;

    // speed watch: logs every change of the timescale dvar, with the frame and the tick it was seen at
    std::atomic<bool> watchingSpeed = false;
    float speedSeen = -1.0f;

    void WatchSpeed()
    {
        if (!watchingSpeed)
            return;
        const auto dvar = Functions::FindDvar("timescale");
        if (!dvar || dvar->current.value == speedSeen)
            return;
        Log(std::format("SPEED {} -> {} tick {} rewinding {}", speedSeen, dvar->current.value,
                        Mod::GetGameInterface()->GetDemoInfo().gameTick, Components::Rewinding::IsRewinding()));
        speedSeen = dvar->current.value;
    }

    void RunMainThreadActions()
    {
        Diagnostics::OnMainThreadFrame();
        ++mainThreadFrames;
        WatchSpeed();
        if (strayEntity >= 0 && Structures::GetClientConnection()->demoplaying)
        {
            const auto cg = Structures::GetClientGlobals();
            const auto entities = reinterpret_cast<std::uint8_t*>(Structures::GetEntities());
            if (cg && cg->time == 0 && entities && ++strayFrames >= 20)
            {
                strayFrames = 0;
                entities[static_cast<std::size_t>(strayEntity.load()) * Structures::CENTITY_SIZE + 4] =
                    static_cast<std::uint8_t>(strayValue.load());
                strayEntity = -1;
            }
        }
        if (stallFrames > 0 && Structures::GetClientConnection()->demoplaying)
        {
            if (const auto cg = Structures::GetClientGlobals(); cg && cg->time > 0)
            {
                --stallFrames;
                Sleep(stallMilliseconds);
            }
        }
        mainFrameTimes.Record();
        if (tracingView)
        {
            if (const auto cg = Structures::GetClientGlobals())
            {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                const auto& origin = cg->refdef.vieworg;
                viewTrace.push_back({now.QuadPart, {origin[0], origin[1], origin[2]}});
            }
        }
        std::lock_guard lock(mainThreadMutex);
        for (const auto& action : mainThreadActions)
            action();
        mainThreadActions.clear();
    }

    void WaitForRewind()
    {
        Sleep(50);
        while (Components::Rewinding::IsRewinding())
            Sleep(5);
    }

    // Whether a wait for the demo's time has lost it: paused for 3 s, which the end of the demo does by itself
    // (Rewinding.cpp), or frozen, the time never comes, and every command after the wait would be lost with it. One call
    // per 5 ms.
    bool IsTimeWaitLost(int& pausedFor)
    {
        const auto paused = (Components::Playback::IsPaused() || Components::Playback::IsGameFrozen()) &&
                            !Components::Rewinding::IsRewinding() &&
                            Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
        pausedFor = paused ? pausedFor + 5 : 0;
        return pausedFor >= 3000;
    }

    // Saves the game's frame as the render thread has it before the overlay is drawn over it (the client area alone),
    // scaled down to a width, as a PNG; false when it couldn't. Taken by the game itself, so it shows the tick it was
    // asked at, unlike "shot", which the scripts take from the screen.
    bool GrabFrame(const std::filesystem::path& path, UINT width)
    {
        bool saved = false;
        RunOnRenderThread([&] {
            const auto device = D3D9::GetDevice();
            IDirect3DSurface9* backBuffer = nullptr;
            if (!device || FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
                return;
            D3DSURFACE_DESC desc{};
            backBuffer->GetDesc(&desc);
            const auto w = std::min(width, desc.Width);
            const auto h = std::max<UINT>(1, desc.Height * w / desc.Width);
            // a plain copy first (the back buffer may be multisampled), read back and filtered down
            IDirect3DSurface9 *copy = nullptr, *read = nullptr, *scaled = nullptr;
            if (SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                                     FALSE, &copy, nullptr)) &&
                SUCCEEDED(device->StretchRect(backBuffer, nullptr, copy, nullptr, D3DTEXF_NONE)) &&
                SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8,
                                                              D3DPOOL_SYSTEMMEM, &read, nullptr)) &&
                SUCCEEDED(device->GetRenderTargetData(copy, read)) &&
                SUCCEEDED(
                    device->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &scaled, nullptr)) &&
                SUCCEEDED(D3DXLoadSurfaceFromSurface(scaled, nullptr, nullptr, read, nullptr, nullptr,
                                                     D3DX_FILTER_TRIANGLE, 0)))
                saved = SUCCEEDED(D3DXSaveSurfaceToFileW(path.c_str(), D3DXIFF_PNG, scaled, nullptr, nullptr));
            for (const auto surface : {scaled, read, copy, backBuffer})
                if (surface)
                    surface->Release();
        });
        return saved;
    }

    std::string DescribeMoment(const std::optional<Types::ReloadMoment>& moment)
    {
        return moment ? std::format("{:.3f} {}", moment->time, ReloadLogic::GetSourceName(moment->source))
                      : std::string("-");
    }

    std::string_view DescribeGrip(const std::optional<Types::ReloadGrip>& grip)
    {
        return !grip ? "-" : grip->kind == Types::ReloadGrip::Kind::Authored ? "authored" : "measured";
    }

    std::string DescribeProfile(const Types::ReloadProfile& profile)
    {
        return std::format("{} ({}), hand {} ({}), out {}, swap {}, in {}, grips {}/{}, fall {}{}",
                           ReloadLogic::GetKindName(profile.kind), ReloadLogic::GetSourceName(profile.kindSource),
                           ReloadLogic::GetHandName(profile.hand), ReloadLogic::GetSourceName(profile.handSource),
                           DescribeMoment(profile.out), DescribeMoment(profile.swap), DescribeMoment(profile.in),
                           DescribeGrip(profile.oldGrip), DescribeGrip(profile.newGrip),
                           profile.fall
                               ? std::format("{} at {:.3f}", profile.fall->fromGun ? "from the gun" : "from the hand",
                                             profile.fall->time)
                               : std::string("-"),
                           profile.note.empty() ? std::string() : ", note: " + profile.note);
    }

    // What profilecheck measured of an actor's first clip: its reload profile, and at its first reload's out, a tick
    // before in and in, how far the profile's magazine is from where the game has the gun's and the drawn one from the
    // profile's
    struct ProfileCheck
    {
        std::string animation, body, weapon;
        std::shared_ptr<const Types::ReloadProfile> profile;
        std::optional<ActorLogic::Reload> reload;
        std::optional<Types::WeaponRig> rig;
        struct Look
        {
            std::string name;
            std::uint32_t tick = 0;
            bool falling = false;               // where it starts to fall, else in the hand
            std::optional<float> toGun;         // the profile's magazine from the gun's (points along the axes)
            std::optional<float> toGunOrigins;  // and their origins
            std::optional<float> drawn;         // the drawn one from the profile's
            std::optional<bool> drawnOnGun;
            bool missing = false;
            std::optional<std::uint32_t> timelineAt;  // where the timeline was instead of the tick
        };
        std::vector<Look> looks;
        bool passed = false;
        std::string error;  // why it couldn't be checked
        std::string text;   // the looks as profilecheck logs them
    };

    // waits up to 5 s for an animation's reload profile on a body with a gun as its override is now, null if it wasn't
    // worked out by then
    std::shared_ptr<const Types::ReloadProfile> WaitForProfile(const std::string& animation, const std::string& body,
                                                               const std::string& weapon)
    {
        for (int wait = 0; wait < 100; ++wait)
        {
            auto profile = Components::ReloadProfiles::Get(animation, body, weapon);
            if (profile && Components::ReloadProfiles::IsUpToDate(animation, body, weapon))
                return profile;
            Sleep(50);
        }
        return nullptr;
    }

    // a reload override as json, "none" for none
    std::string DescribeOverride(const std::optional<ReloadLogic::ProfileEntry>& entry)
    {
        return entry ? ReloadLogic::EntryToJson(*entry).dump() : std::string("none");
    }

    // profilecheck's measurements of an actor (freeze and pause first): with later, that many ticks after out and
    // before in, where each held magazine should be at least the tolerance off
    ProfileCheck CheckProfile(std::uint32_t id, float tolerance, std::optional<std::uint32_t> later)
    {
        ProfileCheck check;
        const auto actor = Components::Actors::GetActor(id);
        if (!actor || actor->clips.empty())
        {
            check.error = "no such actor, or it has no clips";
            return check;
        }
        check.body = actor->models.empty() ? std::string() : actor->models.front();
        check.weapon = Components::Actors::ResolveWeapon(*actor);
        check.animation = actor->clips.front().animation;
        check.profile = WaitForProfile(check.animation, check.body, check.weapon);
        if (!check.profile)
        {
            check.error = std::format("the profile of {} wasn't worked out in 5 s", check.animation);
            return check;
        }
        const auto& profile = check.profile;

        const auto reloads = Components::ReloadProfiles::GetReloads(*actor, UINT32_MAX);
        check.rig = Mod::GetGameInterface()->GetWeaponRig(check.weapon);
        if (reloads.empty() || !check.rig)
        {
            check.error = reloads.empty() ? "no reload to draw" : "no rig";
            return check;
        }
        const auto& rig = *check.rig;
        check.reload = reloads.front();
        const auto& reload = *check.reload;
        const auto moved = later.has_value();
        const auto by = later.value_or(0);
        const auto hand = std::string(ReloadLogic::GetHandBone(profile->hand));
        const bool dropped = profile->kind == Types::ReloadKind::DroppedMagazine;
        // At out and in, where the profile puts the magazine against where the game has it on the gun; and (unless
        // later) the magazine drawn: as the profile puts it in the hand or falling at out and just before in, on the
        // gun at in.
        enum class Drawn
        {
            Unchecked,
            Held,
            Falling,
            OnGun,
        };
        struct Look
        {
            const char* name;
            std::uint32_t tick;
            bool falling;
            const Types::ReloadGrip* grip;
            bool onGun;  // compared with where the game has it on the gun
            Drawn drawn;
        };
        std::vector<Look> looks{{"out", reload.out + by, dropped, profile->oldGrip ? &*profile->oldGrip : nullptr, true,
                                 moved     ? Drawn::Unchecked
                                 : dropped ? Drawn::Falling
                                           : Drawn::Held}};
        if (!moved)
            looks.push_back({"before in", reload.in - 1, false, &*profile->newGrip, false, Drawn::Held});
        looks.push_back({"in", reload.in - std::min(by, reload.in), false, &*profile->newGrip, true,
                         moved ? Drawn::Unchecked : Drawn::OnGun});
        // how near the drawn magazine is to where the profile puts it: the same transforms, worked out twice
        constexpr float DRAWN_TOLERANCE = 0.01f;
        check.passed = true;
        for (const auto& look : looks)
        {
            auto& measured = check.looks.emplace_back(
                ProfileCheck::Look{.name = look.name, .tick = look.tick, .falling = look.falling});
            if (moved && look.falling)
            {
                check.text += std::format(" {} at {}: falls, not checked;", look.name, look.tick);
                continue;
            }
            Components::Playback::MoveFrozenTick(look.tick);
            Sleep(1000);
            std::vector<std::optional<glm::mat4>> bones;
            std::optional<glm::mat4> placement;
            std::optional<Actors::MagazineState> state;
            std::uint32_t tick = 0;
            RunOnMainThread([&] {
                bones = Actors::GetBoneMatrices(id, {hand, rig.magazineBone, "tag_weapon_right"});
                placement = Actors::GetDrawnPlacement(id);
                state = Actors::GetMagazineState(id);
                tick = Components::Playback::GetTimelineTick();
            });
            if (bones.size() != 3 || !bones[0] || !bones[1] || !bones[2] || !placement || !state ||
                (!look.falling && !look.grip) || (look.falling && !profile->fall))
            {
                check.passed = false;
                measured.missing = true;
                check.text += std::format(" {} at {}: missing;", look.name, look.tick);
                continue;
            }
            // a falling one starts from the gun, as the game draws it then (the legs under an upper body clip move it
            // from where the animation alone has it, see ReloadLogic::PlaceFall)
            const auto expected = look.falling ? *bones[2] * rig.magazine : *bones[0] * look.grip->magazine;
            check.text += std::format(" {} at {} ({}):", look.name, look.tick, look.falling ? "falling" : "held");
            if (look.onGun)
            {
                // (as far along the axes as posecheck, about a magazine)
                measured.toGun = PoseLogic::GetTransformDistance(expected, *bones[1], 10.0f);
                measured.toGunOrigins = glm::distance(glm::vec3(expected[3]), glm::vec3((*bones[1])[3]));
                check.passed &= moved ? *measured.toGun >= tolerance : *measured.toGun <= tolerance;
                check.text += std::format(" {:.3f} ({:.3f}) from the gun's", *measured.toGun, *measured.toGunOrigins);
            }
            if (look.drawn == Drawn::OnGun)
            {
                measured.drawnOnGun = !state->offGun && state->held.empty();
                check.passed &= *measured.drawnOnGun;
                check.text += *measured.drawnOnGun ? " drawn on the gun" : " drawn OFF the gun";
            }
            else if (look.drawn != Drawn::Unchecked)
            {
                const auto drawn = look.drawn == Drawn::Held ? state->heldMagazine : state->droppedMagazine;
                if (drawn)
                    measured.drawn = PoseLogic::GetTransformDistance(expected, *drawn, 10.0f);
                check.passed &= state->offGun && measured.drawn && *measured.drawn <= DRAWN_TOLERANCE;
                check.text +=
                    measured.drawn ? std::format(" drawn {:.3f} from it", *measured.drawn) : std::string(" none drawn");
            }
            check.text += ";";
            if (tick != look.tick)
            {
                check.passed = false;
                measured.timelineAt = tick;
                check.text += std::format(" the timeline was at {};", tick);
            }
        }
        return check;
    }

    // where reloadsweep puts its actor: x y z yaw
    glm::vec4 sweepSpot{1700.0f, 300.0f, -64.0f, 180.0f};

    // reloadsweep's look at one reload animation (see the command)
    void SweepReload(const std::string& animation, const std::string& models, const std::string& weapon, bool frames)
    {
        auto json = nlohmann::json{{"animation", animation}, {"models", models}, {"weapon", weapon}};
        const auto catalog = Components::Actors::GetCatalog();
        const auto entry = catalog ? catalog->FindAnimation(animation) : nullptr;
        json["loaded"] = entry != nullptr;
        if (!entry)
        {
            Log("SWEEP " + json.dump());
            return;
        }
        if (!Components::Playback::IsGameFrozen() || !Components::Playback::IsPaused())
        {
            Log(std::format("FAIL reloadsweep {}: freeze and pause first", animation));
            return;
        }

        // the actor, playing it once from the frozen tick
        Components::Actors::Clear();
        const auto start = Components::Playback::GetTimelineTick();
        const auto length = static_cast<std::uint32_t>(std::ceil(entry->duration * 1000.0f));
        Types::Actor actor;
        std::istringstream parts(models);
        for (std::string part; std::getline(parts, part, '+');)
            actor.models.push_back(part);
        actor.position = glm::vec3(sweepSpot);
        actor.yaw = sweepSpot.w;
        // (a weapon's attachments after a colon, e.g. skorpion_mp:dw)
        const auto colon = weapon.find(':');
        actor.weapon = weapon == "-" ? "" : weapon.substr(0, colon);
        if (colon != std::string::npos)
        {
            std::istringstream attachments(weapon.substr(colon + 1));
            for (std::string attachment; std::getline(attachments, attachment, ',');)
                actor.attachments.push_back(attachment);
            std::ranges::sort(actor.attachments);
        }
        actor.clips.push_back({.animation = animation, .start = start, .length = length, .loop = false, .blendIn = 0});
        const auto id = Components::Actors::Add(actor);
        previousActorId = std::exchange(lastActorId, id);
        Components::Playback::MoveFrozenTick(start + 1);
        Sleep(500);

        const auto check = CheckProfile(id, 0.1f, std::nullopt);
        const auto round = [](float value) { return std::round(value * 1e4f) / 1e4f; };
        const auto moment = [&](const std::optional<Types::ReloadMoment>& m) {
            return m ? nlohmann::json::array({round(m->time), ReloadLogic::GetSourceName(m->source)})
                     : nlohmann::json(nullptr);
        };
        const auto optional = [&](const std::optional<float>& value) {
            return value ? nlohmann::json(round(*value)) : nlohmann::json(nullptr);
        };
        json["body"] = check.body;
        json["variant"] = check.weapon;
        json["duration"] = round(entry->duration);
        json["start"] = start;
        json["length"] = length;
        json["magazineBone"] = check.rig ? check.rig->magazineBone : std::string();
        if (const auto& profile = check.profile)
        {
            auto inserts = nlohmann::json::array();
            for (const auto insert : profile->inserts)
                inserts.push_back(round(insert));
            json["profile"] = {
                {"kind", ReloadLogic::GetKindName(profile->kind)},
                {"kindSource", ReloadLogic::GetSourceName(profile->kindSource)},
                {"hand", ReloadLogic::GetHandName(profile->hand)},
                {"handSource", ReloadLogic::GetSourceName(profile->handSource)},
                {"out", moment(profile->out)},
                {"swap", moment(profile->swap)},
                {"in", moment(profile->in)},
                {"inserts", inserts},
                {"grips", {DescribeGrip(profile->oldGrip), DescribeGrip(profile->newGrip)}},
                {"fall", profile->fall
                             ? nlohmann::json{{"fromGun", profile->fall->fromGun}, {"time", round(profile->fall->time)}}
                             : nlohmann::json(nullptr)},
                {"note", profile->note},
            };
        }
        if (check.reload)
            json["reload"] = {{"out", check.reload->out},
                              {"swap", check.reload->swap ? nlohmann::json(*check.reload->swap) : nullptr},
                              {"in", check.reload->in},
                              {"end", check.reload->end},
                              {"dropped", check.reload->dropped}};
        auto looks = nlohmann::json::array();
        for (const auto& look : check.looks)
            looks.push_back({{"name", look.name},
                             {"tick", look.tick},
                             {"falling", look.falling},
                             {"toGun", optional(look.toGun)},
                             {"toGunOrigins", optional(look.toGunOrigins)},
                             {"drawn", optional(look.drawn)},
                             {"drawnOnGun", look.drawnOnGun ? nlohmann::json(*look.drawnOnGun) : nullptr},
                             {"missing", look.missing},
                             {"timelineAt", look.timelineAt ? nlohmann::json(*look.timelineAt) : nullptr}});
        json["check"] = {{"passed", check.error.empty() ? nlohmann::json(check.passed) : nullptr},
                         {"error", check.error},
                         {"looks", looks}};

        // Four frames: as it leaves the gun, while the hand holds the old one (pulled) or fetches a new one (dropped),
        // with the new one after the swap, and a tick before it goes in. Kinds that aren't drawn get them from their
        // profile's moments, or spread over the animation.
        const auto& profile = check.profile;
        const auto tickAt = [&](float time) {
            return start + static_cast<std::uint32_t>(std::ceil(time * static_cast<float>(length)));
        };
        std::uint32_t out = tickAt(0.2f), in = tickAt(0.8f);
        std::optional<std::uint32_t> swap;
        if (check.reload)
        {
            out = check.reload->out;
            in = check.reload->in;
            swap = check.reload->swap;
        }
        else if (profile)
        {
            if (profile->out)
                out = tickAt(profile->out->time);
            if (profile->in && tickAt(profile->in->time) > out + 2)
                in = tickAt(profile->in->time);
            if (profile->swap)
                swap = tickAt(profile->swap->time);
        }
        if (in <= out + 2)
            in = std::max(out + 3, tickAt(0.8f));
        if (swap && (*swap <= out || *swap >= in))
            swap.reset();
        const auto between = [](std::uint32_t a, std::uint32_t b) { return a + (b - a) / 2; };
        const bool pulled = profile && profile->kind == Types::ReloadKind::Magazine;
        const std::vector<std::pair<std::string, std::uint32_t>> moments{
            {"out", out},
            {pulled ? "old held" : "fetching", swap ? between(out, *swap) : out + (in - out) / 3},
            {"new held", swap ? between(*swap, in) : out + 2 * (in - out) / 3},
            {"before in", in - 1},
        };

        // the camera: from the actor's front left, a little above, looking down at its hands and the ground
        constexpr float DISTANCE = 90.0f, PITCH = 35.0f, TURN = 35.0f, FOV = 60.0f;
        Components::Playback::MoveFrozenTick(out);
        Sleep(400);
        std::vector<std::optional<glm::vec3>> bones;
        RunOnMainThread([&] { bones = Actors::GetBonePositions(id, {"j_wrist_le", "j_wrist_ri", "j_mainroot"}); });
        glm::vec3 target = glm::vec3(sweepSpot) + glm::vec3(0.0f, 0.0f, 30.0f);
        if (bones.size() == 3 && bones[0] && bones[1] && bones[2])
            target = 0.25f * (*bones[0] + *bones[1]) + 0.5f * *bones[2];
        const auto yaw = glm::radians(sweepSpot.w + TURN);
        const auto pitch = glm::radians(PITCH);
        const auto position = target + DISTANCE * glm::vec3(std::cos(pitch) * std::cos(yaw),
                                                            std::cos(pitch) * std::sin(yaw), std::sin(pitch));
        auto& camera = Components::CameraManager::Get().GetActiveCamera();
        camera->GetPosition() = position;
        camera->GetRotation() = {PITCH, sweepSpot.w + TURN + 180.0f, 0.0f};
        camera->GetFov() = FOV;
        json["camera"] = {
            round(position.x), round(position.y), round(position.z), PITCH, sweepSpot.w + TURN + 180.0f, FOV};

        auto shots = nlohmann::json::array();
        for (std::size_t i = 0; i < moments.size() && frames; ++i)
        {
            const auto& [label, tick] = moments[i];
            Components::Playback::MoveFrozenTick(tick);
            Sleep(600);
            std::optional<Actors::MagazineState> state;
            std::uint32_t at = 0;
            RunOnMainThread([&] {
                state = Actors::GetMagazineState(id);
                at = Components::Playback::GetTimelineTick();
            });
            const auto file = std::format("sweep-{}-{}.png", animation, i + 1);
            const bool saved = GrabFrame(directory / "shots" / file, 640);
            shots.push_back({{"label", label},
                             {"tick", tick},
                             {"at", at},
                             {"file", saved ? file : std::string()},
                             {"offGun", state && state->offGun},
                             {"held", state ? state->held : std::string()},
                             {"dropped", state ? state->dropped.size() : 0}});
        }
        json["frames"] = shots;
        Log("SWEEP " + json.dump());
        if (check.error.empty())
            Log(std::format("{} reloadsweep {}:{}", check.passed ? "PASS" : "FAIL", animation, check.text));
        Components::Actors::Clear();
    }

    // the commands for characters and character zones (actor character, charactercheck, zone, assetentry,
    // modelmaterials, d3dflags); false for any other
    bool RunCharacterZoneCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "actor")
        {
            // actor character <actor> <character id> [normal|burnt|wounded [<upper> <lower>]]: makes the actor that
            // character, as the Look page does | actor character <actor> expect <character id> [normal|burnt|wounded]
            // [<model>...]: PASS when it is (with those models among its own)
            const auto position = args.tellg();
            std::string sub;
            args >> sub;
            if (sub == "loaded")
            {
                // actor loaded <actor> [ms]: waits until the models of the actor are loaded (from character
                // zones or a pack), PASS when they are
                std::string actorRef;
                std::uint32_t timeout = 120000;
                args >> actorRef >> timeout;
                const auto started = std::chrono::steady_clock::now();
                bool loaded = false;
                for (;;)
                {
                    const auto actor = Components::Actors::GetActor(ParseActorId(actorRef));
                    const auto catalog = Components::Actors::GetCatalog();
                    loaded = actor && catalog && catalog->HasModels(actor->models);
                    if (loaded || std::chrono::steady_clock::now() - started > std::chrono::milliseconds(timeout))
                        break;
                    Sleep(100);
                }
                Log(std::format(
                    "{} actor loaded {} in {} ms", loaded ? "PASS" : "FAIL", actorRef,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                        .count()));
                return true;
            }
            if (sub != "character")
            {
                args.clear();
                args.seekg(position);
                return false;
            }
            std::string actorRef, id;
            args >> actorRef >> id;
            const auto actor = Components::Actors::GetActor(ParseActorId(actorRef));
            const auto catalog = Components::Actors::GetCatalog();
            if (!actor || !catalog)
            {
                Log(std::format("FAIL actor character {}: no such actor or no catalog", actorRef));
                return true;
            }
            using Components::CharacterLogic::Variant;
            const auto variantOf = [](std::string_view text) {
                return text == "burnt" ? Variant::Burnt : text == "wounded" ? Variant::Wounded : Variant::Normal;
            };
            const auto variantName = [](Variant variant) {
                return variant == Variant::Burnt ? "burnt" : variant == Variant::Wounded ? "wounded" : "normal";
            };
            if (id == "expect")
            {
                std::string expected, variant;
                args >> expected >> variant;
                std::vector<std::string> models;
                for (std::string model; args >> model;)
                    models.push_back(model);
                auto character = Components::Actors::GetCharacter(*actor, *catalog);
                if (character && !character->burntOf.empty())
                    if (const auto base = catalog->FindCharacter(character->burntOf))
                        character = base;
                const auto look = character ? Components::CharacterLogic::GetLook(
                                                  *character, actor->models, catalog->FindCharacter(character->burnt))
                                            : Components::CharacterLogic::Look{};
                const bool hasModels = std::ranges::all_of(models, [&](const std::string& model) {
                    return std::ranges::find(actor->models, model) != actor->models.end();
                });
                std::string modelText;
                for (const auto& model : actor->models)
                    modelText += model + " ";
                const bool passed = character && character->id == expected &&
                                    (variant.empty() || variantOf(variant) == look.variant) && hasModels;
                Log(std::format("{} actor character {} is {} ({}, stored '{}'), made of {}", passed ? "PASS" : "FAIL",
                                actor->id, character ? character->id : "none", variantName(look.variant),
                                actor->character, modelText));
                return true;
            }
            const auto character = catalog->FindCharacter(id);
            if (!character)
            {
                Log(std::format("FAIL actor character {}: no character {}", actor->id, id));
                return true;
            }
            std::string variant;
            Components::CharacterLogic::Look look;
            args >> variant >> look.torso >> look.legs;
            look.variant = variantOf(variant);
            auto changed = *actor;
            Components::Actors::ApplyCharacter(changed, *character, look, *catalog);
            Components::Actors::Update(changed);
            std::string modelText;
            for (const auto& model : changed.models)
                modelText += model + " ";
            Log(std::format("ACTOR {} is {} ({}), made of {}, pack '{}'", changed.id, character->id,
                            variantName(look.variant), modelText, Components::Actors::GetPack()));
        }
        else if (name == "charactercheck")
        {
            // charactercheck: logs the catalog's characters (the map's and the campaign's) and what the loaded
            // character models are, and PASS when the campaign's read, the map has characters, and no loaded model a
            // character uses is filed as the name alone would say instead (the misfilings of B4)
            const auto catalog = Components::Actors::GetCatalog();
            if (!catalog)
            {
                Log("FAIL charactercheck: no catalog");
                return true;
            }
            std::size_t map = 0, campaign = 0, loaded = 0;
            for (const auto& character : catalog->characters)
            {
                (character.level.empty() ? map : campaign) += 1;
                loaded += catalog->HasModels(character.GetModels()) ? 1 : 0;
            }
            const auto& kinds = Components::Actors::GetCampaignModelKinds();
            std::size_t byCharacters = 0, misfiled = 0;
            for (const auto& model : catalog->models)
            {
                const auto kind = kinds.find(model.name);
                if (kind == kinds.end())
                    continue;
                ++byCharacters;
                if (kind->second != model.kind)
                {
                    ++misfiled;
                    Log(std::format("CHARACTERCHECK {} is {} but filed as {}", model.name,
                                    static_cast<int>(kind->second), static_cast<int>(model.kind)));
                }
            }
            Log(
                std::format("{} charactercheck: {} characters of the map, {} of the campaign and Zombies in {} levels, "
                            "{} loaded; {} loaded models filed by the characters, {} otherwise",
                            map > 0 && campaign >= 250 && misfiled == 0 ? "PASS" : "FAIL", map, campaign,
                            catalog->levels.size(), loaded, byCharacters, misfiled));
        }
        else if (name == "zone")
        {
            // zone <name>: loads a character zone (made by tools/t5-characters) and logs what it registered, shared
            // and had no room for, the time and memory it took, and its materials as the pack command does | zone
            // unload <name>: unloads it with the zones that go with it | zone list: logs the character zones and every
            // zone the game has loaded, with their flags | zone wanted: logs the zones the scene's actors need | zone
            // wait [ms]: waits until those are loaded (or failed), PASS when they are | zone expect <name>
            // loaded|absent | zone budget <zones> <MB>: what the game keeps loaded before unloading what no actor
            // needs | zone rescan: looks for the zones the setup built again
            std::string what;
            args >> what;
            const auto privateMegabytes = [] {
                PROCESS_MEMORY_COUNTERS_EX counters{};
                GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                     sizeof(counters));
                return static_cast<std::int64_t>(counters.PrivateUsage >> 20);
            };
            const auto loadedNames = [] {
                std::vector<std::string> names;
                RunOnMainThread([&] {
                    for (const auto& zone : ContentPacks::GetCharacterZones())
                        names.push_back(zone.name);
                });
                return names;
            };
            if (what == "wanted" || what == "wait")
            {
                std::uint32_t timeout = 30000;
                args >> timeout;
                const auto started = std::chrono::steady_clock::now();
                std::vector<std::string> wanted, missing;
                std::string error;
                for (;;)
                {
                    wanted = Components::Actors::GetWantedCharacterZones();
                    const auto loaded = loadedNames();
                    error = ContentPacks::GetCharacterZoneError();
                    missing.clear();
                    for (const auto& zone : wanted)
                        if (std::ranges::find(loaded, zone) == loaded.end())
                            missing.push_back(zone);
                    if (what == "wanted" || missing.empty() || !error.empty() ||
                        std::chrono::steady_clock::now() - started > std::chrono::milliseconds(timeout))
                        break;
                    Sleep(100);
                }
                std::string names, missingNames;
                for (const auto& zone : wanted)
                    names += zone + " ";
                for (const auto& zone : missing)
                    missingNames += zone + " ";
                const auto took =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
                if (what == "wanted")
                    Log(std::format("ZONE wanted {}(missing {}) error '{}'", names, missingNames, error));
                else
                    Log(std::format("{} zone wait: {}loaded in {} ms{}{}", missing.empty() ? "PASS" : "FAIL", names,
                                    took.count(), missing.empty() ? "" : ", missing " + missingNames,
                                    error.empty() ? "" : ", error " + error));
            }
            else if (what == "expect")
            {
                std::string zone, state;
                args >> zone >> state;
                const auto loaded = loadedNames();
                const bool isLoaded = std::ranges::find(loaded, zone) != loaded.end();
                Log(std::format("{} zone {} {} (expected {})", isLoaded == (state != "absent") ? "PASS" : "FAIL", zone,
                                isLoaded ? "loaded" : "absent", state));
            }
            else if (what == "most")
            {
                // zone most <n>: PASS when at most that many character zones are loaded, sound zones aside: they aren't
                // in the budget, and stay while the game's zones aren't short
                std::size_t most = 0;
                args >> most;
                const auto loaded = static_cast<std::size_t>(std::ranges::count_if(
                    loadedNames(), [](const std::string& name) { return !name.starts_with("t5s_"); }));
                Log(std::format("{} zone most {}: {} loaded", loaded <= most ? "PASS" : "FAIL", most, loaded));
            }
            else if (what == "weaponroom")
            {
                // zone weaponroom <n>: the free slots kept before a gun's zone loads, 24 as the mod has it
                std::uint32_t room = 24;
                args >> room;
                ContentPacks::SetWeaponRoomKept(room);
                Log(std::format("ZONE weaponroom {}", room));
            }
            else if (what == "budget")
            {
                std::size_t zones = 16;
                std::uint64_t megabytes = 512;
                args >> zones >> megabytes;
                ContentPacks::SetCharacterZoneBudget(zones, megabytes << 20);
                Log(std::format("ZONE budget {} zones, {} MB", zones, megabytes));
            }
            else if (what == "addressfloor")
            {
                // zone addressfloor <MB>|default: the free address space below which the character zones no actor
                // needs go whatever the budget (512 MB, a largest block of half that)
                std::string value;
                args >> value;
                const std::uint64_t megabytes = value == "default" ? 0 : std::strtoull(value.c_str(), nullptr, 10);
                ContentPacks::SetAddressSpaceFloor(megabytes << 20);
                Log(std::format("ZONE addressfloor {}", megabytes ? std::to_string(megabytes) + " MB" : "default"));
            }
            else if (what == "ignore")
            {
                // zone ignore on|off: the mod acts as if the setup hadn't built the character zones (for scenarios of
                // level packs)
                std::string state;
                args >> state;
                Components::Actors::IgnoreCharacterZones(state == "on");
                Log(std::format("ZONE ignore {}: character zones set up {}", state,
                                Components::Actors::HasCharacterZones()));
            }
            else if (what == "hold")
            {
                // zone hold on|off: the zones actors want aren't loaded while held (as if each took long), to see what
                // waits for them
                std::string state;
                args >> state;
                ContentPacks::HoldCharacterZoneLoads(state == "on");
                Log(std::format("ZONE hold {}", state));
            }
            else if (what == "rescan")
            {
                Components::Actors::RescanCharacterZones();
                Log(std::format("ZONE rescan: character zones set up {}", Components::Actors::HasCharacterZones()));
            }
            else if (what == "unload")
            {
                std::string zone, ifLoaded;
                args >> zone >> ifLoaded;
                // (zone unload <name> ifloaded: one that isn't loaded is fine too)
                bool loaded = true;
                if (ifLoaded == "ifloaded")
                    RunOnMainThread([&] {
                        loaded = std::ranges::any_of(
                            ContentPacks::GetCharacterZones(),
                            [&](const ContentPacks::CharacterZoneInfo& info) { return info.name == zone; });
                    });
                if (!loaded)
                {
                    Log(std::format("PASS zone unload {}: it isn't loaded", zone));
                    return true;
                }
                std::vector<std::string> gone;
                const auto before = privateMegabytes();
                const auto started = std::chrono::steady_clock::now();
                RunOnMainThread([&] { gone = ContentPacks::UnloadCharacterZone(zone); });
                const auto took =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
                std::string names;
                for (const auto& unloadedZone : gone)
                    names += unloadedZone + " ";
                Log(std::format("{} zone unload {}: unloaded {}in {} ms, memory {} -> {} MB",
                                std::ranges::find(gone, zone) != gone.end() ? "PASS" : "FAIL", zone, names,
                                took.count(), before, privateMegabytes()));
            }
            else if (what == "list")
            {
                std::vector<ContentPacks::CharacterZoneInfo> zones;
                std::vector<std::pair<std::string, std::int32_t>> loaded;
                std::string pools;
                RunOnMainThread([&] {
                    zones = ContentPacks::GetCharacterZones();
                    loaded = ContentPacks::GetLoadedZones();
                    for (const auto type : {Structures::ASSET_TYPE_XMODEL, Structures::ASSET_TYPE_MATERIAL,
                                            Structures::ASSET_TYPE_TECHNIQUE_SET, Structures::ASSET_TYPE_IMAGE})
                        pools += std::format(" {}:{}", type, ContentPacks::CountFreeSlots(type));
                });
                for (const auto& zone : zones)
                {
                    std::string models, dependsOn;
                    for (const auto& model : zone.models)
                        models += model + " ";
                    for (const auto& other : zone.dependsOn)
                        dependsOn += other + " ";
                    Log(std::format("ZONE character {} flags {:X} textures {:.1f} MB uses {}{}models {}", zone.name,
                                    zone.flags, zone.textureBytes / 1048576.0, dependsOn,
                                    zone.usesPack ? "the pack " : "", models));
                }
                std::string names;
                for (const auto& [zone, flags] : loaded)
                    names += std::format("{}:{:X} ", zone, flags);
                Log(std::format("ZONE list {} character zones, {} zones loaded: {}", zones.size(), loaded.size(),
                                names));
                Log("ZONE pools free (type:slots)" + pools);
            }
            else
            {
                const auto& zone = what;
                ContentPacks::LoadResult loaded;
                std::vector<std::string> incomplete;
                RunOnMainThread([&] {
                    loaded = ContentPacks::LoadCharacterZone(zone);
                    // checked before the next frame sorts the materials, which crashes on an incomplete one
                    incomplete = ContentPacks::FindIncompleteMaterials();
                });
                std::string counts;
                for (std::size_t type = 0; type < loaded.kept.size(); ++type)
                    if (loaded.kept[type] || loaded.poolFull[type] || loaded.shared[type])
                        counts += std::format("{}:{}/{}/{} ", type, loaded.kept[type], loaded.poolFull[type],
                                              loaded.shared[type]);
                Log(std::format("ZONE {} loaded {} error '{}' in {} ms, memory {} -> {} MB", zone, loaded.loaded,
                                loaded.error, loaded.milliseconds, loaded.memoryBefore >> 20,
                                loaded.memoryAfter >> 20));
                Log("ZONE type:registered/poolfull/shared " + counts);
                Log(std::format(
                    "ZONE released {} textures ({:.1f} MB) and {} shaders and declarations ({} kept, as its own "
                    "technique sets use them); replaced {} technique sets (adding {} constants and samplers), removed "
                    "{} techniques",
                    loaded.releasedTextures, loaded.releasedTextureBytes / 1048576.0, loaded.releasedShaders,
                    loaded.keptShaders, loaded.replacedTechniqueSets, loaded.addedArguments, loaded.removedTechniques));
                std::string models;
                for (const auto& model : loaded.models)
                    models += model + " ";
                Log("ZONE models " + models);
                for (const auto& note : loaded.notes)
                    Log("ZONE note " + note);
                for (const auto& material : incomplete)
                    Log("MATERIAL incomplete " + material);
                Log(std::format("{} zone {}: {} materials lack arguments",
                                loaded.loaded && loaded.error.empty() && incomplete.empty() ? "PASS" : "FAIL", zone,
                                incomplete.size()));
            }
        }
        else if (name == "d3dflags")
        {
            // d3dflags: logs how the game's Direct3D device was made (D3DCREATE_MULTITHREADED is 0x4)
            D3DDEVICE_CREATION_PARAMETERS parameters{};
            const auto device = D3D9::GetDevice();
            const bool read = device && SUCCEEDED(device->GetCreationParameters(&parameters));
            Log(std::format("D3DFLAGS read {} behavior {:X} multithreaded {}", read, parameters.BehaviorFlags,
                            (parameters.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0));
        }
        else if (name == "assetentry")
        {
            // assetentry <type> <name>: logs the asset table's entries of that type and name, stand-ins of unloaded
            // zones' assets too (see ContentPacks.cpp)
            std::int32_t type = 0;
            std::string asset;
            args >> type >> asset;
            std::vector<std::string> entries;
            RunOnMainThread([&] { entries = ContentPacks::DescribeEntries(type, asset); });
            for (const auto& entry : entries)
                Log(std::format("ASSETENTRY {} {}: {}", type, asset, entry));
            Log(std::format("ASSETENTRY {} {}: {} entries", type, asset, entries.size()));
        }
        else if (name == "modelmaterials")
        {
            // modelmaterials <model>: logs the materials of a registered model's surfaces with their technique sets
            // and images, and FAIL when one is a stand-in the game made for a material no loaded zone has (technique
            // set 2d, image default), which models are never drawn with
            std::string model;
            args >> model;
            std::vector<std::string> lines;
            std::size_t standIns = 0;
            bool found = false;
            RunOnMainThread([&] {
                const auto xmodel = static_cast<const Structures::XModel*>(
                    ContentPacks::FindAsset(Structures::ASSET_TYPE_XMODEL, model.c_str()));
                if (!xmodel)
                    return;
                found = true;
                std::set<const Structures::Material*> seen;
                for (std::uint32_t surface = 0; xmodel->materialHandles && surface < xmodel->numsurfs; ++surface)
                {
                    const auto material = xmodel->materialHandles[surface];
                    if (!material || !seen.insert(material).second)
                        continue;
                    const std::string techniqueSet = material->techniqueSet ? material->techniqueSet->name : "none";
                    // whether the technique set is the one registered by its name, and how many of its techniques
                    // have a pass without a vertex declaration for the format models are drawn with (type 1)
                    std::string setState;
                    if (material->techniqueSet)
                    {
                        std::size_t withoutModelFormat = 0;
                        for (const auto technique : material->techniqueSet->techniques)
                            for (std::uint32_t pass = 0; technique && pass < technique->passCount; ++pass)
                                if (const auto decl = technique->passArray[pass].vertexDecl;
                                    decl && !decl->declarations[1])
                                {
                                    ++withoutModelFormat;
                                    break;
                                }
                        const auto registered =
                            ContentPacks::FindAsset(Structures::ASSET_TYPE_TECHNIQUE_SET, techniqueSet.c_str());
                        setState = std::format(" {}, {} without type 1",
                                               registered == material->techniqueSet ? "registered" : "a copy",
                                               withoutModelFormat);
                    }
                    std::string images;
                    bool defaultImage = false;
                    std::size_t imageCopies = 0;
                    for (std::uint32_t i = 0; material->textureTable && i < material->textureCount; ++i)
                    {
                        const auto image = material->textureTable[i].image;
                        const std::string imageName = image && image->name ? image->name : "none";
                        defaultImage |= imageName == "default";
                        images += std::format("{}{}", images.empty() ? "" : ",", imageName);
                        if (image && ContentPacks::FindAsset(Structures::ASSET_TYPE_IMAGE, imageName.c_str()) != image)
                            ++imageCopies;
                    }
                    const bool standIn = techniqueSet == "2d" || defaultImage;
                    standIns += standIn ? 1 : 0;
                    lines.push_back(std::format("MATERIAL {} surface {} {} [{}{}] {}{}{}", model, surface,
                                                material->name, techniqueSet, setState, images,
                                                imageCopies ? std::format(" ({} not registered)", imageCopies) : "",
                                                standIn ? " STAND-IN" : ""));
                }
            });
            for (const auto& line : lines)
                Log(line);
            Log(std::format("{} modelmaterials {}: {} materials, {} stand-ins{}",
                            found && standIns == 0 ? "PASS" : "FAIL", model, lines.size(), standIns,
                            found ? "" : " (no such model)"));
        }
        else
            return false;
        return true;
    }

    // the bytes in use in a heap, or nothing when walking it failed (the game's other threads keep using their heaps)
    std::optional<std::uint64_t> GetHeapBusyBytes(HANDLE heap)
    {
        std::uint64_t busy = 0;
        bool walked = true;
        if (!HeapLock(heap))
            return std::nullopt;
        __try
        {
            PROCESS_HEAP_ENTRY entry{};
            while (HeapWalk(heap, &entry))
                if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
                    busy += entry.cbData;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            walked = false;
        }
        HeapUnlock(heap);
        return walked ? std::optional(busy) : std::nullopt;
    }

    struct MemoryStats
    {
        std::uint64_t privateBytes = 0;
        std::uint64_t committedPrivate = 0, committedMapped = 0, committedImage = 0, reserved = 0, free = 0, largestFree = 0;
        std::vector<std::pair<HANDLE, std::optional<std::uint64_t>>> heaps;
    };

    MemoryStats GetMemoryStats(bool walkHeaps)
    {
        MemoryStats stats;
        PROCESS_MEMORY_COUNTERS_EX counters{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters));
        stats.privateBytes = counters.PrivateUsage;
        MEMORY_BASIC_INFORMATION info{};
        for (std::uintptr_t address = 0; VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info));)
        {
            if (info.State == MEM_COMMIT)
                (info.Type == MEM_IMAGE    ? stats.committedImage
                 : info.Type == MEM_MAPPED ? stats.committedMapped
                                           : stats.committedPrivate) += info.RegionSize;
            else if (info.State == MEM_RESERVE)
                stats.reserved += info.RegionSize;
            else
            {
                stats.free += info.RegionSize;
                stats.largestFree = std::max<std::uint64_t>(stats.largestFree, info.RegionSize);
            }
            const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (next <= address)
                break;
            address = next;
        }
        if (walkHeaps)
        {
            std::vector<HANDLE> heaps(GetProcessHeaps(0, nullptr));
            heaps.resize(GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data()));
            for (const auto heap : heaps)
                stats.heaps.emplace_back(heap, GetHeapBusyBytes(heap));
        }
        return stats;
    }

    // memstat line: the address space, the heaps and what the mod holds, in one line; run-scenarios logs it as each
    // scenario starts
    std::string DescribeMemoryLine()
    {
        constexpr double MB = 1048576.0;
        const auto started = GetTickCount64();
        const auto stats = GetMemoryStats(false);
        std::vector<HANDLE> heaps(GetProcessHeaps(0, nullptr) + 16);
        heaps.resize(GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data()));
        std::uint64_t busy = 0, free = 0, large = 0, otherBusy = 0;
        std::uint32_t unwalked = 0;
        MemoryWatch::HeapUse process;
        for (const auto heap : heaps)
        {
            const auto use = MemoryWatch::WalkHeap(heap);
            if (!use.walked)
                ++unwalked;
            busy += use.busy;
            free += use.free;
            large += use.large;
            if (heap == GetProcessHeap())
                process = use;
            else
                otherBusy += use.busy;
        }
        const auto walked = GetTickCount64() - started;
        const auto pack = Components::TexturePack::GetStats();
        std::size_t zones = 0, characterZones = 0;
        RunOnMainThread([&] {
            zones = ContentPacks::GetLoadedZones().size();
            characterZones = ContentPacks::GetCharacterZones().size();
        });
        return std::format(
            "MEMLINE free {:.0f} MB, largest {:.0f} MB, private {:.0f} MB, committed private {:.0f} mapped {:.0f} "
            "image {:.0f}, reserved {:.0f}; heaps in use {:.0f} MB ({:.0f} in blocks of 508 KB and up), free inside "
            "{:.0f}; the process's heap in use {:.0f}, free inside {:.0f}; other heaps in use {:.0f} in {}{}; pack {:.0f} "
            "MB on the card; zones {} (character {}); walked in {} ms",
            stats.free / MB, stats.largestFree / MB, stats.privateBytes / MB, stats.committedPrivate / MB,
            stats.committedMapped / MB, stats.committedImage / MB, stats.reserved / MB, busy / MB, large / MB,
            free / MB, process.busy / MB, process.free / MB, otherBusy / MB, heaps.size() - 1,
            unwalked ? std::format(" ({} not walked)", unwalked) : "", pack.inUse / MB, zones, characterZones, walked);
    }

    // memstat watch on [bytes] | off | mark | report [since] [rows] | peaks [rows]: MemoryWatch, who holds heap blocks of
    // at least that size (65536) and reservations of address space, taken while on, and who held the most at once
    void RunMemoryWatch(std::istringstream& args)
    {
        std::string what;
        args >> what;
        if (what == "on")
        {
            std::size_t bytes = 0;
            if (!(args >> bytes))
                bytes = 65536;
            std::string error;
            Log(MemoryWatch::Start(bytes, &error)
                    ? std::format("MEMWATCH on, heap blocks of {} bytes and up, and reservations", bytes)
                    : "FAIL memstat watch: " + error);
        }
        else if (what == "off")
        {
            MemoryWatch::Stop();
            Log("MEMWATCH off");
        }
        else if (what == "mark")
        {
            MemoryWatch::Mark();
            Log("MEMWATCH marked");
        }
        else if (what == "report")
        {
            bool since = false;
            std::size_t rows = 16;
            for (std::string word; args >> word;)
            {
                if (word == "since")
                    since = true;
                else
                    rows = std::strtoul(word.c_str(), nullptr, 10);
            }
            for (const auto& line : MemoryWatch::Report(since, rows))
                Log(line);
        }
        else if (what == "peaks")
        {
            std::size_t rows = 0;
            if (!(args >> rows))
                rows = 16;
            for (const auto& line : MemoryWatch::ReportPeaks(rows))
                Log(line);
        }
        else
            Log("FAIL memstat watch on|off|mark|report|peaks");
    }

    // what statewatch sampled (see RunDiagnosticCommand), complete once done
    std::atomic<bool> stateWatchDone = true;
    std::vector<std::string> stateChanges;

    // Waits until the render thread has drawn two more frames of the overlay (or a second has gone by), so that what
    // was posted before has been taken in a frame of its own: posted a few ms apart, a mouse move and a release came in
    // one frame when the game held up the overlay's frames, and a splitter dragged ended where the frame before had it
    void WaitForOverlayFrames()
    {
        const auto frames = renderThreadFrames.load();
        for (const auto started = GetTickCount64();
             renderThreadFrames.load() - frames < 2 && GetTickCount64() - started < 1000;)
            Sleep(1);
    }

    // what inputwatch recorded (see RunDiagnosticCommand): until then, on the render thread, each frame's mouse
    std::atomic<std::uint64_t> inputWatchUntil = 0;
    std::vector<std::string> inputFrames;

    void RecordInputFrame()
    {
        const auto now = GetTickCount64();
        if (now >= inputWatchUntil)
            return;
        const auto& g = *ImGui::GetCurrentContext();
        const auto& io = ImGui::GetIO();
        std::string events;
        for (const auto& event : g.InputEventsTrail)
            if (event.Type == ImGuiInputEventType_MouseButton)
                events += std::format(" button{}{}", event.MouseButton.Button, event.MouseButton.Down ? "down" : "up");
        std::string stop = "none";
        for (const auto& [name, drawn] : ImGuiEx::Keyframeable::GetDrawn())
            if (name == "demo.stop")
                stop = std::format("{:.0f},{:.0f}-{:.0f},{:.0f}", drawn.min.x, drawn.min.y, drawn.max.x, drawn.max.y);
        if (events.empty() && !io.MouseDown[0] && !io.MouseReleased[0] && !g.ActiveId && inputFrames.size() > 1 &&
            inputFrames.back().ends_with(" idle"))
            return;
        inputFrames.push_back(
            std::format("{} frame {} events{} down {} clicked {} released {} at {:.0f},{:.0f} hovered "
                        "{:X} active {:X} stop {}{}",
                        now, g.FrameCount, events.empty() ? " none" : events, io.MouseDown[0], io.MouseClicked[0],
                        io.MouseReleased[0], io.MousePos.x, io.MousePos.y, g.HoveredId, g.ActiveId, stop,
                        events.empty() && !io.MouseDown[0] && !g.ActiveId ? " idle" : ""));
    }

    // commands for finding out what the game does (see RunCharacterZoneCommand for why they are apart)
    bool RunDiagnosticCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "statewatch")
        {
            // statewatch <ms> | statewatch wait: samples every few ms on a thread of its own, for that long, whether
            // the game is in a demo (cl_ingame, demoplaying), a rewind, the pause and the demo's name, so other
            // commands run meanwhile; wait logs each change, with cg.time and the client's server time and their
            // jumps (STATEWATCH lines), once the time is up
            std::string what;
            args >> what;
            while (!stateWatchDone)
                Sleep(10);
            if (what == "wait")
            {
                for (const auto& change : stateChanges)
                    Log("STATEWATCH " + change);
                Log(std::format("STATEWATCH {} changes", stateChanges.size()));
                stateChanges.clear();
                return true;
            }
            stateChanges.clear();
            stateWatchDone = false;
            const auto milliseconds = std::strtoull(what.c_str(), nullptr, 10);
            std::thread([milliseconds, module = ReferenceModule()] {
                const auto started = GetTickCount64();
                std::string last;
                std::int32_t lastTime = 0;
                auto renderFrames = renderThreadFrames.load();
                auto renderFrameAt = started;
                while (GetTickCount64() - started < milliseconds && !ejecting)
                {
                    // and when the render thread (which draws the UI) drew no frame for a while
                    if (const auto frames = renderThreadFrames.load(); frames != renderFrames)
                    {
                        const auto now = GetTickCount64();
                        if (now - renderFrameAt > 30)
                            stateChanges.push_back(std::format("{} ms the render thread drew no frame for {} ms",
                                                               now - started, now - renderFrameAt));
                        renderFrames = frames;
                        renderFrameAt = now;
                    }
                    const auto clc = Structures::GetClientConnection();
                    const auto cg = Structures::GetClientGlobals();
                    const auto inGame = Functions::FindDvar("cl_ingame");
                    const std::int32_t time = cg ? cg->time : 0;
                    const auto state =
                        std::format("{} ingame {} demoplaying {} rewinding {} paused {} server '{}'",
                                    magic_enum::enum_name(Mod::GetGameInterface()->GetGameState()),
                                    inGame && inGame->current.enabled ? 1 : 0, clc->demoplaying,
                                    Components::Rewinding::IsRewinding(), Components::Playback::IsPaused(),
                                    Structures::GetClientStatic()->servername);
                    const bool jumped = time < lastTime || time > lastTime + 300;
                    if (state != last || jumped)
                        stateChanges.push_back(
                            std::format("{} ms {} cg.time {} serverTime {}{}", GetTickCount64() - started, state, time,
                                        Structures::GetClientActive()->serverTime, jumped ? " (time jumped)" : ""));
                    last = state;
                    lastTime = time;
                    Sleep(2);
                }
                stateWatchDone = true;
                FreeLibraryAndExitThread(module, 0);
            }).detach();
        }
        else if (name == "stallmain")
        {
            // stallmain <ms> [frames]: the main thread sleeps that long in each of that many frames (1) once a demo
            // plays (cg.time set), as it stalls while the shaders a load needs are compiled; before the demo command
            // (load-test.ps1's "pre:"), it does so as the demo starts
            int milliseconds = 0, frames = 1;
            args >> milliseconds >> frames;
            stallMilliseconds = milliseconds;
            stallFrames = frames;
            Log(std::format("STALLMAIN {} ms in {} frames", milliseconds, frames));
        }
        else if (name == "strayclient")
        {
            // strayclient <entity> <local client>: once a demo waits for its first snapshot (cg.time 0), writes that
            // local client into the entity, as Plutonium's stray write does (Hooks/EntityClients.cpp); given before the
            // demo (load-test.ps1's "pre:")
            int entity = -1, value = 0;
            args >> entity >> value;
            if (entity >= 0 && entity < static_cast<int>(Structures::MAX_GENTITIES))
            {
                strayValue = value;
                strayEntity = entity;
            }
            Log(std::format("STRAYCLIENT entity {} local client {}", entity, value));
        }
        else if (name == "framelimit")
        {
            // logs how many frames of demos were held to com_maxFrameTime and the milliseconds they lost
            // (Hooks/Playback.cpp)
            const auto stats = Hooks::Playback::GetFrameLimitStats();
            Log(std::format("FRAMELIMIT frames {} ms {}", stats.frames, stats.milliseconds));
        }
        else if (name == "inputwatch")
        {
            // inputwatch <ms> | inputwatch wait: records each frame of the overlay for that long (on the render
            // thread): the mouse button events ImGui took that frame, the button's state, clicks and releases, where
            // the mouse is, the item hovered and active and where demo.stop was drawn; wait logs them (INPUTWATCH
            // lines, frames with nothing going on once)
            std::string what;
            args >> what;
            if (what == "wait")
            {
                while (GetTickCount64() < inputWatchUntil)
                    Sleep(10);
                std::vector<std::string> frames;
                RunOnRenderThread([&] { frames.swap(inputFrames); });
                for (const auto& frame : frames)
                    Log("INPUTWATCH " + frame);
                return true;
            }
            RunOnRenderThread([&] {
                inputFrames.clear();
                inputWatchUntil = GetTickCount64() + std::strtoull(what.c_str(), nullptr, 10);
            });
        }
        else if (name == "waitscene")
        {
            // waitscene [ms]: waits up to ms (60000) until the demo's scene has been read from its file (its actors are
            // back), which is a few seconds after the demo has started to play; PASS or FAIL
            DWORD timeout = 60000;
            if (!(args >> timeout))
                timeout = 60000;
            const auto started = GetTickCount64();
            while (Components::Actors::IsSceneLoadPending() && GetTickCount64() - started < timeout)
                Sleep(50);
            const bool read = !Components::Actors::IsSceneLoadPending();
            Log(std::format("{} waitscene after {} ms", read ? "PASS" : "FAIL", GetTickCount64() - started));
        }
        else if (name == "rewindclick")
        {
            // rewindclick <control> [ms]: finds where a control that records where it was drawn is (as control does),
            // waits up to ms (30000) until the demo rewinds (its end rewinds it to a second before) and clicks it once
            // the game catches up with where it rewound to and the render thread, which draws the overlay, has drawn
            // no frame for 15 ms (then none for 100-150 ms), the press and the release posted together so that they
            // reach the overlay in one frame, as a click in that time does; PASS when it rewound
            std::string control;
            DWORD timeout = 30000;
            args >> control;
            if (!(args >> timeout))
                timeout = 30000;
            std::optional<ImGuiEx::Keyframeable::Drawn> found;
            RunOnRenderThread([&] {
                for (const auto& [each, drawn] : ImGuiEx::Keyframeable::GetDrawn())
                    if (each == control)
                        found = drawn;
            });
            if (!found)
            {
                Log(std::format("FAIL rewindclick: no {} drawn", control));
                return true;
            }
            const POINT at{static_cast<LONG>(std::lround((found->min.x + found->max.x) / 2)),
                           static_cast<LONG>(std::lround((found->min.y + found->max.y) / 2))};
            PostMouse(WM_MOUSEMOVE, at.x, at.y);
            const auto started = GetTickCount64();
            bool rewound = false;
            while (!rewound && GetTickCount64() - started < timeout)
            {
                rewound = Components::Rewinding::IsRewinding();
                if (!rewound)
                    Sleep(1);
            }
            // (the catching up starts as the rewind is done, and then the render thread draws no frame for 100-150 ms)
            while (rewound && Components::Rewinding::IsRewinding() && GetTickCount64() - started < timeout)
                Sleep(1);
            const auto rewoundAt = GetTickCount64();
            auto frames = renderThreadFrames.load();
            for (auto frameAt = rewoundAt;
                 rewound && GetTickCount64() - frameAt < 15 && GetTickCount64() - rewoundAt < 1000; Sleep(1))
            {
                if (const auto now = renderThreadFrames.load(); now != frames)
                {
                    frames = now;
                    frameAt = GetTickCount64();
                }
            }
            // (both at once, as a click quicker than that does: the overlay takes them in one frame)
            if (rewound)
            {
                PostMessageA(GetGameWindow(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(at.x, at.y));
                PostMessageA(GetGameWindow(), WM_LBUTTONUP, 0, MAKELPARAM(at.x, at.y));
                Sleep(40);
            }
            Log(std::format("{} rewindclick {} at {} {}, {} after {} ms", rewound ? "PASS" : "FAIL", control, at.x,
                            at.y, rewound ? "clicked as the demo rewound" : "no rewind", GetTickCount64() - started));
        }
        else if (name == "memstat")
        {
            // memstat [heaps] | memstat mark | memstat expect <MB>: logs the process's private bytes, its committed
            // memory by kind (private, mapped, image) and with heaps the bytes in use in each heap (HeapWalk), to
            // tell where memory grows; remembers the bytes in use in the process's heap (where the game allocates and
            // the duplicates' leak showed; private bytes include its free pages); PASS when they grew at most that much
            // since
            static std::uint64_t marked = 0;
            std::string what;
            args >> what;
            if (what == "line")
            {
                Log(DescribeMemoryLine());
                return true;
            }
            if (what == "map")
            {
                std::size_t rows = 0;
                if (!(args >> rows))
                    rows = 12;
                for (const auto& line : MemoryWatch::DescribeAddressSpace(rows))
                    Log(line);
                return true;
            }
            if (what == "watch")
            {
                RunMemoryWatch(args);
                return true;
            }
            if (what == "compact")
            {
                // memstat compact: what the heaps give back when asked (HeapCompact on each, then Windows' resource
                // optimization of every heap), with the memory line before and after
                Log(DescribeMemoryLine());
                std::vector<HANDLE> heaps(GetProcessHeaps(0, nullptr) + 16);
                heaps.resize(GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data()));
                std::size_t largest = 0;
                for (const auto heap : heaps)
                    largest = std::max<std::size_t>(largest, HeapCompact(heap, 0));
                HEAP_OPTIMIZE_RESOURCES_INFORMATION optimize{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
                const bool optimized =
                    HeapSetInformation(nullptr, HeapOptimizeResources, &optimize, sizeof(optimize)) != FALSE;
                Log(std::format("MEMSTAT compacted {} heaps (largest free block left {:.1f} MB), optimized {}",
                                heaps.size(), largest / 1048576.0, optimized));
                Log(DescribeMemoryLine());
                return true;
            }
            const auto stats = GetMemoryStats(what == "heaps");
            constexpr double MB = 1048576.0;
            const auto heapBusy = [] { return GetHeapBusyBytes(GetProcessHeap()).value_or(0); };
            if (what == "mark")
                marked = heapBusy();
            if (what == "expect")
            {
                double most = 0.0;
                args >> most;
                const auto grown = (static_cast<double>(heapBusy()) - static_cast<double>(marked)) / MB;
                Log(std::format("{} memstat the process's heap grew {:.1f} MB since the mark (at most {:.1f})",
                                grown <= most ? "PASS" : "FAIL", grown, most));
            }
            std::string heaps;
            for (const auto& [heap, busy] : stats.heaps)
                heaps += busy ? std::format(" {:X}:{:.1f}", reinterpret_cast<std::uintptr_t>(heap), *busy / MB)
                              : std::format(" {:X}:?", reinterpret_cast<std::uintptr_t>(heap));
            Log(
                std::format("MEMSTAT private {:.1f} MB, committed private {:.1f} mapped {:.1f} image {:.1f}, "
                            "reserved {:.1f}, free {:.1f}{}{}",
                            stats.privateBytes / MB, stats.committedPrivate / MB, stats.committedMapped / MB,
                            stats.committedImage / MB, stats.reserved / MB, stats.free / MB,
                            heaps.empty() ? "" : ", heaps busy MB", heaps));
        }
        else
            return false;
        return true;
    }

    // the scene's lights (light)
    bool RunLightCommand(const std::string& name, std::istringstream& args)
    {
        // light add <point|spot> crosshair: as Add Light does (ActorActions::AddLight), on the surface where the camera
        //   looks, a spot where the camera is, aimed at it; the latest becomes last
        // light add <point|spot> <x y z> <r g b> <intensity> <radius>: at a point, from the playhead
        // light set <actor> color <r g b> | intensity <x> | radius <x> | cone <degrees> | softness <x> | shadows <0|1>
        //   | pitch <degrees> | yaw <degrees> | aim <x y z> | at <x y z> | bar <start> <end> | flicker <amount> <rate>
        //   | hidden <0|1>
        //   | follow <player|actor> <id> <bone> [x y z] | follow off | firstperson <auto|body|viewgun> | steady
        //   <seconds>
        // light key <actor> <color|intensity|radius|cone> <tick|now> <value...>: keys it, as a row's diamond does
        // light keys <actor> clear: its keys go
        // light keys all clear: every light's keys go, those of lights not in the scene too
        // light orphan <light's own id> <tick>: a key of a light not in the scene, as a demo's file keeps for a scene
        //   not loaded
        // light state [<actor> expect at <x y z> <distance> | expect aim <x y z> <degrees> | expect on|off | expect
        // kind
        //   <point|spot> | expect bar <start> <end> <ticks> | expect keys <n> | expect key <channel> <tick> <ticks> |
        //   expect cone|radius|highlights|softness <x> <tolerance> | expect flicker <amount> <tolerance> | expect
        //   follows <bone|off>]: logs what the renderer did last frame, or a light's place, and PASS when it is that
        //   close, points within those degrees of that direction, is on or off at the timeline's tick, is that kind,
        //   its bar is that, it has n keys in all, its channel has a key that close to a tick, its value at the
        //   timeline's tick is that, or it follows that bone
        // light expect pass <n> | shadow <0|1> | visible <n>: PASS when the additive pass drew n lights, the shadowed
        //   spot has its shadow map (or no spot does), or the game's own way draws n lights
        if (name != "light")
            return false;
        namespace LightLogic = Components::LightLogic;
        const auto channelOf = [](const std::string& word) -> std::optional<LightLogic::Channel> {
            if (word == "color")
                return LightLogic::Channel::Color;
            if (word == "intensity")
                return LightLogic::Channel::Intensity;
            if (word == "radius")
                return LightLogic::Channel::Radius;
            if (word == "cone")
                return LightLogic::Channel::Cone;
            if (word == "position")
                return LightLogic::Channel::Position;
            if (word == "aim")
                return LightLogic::Channel::Aim;
            return std::nullopt;
        };

        std::string action;
        args >> action;
        if (action == "add")
        {
            std::string kindName, where;
            args >> kindName >> where;
            const auto kind = kindName == "spot" ? Types::LightKind::Spot : Types::LightKind::Point;
            if (where == "crosshair")
            {
                RunOnRenderThread([&] {
                    const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                    UI::ActorActions::AddLight(kind, camera->GetPosition(), camera->GetForwardVector());
                });
                const auto actors = Components::Actors::GetActors();
                const auto id = actors.empty() || !actors.back().light ? 0 : actors.back().id;
                if (id)
                    previousActorId = std::exchange(lastActorId, id);
                Log(id ? std::format("LIGHT added {} as actor {} at the crosshair", kindName, id)
                       : std::string("FAIL light add: it wasn't added"));
                return true;
            }
            glm::vec3 position{};
            std::istringstream x(where);
            if (!(x >> position.x) || !(args >> position.y >> position.z))
            {
                Log("FAIL light add: give a kind and crosshair, or a kind and a point");
                return true;
            }
            const auto tick = Components::Playback::GetActorTick();
            auto actor = LightLogic::MakeLight(kind, position, {0.0f, 0.0f, 1.0f}, position, tick);
            actor.position = position;
            actor.light->pitch = 45.0f;
            if (glm::vec3 color; args >> color.r >> color.g >> color.b)
                actor.light->color = color;
            if (float intensity; args >> intensity)
                actor.light->intensity = intensity;
            if (float radius; args >> radius)
                actor.light->radius = radius;
            actor.name = Components::Actors::MakeName(LightLogic::GetBaseName(kind));
            const auto id = Components::Actors::Add(actor);
            previousActorId = std::exchange(lastActorId, id);
            Log(std::format("LIGHT added {} as actor {}: bar {}-{}", actor.name, id, actor.light->start,
                            actor.light->end));
            return true;
        }

        if (action == "hold")
        {
            // light hold on|off: a light on a bone that's gone stays where it last was, or goes back where it was put
            std::string state;
            args >> state;
            Lights::SetKeepsFollowed(state != "off");
            return true;
        }

        if (action == "watch")
        {
            // light watch <frames> [label]: what the renderer did over that many frames: the least and most of each of
            // its counts, and on how many frames the shadowed spot had its map
            int frames = 120;
            std::string label;
            args >> frames >> label;
            auto least = Lights::GetFrameStats(), most = least;
            int shadowMap = 0, seen = 0, changes = 0;
            auto last = least;
            const auto start = mainThreadFrames.load();
            for (std::uint32_t frame = start; frame < start + static_cast<std::uint32_t>(frames); ++seen)
            {
                const auto stats = Lights::GetFrameStats();
                least.pass = std::min(least.pass, stats.pass);
                most.pass = std::max(most.pass, stats.pass);
                least.visible = std::min(least.visible, stats.visible);
                most.visible = std::max(most.visible, stats.visible);
                least.added = std::min(least.added, stats.added);
                most.added = std::max(most.added, stats.added);
                least.litSurfaces = std::min(least.litSurfaces, stats.litSurfaces);
                most.litSurfaces = std::max(most.litSurfaces, stats.litSurfaces);
                shadowMap += stats.shadowMap ? 1 : 0;
                changes += stats.shadowMap != last.shadowMap || stats.pass != last.pass ? 1 : 0;
                last = stats;
                while (mainThreadFrames.load() == frame)
                    Sleep(1);
                frame = mainThreadFrames.load();
            }
            Log(std::format("LIGHTWATCH {} frames {}: pass {} to {}, added {} to {}, visible {} to {}, lit {} to {}, "
                            "shadow map on {}, changes {}",
                            label, seen, least.pass, most.pass, least.added, most.added, least.visible, most.visible,
                            least.litSurfaces, most.litSurfaces, shadowMap, changes));
            return true;
        }

        if (action == "expect")
        {
            std::string what;
            std::int32_t wanted = 0;
            args >> what >> wanted;
            const auto stats = Lights::GetFrameStats();
            const auto got = what == "pass"     ? stats.pass
                             : what == "shadow" ? static_cast<std::int32_t>(stats.shadowMap)
                                                : stats.visible;
            Log(std::format("{} light expect {} {}: {}", got == wanted ? "PASS" : "FAIL", what, wanted, got));
            return true;
        }

        std::string which;
        args >> which;
        if (action == "keys" && which == "all")
        {
            RunOnRenderThread([] {
                auto& keyframeManager = Components::KeyframeManager::Get();
                for (const auto type : Components::RuntimeProperties::GetAll())
                    if (Components::RuntimeProperties::IsLight(type) && keyframeManager.HasProperty(type))
                        keyframeManager.ClearKeyframes(keyframeManager.GetProperty(type));
            });
            Log("LIGHT all keys cleared");
            return true;
        }
        if (action == "orphan")
        {
            std::uint32_t tick = 0;
            args >> tick;
            RunOnRenderThread([&] {
                auto& keyframeManager = Components::KeyframeManager::Get();
                const auto& property =
                    Components::Lights::GetProperty(static_cast<std::uint32_t>(std::stoul(which)), LightLogic::Channel::Cone);
                Types::KeyframeValue value;
                value.floatingPoint = 30.0f;
                keyframeManager.AddKeyframe(property, Types::Keyframe(property, tick, value));
                keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
            });
            Log(std::format("LIGHT a key of light {} at {}", which, tick));
            return true;
        }
        if (action == "state" && which.empty())
        {
            const auto stats = Lights::GetFrameStats();
            Log(std::format("LIGHT last frame: pass {} in {} draws, {} lit surfaces, game's own {} added {} visible, "
                            "shadowed {} (map {}), exposure {:.4f}",
                            stats.pass, stats.batches, stats.litSurfaces, stats.added, stats.visible, stats.shadowed,
                            stats.shadowMap, stats.exposure));
            return true;
        }

        const auto id = ParseActorId(which);
        auto actor = Components::Actors::GetActor(id);
        if (!actor || !actor->light)
        {
            Log(std::format("FAIL light {}: {} isn't a light", action, which));
            return true;
        }
        auto& light = *actor->light;
        if (action == "set")
        {
            std::string what;
            args >> what;
            if (what == "color")
                args >> light.color.r >> light.color.g >> light.color.b;
            else if (what == "intensity")
                args >> light.intensity;
            else if (what == "radius")
                args >> light.radius;
            else if (what == "cone")
                args >> light.cone;
            else if (what == "softness")
                args >> light.softness;
            else if (what == "shadows")
                args >> light.shadows;
            else if (what == "pitch")
                args >> light.pitch;
            else if (what == "yaw")
                args >> actor->yaw;
            else if (what == "at")
                args >> actor->position.x >> actor->position.y >> actor->position.z;
            else if (what == "aim")
            {
                glm::vec3 point{};
                args >> point.x >> point.y >> point.z;
                const auto facing = ActorLogic::GetFacing(point - actor->position);
                light.pitch = facing.x;
                actor->yaw = facing.y;
            }
            else if (what == "bar")
                args >> light.start >> light.end;
            else if (what == "hidden")
                args >> actor->hidden;
            else if (what == "flicker")
                args >> light.flickerAmount >> light.flickerRate;
            else if (what == "firstperson" && light.follow)
            {
                std::string mode;
                args >> mode;
                light.follow->firstPerson = mode == "body"      ? Types::LightFirstPerson::Body
                                            : mode == "viewgun" ? Types::LightFirstPerson::ViewGun
                                                                : Types::LightFirstPerson::Auto;
            }
            else if (what == "steady" && light.follow)
                args >> light.follow->steady;
            else if (what == "follow")
            {
                std::string kind;
                args >> kind;
                if (kind == "off")
                    light.follow.reset();
                else
                {
                    Types::LightFollow follow;
                    follow.target.kind = kind == "actor" ? Types::BoneCameraTargetKind::Actor
                                                         : Types::BoneCameraTargetKind::Player;
                    std::string target;
                    args >> target >> follow.bone;
                    follow.target.id =
                        kind == "actor" ? static_cast<std::int32_t>(ParseActorId(target)) : std::atoi(target.c_str());
                    if (glm::vec3 offset; args >> offset.x >> offset.y >> offset.z)
                        follow.offset = offset;
                    light.follow = follow;
                }
            }
            else
            {
                Log(std::format("FAIL light set: no {}", what));
                return true;
            }
            Components::Actors::Update(*actor);
            Log(std::format("LIGHT {} set {}", id, what));
            return true;
        }

        if (action == "key")
        {
            std::string channelName, tickText;
            args >> channelName >> tickText;
            const auto channel = channelOf(channelName);
            if (!channel)
            {
                Log(std::format("FAIL light key: no {}", channelName));
                return true;
            }
            const auto tick = tickText == "now" ? Components::Playback::GetTimelineTick()
                                                : static_cast<std::uint32_t>(std::stoul(tickText));
            Types::KeyframeValue value;
            if (*channel == LightLogic::Channel::Color || *channel == LightLogic::Channel::Position ||
                *channel == LightLogic::Channel::Aim)
                args >> value.vector3.x >> value.vector3.y >> value.vector3.z;
            else
                args >> value.floatingPoint;
            RunOnRenderThread([&] {
                auto& keyframeManager = Components::KeyframeManager::Get();
                const auto& property = Components::Lights::GetProperty(light.id, *channel);
                keyframeManager.AddKeyframe(property, Types::Keyframe(property, tick, value));
                keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
            });
            Log(std::format("LIGHT {} keyed {} at {}", id, channelName, tick));
            return true;
        }

        if (action == "keys")
        {
            RunOnRenderThread([&] { Components::Lights::ClearKeys(light.id); });
            Log(std::format("LIGHT {} keys cleared", id));
            return true;
        }

        if (action == "steps")
        {
            // light steps <actor> <frames>|capture [label [dump | expect <least mean> <most still> <most move> [<most
            // turn>]]]: how it moved over that many frames, or those of the last capture: the mean and most of a
            // frame's move, how many frames are more than 3 times the mean (jumps), how many didn't move, its aim's
            // most turn and on how many frames the gun on screen placed it; dump logs each frame; PASS when it moved at
            // least that mean, stood still on at most so many frames, moved at most that much in one and turned at most
            // so many degrees in one
            std::string framesText, label, dump;
            float leastMean = 0.0f, mostMove = 0.0f, mostTurn = 360.0f;
            std::int64_t mostStill = 0;
            args >> framesText >> label >> dump;
            if (dump == "expect")
                args >> leastMean >> mostStill >> mostMove >> mostTurn;
            const auto frames = static_cast<std::size_t>(std::atoi(framesText.c_str()));
            const auto start = mainThreadFrames.load();
            while (mainThreadFrames.load() < start + static_cast<std::uint32_t>(frames) + 1)
                Sleep(5);
            auto placed = Lights::GetPlacedLights();
            std::erase_if(placed, [&](const auto& each) { return each.id != light.id; });
            if (framesText == "capture")
            {
                const auto end = std::find_if(placed.rbegin(), placed.rend(), [](const auto& each) {
                                     return each.capturing;
                                 }).base();
                const auto first = std::find_if(std::make_reverse_iterator(end), placed.rend(), [](const auto& each) {
                                       return !each.capturing;
                                   }).base();
                placed = {first, end};
                // (the first frames of a capture are drawn while the demo still stands)
                while (placed.size() > 1 && placed[1].tick == placed[0].tick)
                    placed.erase(placed.begin());
            }
            else if (placed.size() > frames)
            {
                placed.erase(placed.begin(), placed.end() - frames);
            }
            if (placed.size() < 3)
            {
                Log("FAIL light steps: too few frames");
                return true;
            }
            std::vector<float> moved;
            float turned = 0.0f;
            for (std::size_t i = 1; i < placed.size(); ++i)
            {
                moved.push_back(glm::distance(placed[i - 1].position, placed[i].position));
                const auto dot = glm::dot(glm::normalize(placed[i - 1].direction), glm::normalize(placed[i].direction));
                const auto turn = glm::degrees(std::acos(std::clamp(dot, -1.0f, 1.0f)));
                turned = std::max(turned, turn);
                if (dump == "dump")
                    Log(std::format("LIGHTSTEP tick {} moved {:.3f} turned {:.2f} at {:.2f} {:.2f} {:.2f} aim {:.3f} "
                                    "{:.3f} {:.3f}",
                                    placed[i].tick, moved.back(), turn, placed[i].position.x, placed[i].position.y,
                                    placed[i].position.z, placed[i].direction.x, placed[i].direction.y,
                                    placed[i].direction.z));
            }
            const auto sum = std::accumulate(moved.begin(), moved.end(), 0.0f);
            const auto mean = sum / static_cast<float>(moved.size());
            const auto jumps = std::ranges::count_if(moved, [&](float step) { return step > mean * 3.0f; });
            const auto still = std::ranges::count_if(moved, [](float step) { return step < 1e-5f; });
            const auto byGun = std::ranges::count_if(placed, [](const auto& each) { return each.viewGun; });
            Log(
                std::format("LIGHTSTEPS {} frames {}: moved mean {:.3f} most {:.3f}, jumps {}, still {}, turned most "
                            "{:.2f}, by the gun on screen {}, from {:.1f} {:.1f} {:.1f} to {:.1f} {:.1f} {:.1f}",
                            label, moved.size(), mean, std::ranges::max(moved), jumps, still, turned, byGun,
                            placed.front().position.x, placed.front().position.y, placed.front().position.z,
                            placed.back().position.x, placed.back().position.y, placed.back().position.z));
            if (dump == "expect")
            {
                const auto most = std::ranges::max(moved);
                const auto passed = mean >= leastMean && still <= mostStill && most <= mostMove && turned <= mostTurn;
                Log(
                    std::format("{} light steps {}: mean {:.3f} (at least {}), still {} (at most {}), most {:.3f} (at "
                                "most {}), turned most {:.2f} (at most {})",
                                passed ? "PASS" : "FAIL", label, mean, leastMean, still, mostStill, most, mostMove,
                                turned, mostTurn));
            }
            return true;
        }

        if (action == "state")
        {
            std::optional<LightLogic::State> state;
            LightLogic::Placed placed;
            RunOnMainThread([&] {
                const auto keys = Components::Lights::GetKeys();
                const auto found = keys->find(light.id);
                state = LightLogic::GetState(*actor, found == keys->end() ? LightLogic::Keys{} : found->second,
                                             Components::Playback::GetActorTick(),
                                             Components::Playback::GetTimelineTick());
                const auto keyed =
                    Components::Lights::WithKeyedPlacement(*actor, Components::Playback::GetTimelineTick());
                placed = LightLogic::Place(keyed);
                if (const auto& follow = keyed.light->follow)
                    if (const auto bone = Mod::GetGameInterface()->GetTargetBone(follow->target, follow->bone))
                        placed = LightLogic::Place(keyed, bone->position, bone->axes);
            });
            Log(std::format("LIGHT {} {} at {:.1f} {:.1f} {:.1f} aim {:.2f} {:.2f} {:.2f}: {}", id, actor->name,
                            placed.position.x, placed.position.y, placed.position.z, placed.direction.x,
                            placed.direction.y, placed.direction.z,
                            state ? std::format("on, {:.2f} {:.2f} {:.2f}, radius {:.0f}", state->color.r,
                                                state->color.g, state->color.b, state->radius)
                                  : std::string("off")));
            std::string expect, what;
            if (args >> expect >> what)
            {
                glm::vec3 point{};
                float tolerance = 0.0f;
                if (what == "on" || what == "off")
                {
                    const auto passed = state.has_value() == (what == "on");
                    Log(std::format("{} light state {} {}", passed ? "PASS" : "FAIL", id, what));
                }
                else if (what == "at" && args >> point.x >> point.y >> point.z >> tolerance)
                {
                    const auto distance = glm::length(placed.position - point);
                    Log(std::format("{} light state {} at: {:.1f} away", distance <= tolerance ? "PASS" : "FAIL", id,
                                    distance));
                }
                else if (what == "aim" && args >> point.x >> point.y >> point.z >> tolerance)
                {
                    const auto angle = glm::degrees(std::acos(std::clamp(
                        glm::dot(glm::normalize(placed.direction), glm::normalize(point)), -1.0f, 1.0f)));
                    Log(std::format("{} light state {} aim: {:.1f} degrees off", angle <= tolerance ? "PASS" : "FAIL",
                                    id, angle));
                }
                else if (what == "kind")
                {
                    std::string kind;
                    args >> kind;
                    const auto passed = (light.kind == Types::LightKind::Spot) == (kind == "spot");
                    Log(std::format("{} light state {} kind {}", passed ? "PASS" : "FAIL", id, kind));
                }
                else if (what == "bar")
                {
                    std::int64_t start = 0, end = 0, ticks = 0;
                    args >> start >> end >> ticks;
                    const auto passed = std::abs(light.start - start) <= ticks && std::abs(light.end - end) <= ticks;
                    Log(std::format("{} light state {} bar {} to {}", passed ? "PASS" : "FAIL", id, light.start,
                                    light.end));
                }
                else if (what == "keys" || what == "key")
                {
                    std::size_t count = 0;
                    std::string channelName;
                    std::int64_t tick = 0, ticks = 0, nearest = -1;
                    if (what == "keys")
                        args >> count;
                    else
                        args >> channelName >> tick >> ticks;
                    const auto wanted = channelOf(channelName);
                    std::size_t found = 0;
                    RunOnRenderThread([&] {
                        auto& keyframeManager = Components::KeyframeManager::Get();
                        for (const auto channel : LightLogic::ALL_CHANNELS)
                        {
                            const auto& keys = keyframeManager.GetKeyframes(Components::Lights::GetProperty(light.id, channel));
                            found += keys.size();
                            if (channel != wanted)
                                continue;
                            for (const auto& key : keys)
                                if (nearest < 0 || std::abs(key.tick - tick) < std::abs(nearest - tick))
                                    nearest = key.tick;
                        }
                    });
                    if (what == "keys")
                        Log(std::format("{} light state {} keys: {}", found == count ? "PASS" : "FAIL", id, found));
                    else
                        Log(std::format("{} light state {} key {}: {}",
                                        nearest >= 0 && std::abs(nearest - tick) <= ticks ? "PASS" : "FAIL", id,
                                        channelName, nearest));
                }
                else if (what == "cone" || what == "radius" || what == "highlights" || what == "softness" ||
                         what == "flicker")
                {
                    float value = 0.0f;
                    args >> value >> tolerance;
                    const auto got = what == "flicker"  ? light.flickerAmount
                                     : !state           ? -1.0f
                                     : what == "cone"   ? state->cone
                                     : what == "radius" ? state->radius
                                     : what == "softness" ? state->softness
                                                          : state->highlights;
                    Log(std::format("{} light state {} {}: {:.2f}", std::abs(got - value) <= tolerance ? "PASS" : "FAIL",
                                    id, what, got));
                }
                else if (what == "follows")
                {
                    std::string bone;
                    args >> bone;
                    const auto passed = bone == "off" ? !light.follow : light.follow && light.follow->bone == bone;
                    Log(std::format("{} light state {} follows {}", passed ? "PASS" : "FAIL", id,
                                    light.follow ? light.follow->bone : "nothing"));
                }
                else if (what == "steady")
                {
                    float value = 0.0f;
                    args >> value >> tolerance;
                    const auto got = light.follow ? light.follow->steady : -1.0f;
                    Log(std::format("{} light state {} steady: {:.2f}",
                                    std::abs(got - value) <= tolerance ? "PASS" : "FAIL", id, got));
                }
                else if (what == "firstperson")
                {
                    std::string mode;
                    args >> mode;
                    const auto got = !light.follow                                                   ? "none"
                                     : light.follow->firstPerson == Types::LightFirstPerson::Body    ? "body"
                                     : light.follow->firstPerson == Types::LightFirstPerson::ViewGun ? "viewgun"
                                                                                                     : "auto";
                    Log(std::format("{} light state {} firstperson: {}", mode == got ? "PASS" : "FAIL", id, got));
                }
                else
                    Log("FAIL light state: expect on, off, at, aim, kind, bar, keys, key, a value, follows, steady or "
                        "firstperson");
            }
            return true;
        }

        Log(std::format("FAIL light: no {}", action));
        return true;
    }

    // The sun of the world loaded, as the map has it (GfxWorld.sunParse): what 0x719D40 copies into the dvars, read
    // apart from them as the check of the Look tab's defaults
    Types::Sun ReadWorldSun()
    {
        constexpr std::uintptr_t WORLD = 0x46EC4CC;  // rgp.world
        constexpr std::size_t SUN_PARSE = 0x38;
        const auto world = *reinterpret_cast<const std::uint8_t* const*>(WORLD);
        Types::Sun sun{};
        if (!world)
            return sun;
        const auto parse = world + SUN_PARSE;
        sun.direction = glm::make_vec3(reinterpret_cast<const float*>(parse + 0x4C));
        sun.color = glm::make_vec3(reinterpret_cast<const float*>(parse + 0x68));
        sun.brightness = *reinterpret_cast<const float*>(parse + 0x74);
        return sun;
    }

    // visuals map: logs the sun of the world loaded, the map's values the game gives, and the Look tab's defaults and
    // settings; visuals map expect: PASS when the tab's defaults are the world's sun (and the game's map values);
    // visuals map expect <setting>...: the same, and the tab has those settings at their defaults
    void RunVisualsMapCommand(std::istringstream& args)
    {
        using Settings = Components::VisualConfiguration::Settings;
        namespace VisualsLogic = Components::VisualsLogic;
        auto visualsMenu = UI::UIManager::Get().GetUIComponent<UI::VisualsMenu>(UI::Component::VisualsMenu);
        Types::Sun world{}, game{};
        std::optional<Types::MapVisuals> map;
        Settings defaults, settings;
        RunOnRenderThread([&] {
            world = ReadWorldSun();
            game = Mod::GetGameInterface()->GetSun();
            map = Mod::GetGameInterface()->GetMapVisuals();
            defaults = visualsMenu->GetDefaults();
            settings = visualsMenu->GetSettings();
        });
        const auto describe = [](glm::vec3 color, glm::vec3 direction, float brightness) {
            return std::format("color {:.3f} {:.3f} {:.3f} direction {:.2f} {:.2f} {:.2f} brightness {:.3f}", color.x,
                               color.y, color.z, direction.x, direction.y, direction.z, brightness);
        };
        const auto worldText = describe(world.color, world.direction, world.brightness);
        Log("VISUALSMAP world " + worldText);
        if (map)
            Log("VISUALSMAP reset " + describe(map->sun.color, map->sun.direction, map->sun.brightness));
        Log("VISUALSMAP default " + describe(defaults.sunColor, defaults.sunDirection, defaults.sunBrightness));
        Log("VISUALSMAP tab " + describe(settings.sunColor, settings.sunDirection, settings.sunBrightness));
        Log("VISUALSMAP game " + describe(game.color, game.direction, game.brightness));
        std::string action;
        if (!(args >> action))
            return;

        const auto same = [](glm::vec3 a, glm::vec3 b) {
            return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(1e-3f)));
        };
        const auto isDefaults = [&](glm::vec3 color, glm::vec3 direction, float brightness) {
            return same(color, world.color) && same(direction, world.direction) &&
                   std::abs(brightness - world.brightness) <= 1e-3f;
        };
        std::string differing;
        if (!isDefaults(defaults.sunColor, defaults.sunDirection, defaults.sunBrightness))
            differing += " the tab's defaults";
        if (!map || !isDefaults(map->sun.color, map->sun.direction, map->sun.brightness))
            differing += " the game's map values";
        for (std::string dvar; args >> dvar;)
        {
            const auto setting = VisualsLogic::FindSetting(dvar);
            if (!setting)
            {
                differing += " no setting " + dvar;
                continue;
            }
            std::array<float, 4> value{}, wanted{};
            setting->write(settings, value.data());
            setting->write(defaults, wanted.data());
            for (std::size_t i = 0; i < setting->valueCount; ++i)
            {
                if (std::abs(value[i] - wanted[i]) > 1e-3f)
                {
                    differing += " " + dvar;
                    break;
                }
            }
        }
        Log(std::format("{} visuals defaults are the map's{}", differing.empty() ? "PASS" : "FAIL",
                        differing.empty() ? "" : ":" + differing));
    }

    // visuals section <name|all> open|close: opens or closes a Look section as its band does, saved in config.json;
    // visuals section <name> expect open|closed: PASS when it is. Names as Components::VisualsLogic::GetSectionName
    void RunVisualsSectionCommand(std::istringstream& args)
    {
        namespace VisualsLogic = Components::VisualsLogic;
        std::string which, action, wanted;
        args >> which >> action >> wanted;
        auto visualsMenu = UI::UIManager::Get().GetUIComponent<UI::VisualsMenu>(UI::Component::VisualsMenu);
        std::vector<VisualsLogic::Section> sections;
        for (const auto section : VisualsLogic::SECTIONS)
        {
            if (which == "all" || VisualsLogic::GetSectionName(section) == which)
                sections.push_back(section);
        }
        if (sections.empty())
        {
            Log(std::format("FAIL visuals: no section {}", which));
            return;
        }
        if (action == "expect")
        {
            std::set<VisualsLogic::Section> open;
            RunOnRenderThread(
                [&] { open = VisualsLogic::ReadOpenSections(PreferencesConfiguration::Get().lookSectionsOpen); });
            const auto is = open.contains(sections.front()) ? "open" : "closed";
            Log(std::format("{} visuals section {} is {} (expected {})", wanted == is ? "PASS" : "FAIL", which, is,
                            wanted));
            return;
        }
        RunOnRenderThread([&] {
            for (const auto section : sections)
                visualsMenu->SetSectionOpen(section, action == "open");
        });
        Sleep(100);
        Log(std::format("VISUALS section {} {}", which, action));
    }

    // the Visuals tab's commands (visuals); false for any other
    bool RunVisualsCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "visuals")
            return false;

        // The Visuals tab's settings (UI::VisualsMenu), named as presets name them (Components::VisualsLogic):
        //   visuals: logs each, as the tab has it and as the game does (for those the game is given: not the
        //     HUD's, which free cameras leave as they are)
        //   visuals mark: remembers the tab's
        //   visuals expect same: PASS when the tab's are the ones marked, and the game has them
        //   visuals expect changed <setting>: PASS when the tab's setting isn't as marked
        //   visuals expect <setting> <value>... [tolerance]: PASS when the tab and the game have the setting at
        //     those values (one, three for colors and ranges; a switch is 0 or 1), within the tolerance (0.001)
        //   visuals preset <path|default>: loads a preset file, or the Default preset, as the tab's combo does
        //     (the next time it draws: the tab must be open)
        using Settings = Components::VisualConfiguration::Settings;
        namespace VisualsLogic = Components::VisualsLogic;
        std::string action;
        args >> action;
        auto visualsMenu = UI::UIManager::Get().GetUIComponent<UI::VisualsMenu>(UI::Component::VisualsMenu);
        const auto read = [&] {
            std::pair<Settings, Settings> both;  // the tab's, the game's
            RunOnRenderThread([&] {
                both.first = visualsMenu->GetSettings();
                const auto game = Mod::GetGameInterface();
                const auto sun = game->GetSun();
                both.second = {game->GetDof(),        sun.color,         sun.direction, sun.brightness,
                               game->GetFilmtweaks(), game->GetHudInfo()};
                // (the reach sm_sunSampleSizeNear makes with the shadow map as it is)
                const auto size = game->GetSunShadowMapSize();
                if (const auto dvar = game->GetDvar("sm_sunSampleSizeNear"); dvar && size)
                    both.second.shadowDistance =
                        Components::QualityLogic::GetShadowDistance(dvar->value->floating_point, *size);
                both.second.surfaceDepth = Components::SurfaceDepth::IsOn();
                both.second.surfaceDepthAmount = Components::SurfaceDepth::GetDepth();
                both.second.bumpStrength = Components::SurfaceDepth::GetBumpStrength();
            });
            return both;
        };
        const auto features = Mod::GetGameInterface()->GetSupportedFeatures();
        const auto given = [&](const VisualsLogic::Setting& setting) {
            return setting.property && VisualsLogic::IsUsed(*setting.property, features);
        };
        const auto count = [](const VisualsLogic::Setting& setting) {
            return setting.kind == VisualsLogic::Kind::Values || setting.kind == VisualsLogic::Kind::Color ? 3u : 1u;
        };
        const auto valuesOf = [](const VisualsLogic::Setting& setting, const Settings& settings) {
            std::array<float, 4> values{};
            setting.write(settings, values.data());
            return values;
        };
        const auto describe = [&](const VisualsLogic::Setting& setting, const Settings& settings) {
            const auto values = valuesOf(setting, settings);
            std::string text;
            for (std::size_t i = 0; i < count(setting); ++i)
                text += std::format("{}{:.3f}", i ? " " : "", values[i]);
            return text;
        };
        const auto matches = [&](const VisualsLogic::Setting& setting, const Settings& a, const Settings& b,
                                 float tolerance) {
            const auto first = valuesOf(setting, a);
            const auto second = valuesOf(setting, b);
            for (std::size_t i = 0; i < count(setting); ++i)
            {
                if (std::abs(first[i] - second[i]) > tolerance)
                    return false;
            }
            return true;
        };
        if (action == "mark")
        {
            markedVisuals = read().first;
            Log("VISUALS marked");
            return true;
        }
        if (action == "map")
        {
            RunVisualsMapCommand(args);
            return true;
        }
        if (action == "section")
        {
            RunVisualsSectionCommand(args);
            return true;
        }
        if (action == "preset")
        {
            std::string path;
            std::getline(args >> std::ws, path);
            RunOnRenderThread([&] {
                visualsMenu->RequestPreset(path == "default" ? std::nullopt
                                                             : std::optional<std::filesystem::path>(path));
            });
            Sleep(300);
            Log("VISUALS preset " + path);
            return true;
        }
        const auto [tab, game] = read();
        if (action == "expect")
        {
            std::string what;
            args >> what;
            if (what == "same")
            {
                if (!markedVisuals)
                {
                    Log("FAIL visuals: nothing marked");
                    return true;
                }
                std::string differing;
                for (const auto& setting : VisualsLogic::GetSettings())
                {
                    if (!matches(setting, *markedVisuals, tab, 1e-4f))
                        differing += std::format(" {} ({} marked {})", setting.dvar, describe(setting, tab),
                                                 describe(setting, *markedVisuals));
                    else if (given(setting) && !matches(setting, tab, game, 1e-3f))
                        differing += std::format(" {} (game {} tab {})", setting.dvar, describe(setting, game),
                                                 describe(setting, tab));
                }
                Log(std::format("{} visuals as marked{}", differing.empty() ? "PASS" : "FAIL",
                                differing.empty() ? "" : ":" + differing));
                return true;
            }
            if (what == "changed")
            {
                std::string dvar;
                args >> dvar;
                const auto setting = VisualsLogic::FindSetting(dvar);
                if (!setting || !markedVisuals)
                {
                    Log(std::format("FAIL visuals: no setting {} or nothing marked", dvar));
                    return true;
                }
                Log(std::format("{} visuals {} is {} (marked {})",
                                matches(*setting, *markedVisuals, tab, 1e-4f) ? "FAIL" : "PASS", dvar,
                                describe(*setting, tab), describe(*setting, *markedVisuals)));
                return true;
            }
            const auto setting = VisualsLogic::FindSetting(what);
            if (!setting)
            {
                Log(std::format("FAIL visuals: no setting {}", what));
                return true;
            }
            std::vector<float> expected;
            for (float each; args >> each;)
                expected.push_back(each);
            float tolerance = 1e-3f;
            if (expected.size() > count(*setting))
            {
                tolerance = expected.back();
                expected.pop_back();
            }
            if (expected.size() != count(*setting))
            {
                Log(std::format("FAIL visuals: {} has {} values", what, count(*setting)));
                return true;
            }
            const auto inTab = valuesOf(*setting, tab);
            const auto inGame = valuesOf(*setting, game);
            bool passed = true;
            for (std::size_t i = 0; i < expected.size(); ++i)
            {
                passed = passed && std::abs(inTab[i] - expected[i]) <= tolerance &&
                         (!given(*setting) || std::abs(inGame[i] - expected[i]) <= tolerance);
            }
            std::string wanted;
            for (const auto each : expected)
                wanted += std::format(" {:.3f}", each);
            Log(std::format("{} visuals {} is {} (game {}; expected{} +- {})", passed ? "PASS" : "FAIL", what,
                            describe(*setting, tab), given(*setting) ? describe(*setting, game) : "-", wanted,
                            tolerance));
            return true;
        }
        for (const auto& setting : VisualsLogic::GetSettings())
            Log(std::format("VISUALS {} tab {} game {}", setting.dvar, describe(setting, tab),
                            given(setting) ? describe(setting, game) : "-"));
        return true;
    }

    // actor face ..., actor faceclip ...: an actor's face (ActorLogic::GetFace)
    void RunFaceCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "faceclip")
        {
            // actor faceclip <actor> <animation> <start> <length> [<blend in> [<loop 0|1>]]: a clip on its Face row
            std::string which, animation;
            std::uint32_t start = 0, length = 3000, blendIn = ActorLogic::FACE_BLEND;
            int loop = 1;
            args >> which >> animation >> start >> length;
            if (!(args >> blendIn))
                blendIn = ActorLogic::FACE_BLEND;
            if (!(args >> loop))
                loop = 1;
            const auto id = ParseActorId(which);
            const auto clip = Components::Actors::AddFaceClip(
                id, {.animation = animation, .start = start, .length = length, .loop = loop != 0, .blendIn = blendIn});
            Log(std::format("ACTOR face clip {} added to {}", clip, id));
            return;
        }
        // actor face <actor> <off|auto>: whether its face moves on its own; actor face <actor> expect <off|auto> |
        // clips <n> | none | <animation> [<weight> [<tolerance>]]: PASS when it is so, has that many face clips, has no
        // face entry weighed, or has that animation's weighed (that much, within 0.02); logs what the face plays at the
        // timeline's tick (core's samples) and the weights its tree's face entries were given last
        std::string which, mode, wanted;
        float weight = -1.0f, tolerance = 0.02f;
        args >> which >> mode >> wanted;
        if (!(args >> weight))
            weight = -1.0f;
        if (!(args >> tolerance))
            tolerance = 0.02f;
        auto actor = Components::Actors::GetActor(ParseActorId(which));
        if (!actor)
        {
            Log("FAIL actor face: no such actor");
            return;
        }
        if (mode == "off" || mode == "auto")
        {
            actor->face = mode == "auto" ? Types::ActorFace::Auto : Types::ActorFace::Off;
            Components::Actors::Update(*actor);
            Log(std::format("ACTOR face of {} {}", actor->id, mode));
            return;
        }
        const std::string is = actor->face == Types::ActorFace::Auto ? "auto" : "off";
        if (mode == "expect" && (wanted == "off" || wanted == "auto"))
        {
            Log(std::format("{} actor {} face {} (expected {})", is == wanted ? "PASS" : "FAIL", actor->id, is,
                            wanted));
            return;
        }
        if (mode == "expect" && wanted == "clips")
        {
            const auto count = std::to_string(actor->faceClips.size());
            const auto expected = std::to_string(static_cast<int>(weight));
            Log(std::format("{} actor {} has {} face clips (expected {})", count == expected ? "PASS" : "FAIL",
                            actor->id, count, expected));
            return;
        }
        const auto id = actor->id;
        std::optional<Actors::TreeState> state;
        RunOnMainThread([&] { state = Actors::GetTreeState(id); });
        const auto tick = Components::Playback::GetTimelineTick();
        const auto layers = Components::Actors::GetLayers(*actor, tick, state ? state->body : ActorLogic::Body{});
        std::string samples, entries;
        for (const auto& sample : layers.face)
            samples += std::format(" {}{} at {:.3f} s x{:.3f}", sample.clip ? "clip " : "", sample.animation,
                                   sample.time, sample.weight);
        float found = 0.0f, total = 0.0f;
        for (const auto& [entry, entryWeight] : state ? state->faceWeights : decltype(state->faceWeights){})
        {
            entries += std::format(" {}={:.4f}", entry, entryWeight);
            total += entryWeight;
            if (entry == wanted)
                found += entryWeight;
        }
        const auto text = std::format("actor {} tick {} face {}: plays{} (tree{}{})", id, tick, is,
                                      samples.empty() ? " nothing" : samples, entries.empty() ? " none" : entries,
                                      state ? "" : ", not built");
        if (mode != "expect")
            Log("FACE " + text);
        else if (wanted == "none")
            Log(std::format("{} {} (expected none)", state && total <= 0.0f ? "PASS" : "FAIL", text));
        else
            Log(std::format(
                "{} {} (expected {}{})",
                state && (weight < 0.0f ? found >= 0.01f : std::abs(found - weight) <= tolerance) ? "PASS" : "FAIL",
                text, wanted, weight < 0.0f ? std::string() : std::format(" x{:.3f}", weight)));
    }

    // commands for how actors are animated (see RunCharacterZoneCommand for why they are
    // apart)
    bool RunAnimationCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "actor")
        {
            // (actor face and actor faceclip; the other actor commands are Run's)
            const auto position = args.tellg();
            std::string what;
            args >> what;
            if (what == "face" || what == "faceclip")
            {
                RunFaceCommand(what, args);
                return true;
            }
            if (what == "facepaint")
            {
                // actor facepaint <actor> <number>: its face paint (0 for none); actor facepaint <actor> expect <number>:
                // PASS when it has that one
                std::string which, expect;
                std::int32_t paint = 0;
                args >> which >> expect;
                auto actor = Components::Actors::GetActor(ParseActorId(which));
                if (!actor)
                {
                    Log("ACTOR no such actor " + which);
                    return true;
                }
                if (expect == "expect")
                {
                    args >> paint;
                    Log(std::format("{} actor {} face paint {} (expected {})",
                                    actor->facePaint == paint ? "PASS" : "FAIL", actor->id, actor->facePaint, paint));
                    return true;
                }
                paint = std::stoi(expect);
                actor->facePaint = paint;
                Components::Actors::Update(*actor);
                Log(std::format("ACTOR face paint {} on {}", paint, actor->id));
                return true;
            }
            args.clear();
            args.seekg(position);
            return false;
        }
        if (name == "players" && args.peek() != EOF)
        {
            // players expect <entity> <model>...: PASS when a player's or corpse's DObj has those models, in that order
            std::string what, model;
            std::int32_t entity = 0;
            args >> what >> entity;
            if (what == "expect")
            {
                std::vector<std::string> wanted;
                while (args >> model)
                    wanted.push_back(model);
                std::string drawn;
                bool found = false;
                RunOnMainThread([&] {
                    const auto obj = Functions::Com_GetClientDObj(entity, 0);
                    std::size_t next = 0;
                    for (int m = 0; obj && m < obj->numModels; ++m)
                    {
                        drawn += std::string(obj->models[m]->name) + " ";
                        if (next < wanted.size() && wanted[next] == obj->models[m]->name)
                            ++next;
                    }
                    found = next == wanted.size();
                });
                Log(std::format("{} players {} has {}", found ? "PASS" : "FAIL", entity, drawn));
                return true;
            }
        }
        if (name == "corpses" && args.peek() != EOF)
        {
            // corpses expect <client> <animation|model>...: PASS when its corpse plays that animation of the catalog
            // (or the game's own: original) and has those models
            std::string what, animation, model;
            std::int32_t client = 0;
            args >> what >> client >> animation;
            if (what == "expect")
            {
                std::vector<std::string> wanted;
                while (args >> model)
                    wanted.push_back(model);
                bool found = false;
                std::string state = "no corpse";
                RunOnMainThread([&] {
                    const auto entities = Structures::GetEntities();
                    for (std::size_t i = 0; entities && i < Structures::MAX_GENTITIES; ++i)
                    {
                        const auto& es = entities[i].nextState;
                        if (es.eType != Structures::ET_PLAYER_CORPSE || es.clientNum != client)
                            continue;
                        const auto played = Hooks::PlayerAnimation::GetCatalogDeath(static_cast<std::int32_t>(i));
                        const auto obj = Functions::Com_GetClientDObj(static_cast<std::int32_t>(i), 0);
                        std::string drawn;
                        std::size_t next = 0;
                        for (int m = 0; obj && m < obj->numModels; ++m)
                        {
                            drawn += std::string(obj->models[m]->name) + " ";
                            if (next < wanted.size() && wanted[next] == obj->models[m]->name)
                                ++next;
                        }
                        state = std::format("{} plays {} of {}", i, played.empty() ? "original" : played, drawn);
                        found = next == wanted.size() && (played.empty() ? "original" : played) == animation;
                    }
                });
                Log(std::format("{} corpses client {}: {}", found ? "PASS" : "FAIL", client, state));
                return true;
            }
        }
        if (name == "facepaints")
        {
            // facepaints: logs the game's table of face paints (mp/weaponoptions.csv's patterns, by number)
            RunOnMainThread([&] {
                const auto table = reinterpret_cast<const Structures::Material* const*>(0xD5BFE8 + 0x1880);
                for (std::size_t i = 0; i < 64; ++i)
                    if (table[i])
                        Log(std::format("FACEPAINT {} {}", i, table[i]->name));
            });
            return true;
        }
        if (name == "built")
        {
            // built <actor> [<model>[+<model>...]]: logs the models the actor's DObj is made of (a spine rig first,
            // see Actors::GetSpineRig); with models, PASS when exactly those, in that order
            std::string which, expected;
            args >> which >> expected;
            const auto id = ParseActorId(which);
            std::vector<std::string> models;
            RunOnMainThread([&] { models = Actors::GetBuiltModels(id); });
            std::string text;
            for (const auto& model : models)
                text += (text.empty() ? "" : "+") + model;
            if (expected.empty())
                Log(std::format("BUILT actor {} of {}", id, text.empty() ? "nothing" : text));
            else
                Log(std::format("{} actor {} built of {} (expected {})", text == expected ? "PASS" : "FAIL", id,
                                text.empty() ? "nothing" : text, expected));
            return true;
        }
        if (name == "spinecheck")
        {
            // spinecheck <actor|all>: PASS for an actor whose torso the game draws upright, as far as its tree goes
            // (see Actors::GetSpineState, SpineRig in Actors.cpp): no animation made for MP bodies weighed on a body
            // without a torso_stabilizer (an SP body without its spine rig turns its torso by about 90 degrees), and
            // where animations made for SP bodies weigh on a stabilizer (an MP body's or the rig's), the made up
            // entries lining it up, resting it or aiming it weigh too (or the torso bends forward by about 90 degrees)
            std::string which;
            args >> which;
            std::vector<std::uint32_t> ids;
            if (which == "all")
                for (const auto& actor : Components::Actors::GetActors())
                    ids.push_back(actor.id);
            else
                ids.push_back(ParseActorId(which));
            if (ids.empty())
                Log("FAIL spinecheck: no actors");
            constexpr float NONE = 1e-3f;
            for (const auto id : ids)
            {
                std::optional<Actors::SpineState> state;
                RunOnMainThread([&] { state = Actors::GetSpineState(id); });
                if (!state)
                {
                    Log(std::format("FAIL spinecheck {}: the actor isn't built", id));
                    continue;
                }
                const auto spine = state->lined
                                       ? std::format("lined {:.3f} resting {:.3f} aimed {:.3f}", *state->lined,
                                                     state->resting.value_or(0.0f), state->aimed.value_or(0.0f))
                                       : std::string("no spine entries");
                const auto text = std::format("actor {} ({}): MP animations {:.3f}, SP animations {:.3f}, {}", id,
                                              state->rig          ? "spine rig"
                                              : state->stabilizer ? "MP body"
                                                                  : "SP body",
                                              state->mpWeight, state->spWeight, spine);
                if (!state->stabilizer && state->mpWeight > NONE)
                    Log("FAIL spinecheck " + text + ": the torso is turned (no torso_stabilizer under the MP ones)");
                else if (state->stabilizer && state->spWeight > NONE &&
                         state->lined.value_or(0.0f) + state->resting.value_or(0.0f) + state->aimed.value_or(0.0f) <=
                             NONE)
                    Log("FAIL spinecheck " + text +
                        ": the torso is bent (the stabilizer isn't lined up for the SP ones)");
                else
                    Log("PASS spinecheck " + text);
            }
            return true;
        }
        if (name == "animkind")
        {
            // animkind <animation> [<kind> <family>]: logs what the catalog says an animation moves and the rig it was
            // made for (see PoseLogic::ClassifyAnimation); with both, PASS when it says so
            std::string animation, kind, family;
            args >> animation >> kind >> family;
            const auto catalog = Components::Actors::GetCatalog();
            const auto entry = catalog ? catalog->FindAnimation(animation) : nullptr;
            if (!entry)
            {
                Log("FAIL animkind: " + animation + " isn't in the catalog");
                return true;
            }
            constexpr std::array KINDS = {"FullBody", "Torso", "Face", "Other"};
            constexpr std::array FAMILIES = {"MpPlayer", "SpHuman", "Human", "Dog", "Viewmodel", "Other"};
            const std::string isKind = KINDS[static_cast<std::size_t>(entry->kind)];
            const std::string isFamily = FAMILIES[static_cast<std::size_t>(entry->family)];
            if (kind.empty())
                Log(std::format("ANIMKIND {} {} {}", animation, isKind, isFamily));
            else
                Log(std::format("{} animkind {} is {} {} (expected {} {})",
                                isKind == kind && isFamily == family ? "PASS" : "FAIL", animation, isKind, isFamily,
                                kind, family));
            return true;
        }
        if (name == "after")
        {
            // after <actor> <clip index> [hold|resume | expect <hold|resume|next>]: logs what the actor does once the
            // clip ends (see ActorLogic::GetClipAfter); hold or resume has it do that (ActorLogic::SetClipAfter, to the
            // demo's end); expect: PASS when it does that
            constexpr std::array AFTERS = {"hold", "resume", "next"};
            std::string which, mode, expected;
            std::size_t index = 0;
            args >> which >> index >> mode >> expected;
            auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || index >= actor->clips.size())
            {
                Log("FAIL after: no such actor or clip");
                return true;
            }
            const auto clip = actor->clips[index].id;
            if (mode == "hold" || mode == "resume")
            {
                const bool changed = ActorLogic::SetClipAfter(
                    *actor, clip, mode == "hold" ? ActorLogic::ClipAfter::Hold : ActorLogic::ClipAfter::Resume,
                    Mod::GetGameInterface()->GetDemoInfo().endTick);
                Components::Actors::Update(*actor);
                Log(std::format("AFTER actor {} clip {} {}{}", actor->id, index, mode, changed ? "" : " (unchanged)"));
                return true;
            }
            const std::string is = AFTERS[static_cast<std::size_t>(ActorLogic::GetClipAfter(*actor, clip))];
            if (mode != "expect")
                Log(std::format("AFTER actor {} clip {} {}", actor->id, index, is));
            else
                Log(std::format("{} after actor {} clip {} {} (expected {})", is == expected ? "PASS" : "FAIL",
                                actor->id, index, is, expected));
            return true;
        }
        constexpr std::array GUN_CLASSES = {"rifle",    "pistol",         "rocketlauncher",
                                            "dualwield", "ballisticknife", "minigun"};
        constexpr std::array STANCES = {"auto", "stand", "crouch", "prone"};
        if (name == "stance")
        {
            // stance <actor> <auto|stand|crouch|prone>: how the actor's legs stand under its upper body clips
            std::string which, stance;
            args >> which >> stance;
            auto actor = Components::Actors::GetActor(ParseActorId(which));
            const auto known = std::ranges::find(STANCES, stance);
            if (!actor || known == STANCES.end())
            {
                Log("FAIL stance: no such actor or stance");
                return true;
            }
            actor->stance = static_cast<Types::ActorStance>(known - STANCES.begin());
            Components::Actors::Update(*actor);
            Log(std::format("STANCE actor {} {}", actor->id, stance));
            return true;
        }
        if (name == "locomotion")
        {
            // locomotion <actor> <off|auto> [<speed>]: whether the actor walks and runs as the game's players (see
            // ActorLogic::GetLayers), and how fast its walking clips walk its path, units a second; locomotion <actor>
            // expect <off|auto> [<speed>]: PASS when it does (at that speed, within 0.5; with gaits, its first
            // stretch's)
            std::string which, mode;
            float speed = -1.0f;
            args >> which >> mode;
            const bool expect = mode == "expect";
            if (expect)
                args >> mode;
            args >> speed;
            auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || (mode != "off" && mode != "auto"))
            {
                Log("FAIL locomotion: no such actor or mode");
                return true;
            }
            if (expect)
            {
                const auto is = actor->locomotion == Types::ActorLocomotion::Auto ? "auto" : "off";
                // (with gaits, its first stretch's: as the game measured its animation, or guessed until it has, so
                // within 1%)
                const auto tolerance = actor->stretches.empty() ? 0.5f : std::max(0.5f, speed * 0.01f);
                const auto pathSpeed = actor->stretches.empty()
                                           ? actor->pathSpeed
                                           : ActorLogic::GetStretchSpeed(*actor, actor->stretches.front(),
                                                                         Components::Actors::GetAnimationLookup());
                Log(std::format(
                    "{} locomotion actor {} {} at {:.1f} units/s, stance {} (expected {}{})",
                    is == mode && (speed < 0.0f || std::abs(pathSpeed - speed) <= tolerance) ? "PASS" : "FAIL",
                    actor->id, is, pathSpeed, STANCES[static_cast<std::size_t>(actor->stance)], mode,
                    speed < 0.0f ? std::string() : std::format(" at {:.1f}", speed)));
                return true;
            }
            actor->locomotion = mode == "auto" ? Types::ActorLocomotion::Auto : Types::ActorLocomotion::Off;
            if (speed >= 0.0f)
                actor->pathSpeed = speed;
            Components::Actors::Update(*actor);
            Log(std::format("LOCOMOTION actor {} {} at {:.1f} units/s", actor->id, mode, actor->pathSpeed));
            return true;
        }
        if (name == "gunclass")
        {
            // gunclass <actor> [<class>]: logs the class of the actor's gun as its legs stand with it (rifle, pistol,
            // rocketlauncher, dualwield, ballisticknife) and the weapon def's fields it comes from; with a class, PASS
            // when it is that
            std::string which, expected;
            args >> which >> expected;
            const auto id = ParseActorId(which);
            std::optional<Actors::TreeState> state;
            RunOnMainThread([&] { state = Actors::GetTreeState(id); });
            if (!state)
            {
                Log(std::format("FAIL gunclass: actor {} isn't built", id));
                return true;
            }
            const std::string gunClass = GUN_CLASSES[static_cast<std::size_t>(state->gunClass)];
            const auto text = std::format("actor {} gun class {} (player anim type {}, weapon class {})", id, gunClass,
                                          state->playerAnimType, state->weapClass);
            if (expected.empty())
                Log("GUNCLASS " + text);
            else
                Log(std::format("{} {} (expected {})", gunClass == expected ? "PASS" : "FAIL", text, expected));
            return true;
        }
        if (name == "legs")
        {
            // legs <actor> [expect none | expect <animation> [<share> [<tolerance>]] [rate <rate> [<tolerance>]]],
            // legs <actor|all> <off|on>: logs what the actor's legs play under its upper body clips or as its
            // locomotion at the timeline's tick (see ActorLogic::GetLayers: each animation, whether it is the full
            // body clip they go on with, its time, its share of the legs and its rate) and the weight its tree gave
            // each entry last; off leaves them out (for posecheck), on puts them back; expect none: PASS without legs;
            // expect <animation>: PASS when the legs play it, with that share (within 0.02) and rate (within 0.02)
            std::string which, mode, animation;
            float share = -1.0f, tolerance = 0.02f, rate = -1.0f, rateTolerance = 0.02f;
            args >> which >> mode >> animation;
            for (std::string word; args >> word;)
            {
                if (word == "rate")
                {
                    args >> rate;
                    if (!(args >> rateTolerance))
                        rateTolerance = 0.02f;
                    break;
                }
                if (share < 0.0f)
                    share = std::stof(word);
                else
                    tolerance = std::stof(word);
            }
            if (mode == "off" || mode == "on")
            {
                // (of all actors with all)
                std::vector<std::uint32_t> ids;
                if (which == "all")
                    for (const auto& actor : Components::Actors::GetActors())
                        ids.push_back(actor.id);
                else
                    ids.push_back(ParseActorId(which));
                for (const auto id : ids)
                {
                    RunOnMainThread([&] { Actors::SetLegsLeftOut(id, mode == "off"); });
                    Log(std::format("LEGS actor {} {}", id, mode == "off" ? "left out" : "back"));
                }
                return true;
            }
            const auto id = ParseActorId(which);
            const auto actor = Components::Actors::GetActor(id);
            std::optional<Actors::TreeState> state;
            RunOnMainThread([&] { state = Actors::GetTreeState(id); });
            if (!actor || !state)
            {
                Log(std::format("FAIL legs: actor {} isn't built", id));
                return true;
            }
            const auto tick = Components::Playback::GetTimelineTick();
            const auto layers = Components::Actors::GetLayers(*actor, tick, state->body);
            std::string legs, entries, library;
            float found = 0.0f, foundRate = 0.0f;
            bool plays = false;
            for (const auto& sample : layers.legs)
            {
                const auto& played = sample.clip ? sample.clip->animation : sample.animation;
                legs += std::format(" {}{} at {:.3f} s x{:.3f} rate {:.3f}", sample.clip ? "clip " : "", played,
                                    sample.time, sample.weight, sample.rate);
                if (played == animation)
                {
                    plays = true;
                    found += sample.weight;
                    foundRate = sample.rate;
                }
            }
            for (const auto& [entry, weight] : state->weights)
                entries += std::format(" {}={:.4f}", entry, weight);
            for (const auto& idle : state->legs)
                library += " " + idle;
            const auto text = std::format("actor {} tick {} stance {} gun {}{}: legs{} (tree{}; library{})", id, tick,
                                          STANCES[static_cast<std::size_t>(actor->stance)],
                                          GUN_CLASSES[static_cast<std::size_t>(state->gunClass)],
                                          state->legsLeftOut ? " (left out)" : "", legs.empty() ? " none" : legs,
                                          entries, library.empty() ? " none" : library);
            if (mode != "expect")
                Log("LEGS " + text);
            else if (animation == "none")
                Log(std::format("{} {} (expected none)", layers.legs.empty() ? "PASS" : "FAIL", text));
            else
                Log(std::format("{} {} (expected {}{}{})",
                                plays && (share < 0.0f || std::abs(found - share) <= tolerance) &&
                                        (rate < 0.0f || std::abs(foundRate - rate) <= rateTolerance)
                                    ? "PASS"
                                    : "FAIL",
                                text, animation, share < 0.0f ? std::string() : std::format(" x{:.3f}", share),
                                rate < 0.0f ? std::string() : std::format(" rate {:.3f}", rate)));
            return true;
        }
        return false;
    }

    // animations ...: the catalog of every animation the game has (Components::Actors::
    // GetAnimationCatalog). Names are followed by a colon in what is logged, as send.ps1 takes a screenshot for "shot "
    std::string DescribeEntry(const Types::AnimationCatalog& animations, const Types::AnimationEntry& entry)
    {
        constexpr std::array KINDS = {"FullBody", "Torso", "Face", "Other"};
        constexpr std::array FAMILIES = {"MpPlayer", "SpHuman", "Human", "Dog", "Viewmodel", "Other"};
        std::string zones;
        for (const auto source : entry.sources)
            zones += (zones.empty() ? "" : "+") + animations.sources[source].zone;
        std::string notetracks;
        for (const auto& notetrack : entry.notetracks)
            notetracks += std::format("{}{}@{:.3f}", notetracks.empty() ? "" : ",", notetrack.name, notetrack.time);
        const auto source = animations.GetSource(entry);
        return std::format(
            "{}: from {} ({}) frames {} at {:.0f} ({:.3f} s) {}{} {} {} bones {} (set {}) notetracks {} [{}] motion "
            "{:.2f},{:.2f},{:.2f} turn {:.1f} states {} > {} {}{} category {} bytes {} hash {}",
            entry.name, zones, source ? AnimationCatalogLogic::GetHeading(*source) : "-", entry.frames, entry.rate,
            entry.Duration(), entry.loop ? "loop" : "once", entry.delta ? " delta" : "",
            KINDS[static_cast<std::size_t>(entry.kind)], FAMILIES[static_cast<std::size_t>(entry.family)],
            entry.boneSet < animations.boneSets.size() ? animations.boneSets[entry.boneSet].size() : 0, entry.boneSet,
            entry.notetracks.size(), notetracks, entry.motion.x, entry.motion.y, entry.motion.z, entry.turn,
            AnimationCatalogLogic::DescribeState(entry.from), AnimationCatalogLogic::DescribeState(entry.to),
            entry.death ? "death" : "-", entry.alert ? " alert" : "",
            AnimationCatalogLogic::GetCategory(entry.name, entry.kind, &entry), entry.bytes, entry.hash);
    }

    // how many of the catalog's entries fit, by family
    std::string CountFits(const Types::AnimationCatalog& animations, const std::vector<bool>& fits, std::size_t& total)
    {
        constexpr std::array FAMILIES = {"MpPlayer", "SpHuman", "Human", "Dog", "Viewmodel", "Other"};
        std::array<std::size_t, FAMILIES.size()> counts{};
        std::size_t faces = 0;
        total = 0;
        for (std::size_t i = 0; i < fits.size() && i < animations.entries.size(); ++i)
        {
            if (!fits[i])
                continue;
            ++total;
            const auto& entry = animations.entries[i];
            ++counts[static_cast<std::size_t>(entry.family)];
            faces += entry.kind == Types::AnimationKind::Face ? 1 : 0;
        }
        std::string text;
        for (std::size_t f = 0; f < FAMILIES.size(); ++f)
            if (counts[f])
                text += std::format(" {} {}", FAMILIES[f], counts[f]);
        return text + std::format(" (faces {})", faces);
    }

    bool RunCatalogCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "animations")
            return false;
        const auto& animations = Components::Actors::GetAnimationCatalog();
        std::string what;
        args >> what;
        if (what == "count")
        {
            // animations count [expect <n>]: how many the browser lists (the catalog's and the game's own it doesn't
            // have), how many of them the game has loaded, of the catalog's sources; PASS when it lists n
            std::string expect;
            std::size_t expected = 0;
            args >> expect >> expected;
            const auto list = Components::Actors::GetAnimationList();
            if (!list)
            {
                Log("FAIL animations count: the game has no catalog yet");
                return true;
            }
            std::size_t loaded = 0, extra = 0, twins = 0, stored = 0;
            for (const auto& animation : list->animations)
            {
                loaded += animation.loaded && !animation.loaded->stored ? 1 : 0;
                stored += animation.loaded && animation.loaded->stored ? 1 : 0;
                extra += animation.entry ? 0 : 1;
                twins += animation.entry && animation.entry->AssetName() != animation.entry->name ? 1 : 0;
            }
            const auto text = std::format(
                "the browser lists {} ({} of the catalog's {}, {} copies that differ, {} the "
                "game has that it doesn't), {} loaded, {} from the animation pack, from {} "
                "places; {} bone sets",
                list->animations.size(), list->animations.size() - extra, animations.entries.size(), twins, extra,
                loaded, stored, list->headings.size(), animations.boneSets.size());
            if (expect != "expect")
                Log("ANIMATIONS " + text);
            else
                Log(std::format("{} animations count: {} (expected {})",
                                list->animations.size() == expected ? "PASS" : "FAIL", text, expected));
            return true;
        }
        if (what == "fit" || what == "fitactor")
        {
            // animations fit <model>[+<model>...] [expect <n>] | animations fitactor <actor> [expect <n>]: how many of
            // the catalog's animations fit those models (AnimationCatalogLogic::Fits, with the bones the game has for
            // them), by family; PASS when n do
            std::string which, expect;
            std::size_t expected = 0;
            args >> which >> expect >> expected;
            std::vector<std::string> models;
            if (what == "fitactor")
            {
                const auto actor = Components::Actors::GetActor(ParseActorId(which));
                if (actor)
                    models = actor->models;
            }
            else
            {
                std::size_t start = 0;
                while (start <= which.size())
                {
                    const auto end = std::min(which.find('+', start), which.size());
                    if (end > start)
                        models.push_back(which.substr(start, end - start));
                    start = end + 1;
                }
            }
            const auto fits = models.empty() ? nullptr : Components::Actors::GetFits(models);
            if (!fits)
            {
                Log(std::format("FAIL animations {} {}: the game hasn't loaded {}", what, which,
                                models.empty() ? "an actor of that" : "all of those models"));
                return true;
            }
            std::size_t total = 0;
            const auto families = CountFits(animations, *fits, total);
            const auto text = std::format("{} of {} fit {}:{}", total, animations.entries.size(), which, families);
            if (expect != "expect")
                Log("ANIMATIONS " + text);
            else
                Log(std::format("{} animations {} (expected {})", total == expected ? "PASS" : "FAIL", text, expected));
            return true;
        }
        if (what == "check")
        {
            // animations check: every animation the game has loaded against the catalog: listed, of the same kind,
            // family, frames (its duration), loop and notetracks; PASS when all are
            const auto catalog = Components::Actors::GetCatalog();
            if (!catalog)
            {
                Log("FAIL animations check: the game has no catalog yet");
                return true;
            }
            std::size_t checked = 0, missing = 0, differ = 0;
            std::vector<std::string> problems;
            for (const auto& animation : catalog->animations)
            {
                // (the first person arms' aren't listed; those of the animation pack are the catalog's; the killstreaks'
                // motions are no animations)
                if (!AnimationCatalogLogic::IsListed(animation.name, animation.family) || animation.stored ||
                    Components::KillstreakLogic::IsMotion(animation.name))
                    continue;
                ++checked;
                const auto entry = animation.entry;
                if (!entry)
                {
                    ++missing;
                    if (problems.size() < 20)
                        problems.push_back(animation.name + ": not in the catalog");
                    continue;
                }
                std::string why;
                if (entry->kind != animation.kind || entry->family != animation.family)
                    why += " kind/family";
                if (std::abs(entry->Duration() - animation.duration) > 1e-3f)
                    why += std::format(" duration {:.3f}/{:.3f}", entry->Duration(), animation.duration);
                if (entry->loop != animation.loop)
                    why += " loop";
                const bool sameNotetracks = std::ranges::equal(
                    entry->notetracks, animation.notetracks,
                    [](const Types::ActorNotetrack& a, const Types::ActorNotetrack& b) {
                        return a.name == b.name && std::abs(a.time - b.time) <= 1e-3f;
                    });
                if (!sameNotetracks)
                    why += std::format(" notetracks {}/{}", entry->notetracks.size(), animation.notetracks.size());
                if (!why.empty())
                {
                    ++differ;
                    if (problems.size() < 20)
                        problems.push_back(animation.name + ":" + why);
                }
            }
            for (const auto& problem : problems)
                Log("ANIMATIONS differs " + problem);
            // and each listed once (the game's own or the animation pack's)
            std::size_t twice = 0, stored = 0;
            for (std::size_t i = 0; i < catalog->animations.size(); ++i)
            {
                stored += catalog->animations[i].stored ? 1 : 0;
                if (i > 0 && catalog->animations[i].name == catalog->animations[i - 1].name)
                {
                    ++twice;
                    Log("ANIMATIONS twice " + catalog->animations[i].name);
                }
            }
            Log(std::format(
                "{} animations check: {} loaded (but first person ones), {} not in the catalog, {} differ "
                "from it, {} listed twice; {} of the animation pack",
                missing == 0 && differ == 0 && twice == 0 ? "PASS" : "FAIL", checked, missing, differ, twice, stored));
            return true;
        }
        // animations <name>: what the catalog says of an animation, and whether the game has it loaded (a copy that
        // differs is named with its zone: animations pb_stand_alert_pistol (common_zombie))
        std::string rest;
        std::getline(args, rest);
        if (const auto start = rest.find_first_not_of(' '); start != std::string::npos)
            what += " " + rest.substr(start, rest.find_last_not_of(' ') - start + 1);
        const auto entry = animations.Find(what);
        const auto catalog = Components::Actors::GetCatalog();
        const auto found = catalog ? catalog->FindAnimation(what) : nullptr;
        const bool loaded = found && !found->stored;
        if (!entry)
            Log(std::format("FAIL animations {}: not in the catalog{}", what,
                            loaded ? ", though the game has it loaded" : ""));
        else
            Log(std::format("ANIMATION {} {}", DescribeEntry(animations, *entry),
                            loaded                                    ? "loaded"
                            : !found                                  ? "not-loaded"
                            : Components::Actors::IsStoreLoaded(what) ? "in-pack loaded"
                                                                      : "in-pack not-loaded"));
        return true;
    }

    // commands for what actors are and the animations that fit them (F3's filter,
    // dogs; see RunCharacterZoneCommand for why they are apart)
    bool RunKindCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "kind")
        {
            // kind <actor> [expect <human|dog> [shepherd|hellhound]]: what the actor is (Components::Actors::
            // GetActorKind, GetBody) and what it starts and dies with, its legs' and face's libraries; PASS when it is
            std::string which, expect, kind, breed;
            args >> which >> expect >> kind >> breed;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor)
            {
                Log(std::format("FAIL kind {}: no such actor", which));
                return true;
            }
            const auto body = Components::Actors::GetBody(*actor);
            const std::string isKind = body.kind == Types::ActorKind::Dog       ? "dog"
                                       : body.kind == Types::ActorKind::Vehicle ? "vehicle"
                                                                                : "human";
            const std::string isBreed = body.breed == ActorLogic::DogBreed::Hellhound ? "hellhound" : "shepherd";
            const auto start = Components::Actors::GetStartAnimations(body);
            const auto join = [](const std::vector<std::string>& names) {
                std::string text;
                for (const auto& each : names)
                    text += (text.empty() ? "" : ",") + each;
                return text.empty() ? std::string("-") : text;
            };
            const auto text = std::format(
                "actor {} is a {}{} (character {}, models {}): starts with idle {} walk {} run {} at {}/{} u/s, "
                "dies with {}, its death {}; legs {}; face {}",
                actor->id, isKind, body.kind == Types::ActorKind::Dog ? " (" + isBreed + ")" : "",
                actor->character.empty() ? "-" : actor->character, join(actor->models), start.idle, start.walk,
                start.run, start.walkSpeed, start.runSpeed, start.death, Components::Actors::GetDeathAnimation(&*actor),
                join(Components::Actors::GetLegsLibrary(*actor, body)),
                join(Components::Actors::GetFaceLibrary(*actor)));
            if (expect != "expect")
                Log("KIND " + text);
            else
                Log(std::format("{} {} (expected {}{})",
                                kind == isKind && (breed.empty() || breed == isBreed) ? "PASS" : "FAIL", text, kind,
                                breed.empty() ? "" : " " + breed));
            return true;
        }
        if (name == "fitfilter")
        {
            // fitfilter [on|off|expect <on|off>|forget]: whether the lists of animations for an actor (the browser, the
            // view's menu) show only those that fit (F3); forget: as if the game had never had the models loaded, whose
            // bones (and so what fits them) are kept once known
            std::string what, expected;
            args >> what >> expected;
            if (what == "on" || what == "off")
                Components::Actors::SetFitFilter(what == "on");
            if (what == "forget")
                Components::Actors::ForgetModels();
            const std::string now = Components::Actors::IsFitFilterOn() ? "on" : "off";
            if (what == "expect")
                Log(std::format("{} fitfilter {} (expected {})", now == expected ? "PASS" : "FAIL", now, expected));
            else
                Log("FITFILTER " + now);
            return true;
        }
        if (name == "browser")
        {
            // browser [expect shown <n> | expect waiting | expect known]: what the animation browser drawn last showed:
            // how many of how many, whether only those that fit, whether that is known, or it waits for the models
            std::string expect, what;
            std::size_t expected = 0;
            args >> expect >> what >> expected;
            UI::AnimationBrowserState state;
            int frame = 0;
            RunOnRenderThread([&] {
                state = UI::GetAnimationBrowserState();
                frame = ImGui::GetFrameCount();
            });
            const auto text =
                std::format("the browser shows {} of {}{}{}{} ({} frames ago)", state.shown, state.listed,
                            state.fitting ? ", those that fit" : ", all", state.fitsKnown ? ", known" : "",
                            state.waiting ? ", waiting for its models" : "", frame - state.frame);
            if (expect != "expect")
                Log("BROWSER " + text);
            else
            {
                // (drawn lately: it is open)
                const bool open = state.frame >= 0 && frame - state.frame <= 5;
                const bool pass = open && (what == "shown"     ? state.shown == expected
                                           : what == "waiting" ? state.waiting
                                           : what == "known"   ? state.fitsKnown
                                                               : false);
                Log(std::format("{} browser {} (expected {}{})", pass ? "PASS" : "FAIL", text, what,
                                what == "shown" ? std::format(" {}", expected) : ""));
            }
            return true;
        }
        if (name == "misfit")
        {
            // misfit <actor> <animation> [expect <none|any>]: why the animation doesn't fit the actor's models (as the
            // UI says it), none when it does; PASS when it fits (none) or not (any)
            std::string which, animation, expect, expected;
            args >> which >> animation >> expect >> expected;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor)
            {
                Log(std::format("FAIL misfit {}: no such actor", which));
                return true;
            }
            const auto fits = Components::Actors::Fits(actor->models, animation);
            const auto why = Components::Actors::DescribeMisfit(actor->models, animation);
            const auto text = std::format("{} on actor {}: {}{}", animation, actor->id,
                                          !fits   ? "not known yet"
                                          : *fits ? "fits"
                                                  : "doesn't fit",
                                          why.empty() ? "" : " (" + why + ")");
            if (expect != "expect")
                Log("MISFIT " + text);
            else
                Log(std::format("{} misfit {} (expected {})",
                                fits && (expected == "none" ? *fits : !*fits) ? "PASS" : "FAIL", text, expected));
            return true;
        }
        return false;
    }

    // a killstreak's name as the harness takes it: its name lower case without spaces or dashes (rcxd, gunship,
    // carepackagechopper...)
    std::string GetKillstreakKey(const Components::KillstreakLogic::Info& info)
    {
        std::string key;
        for (const auto c : info.name)
            if (std::isalnum(static_cast<unsigned char>(c)))
                key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return key;
    }

    // the Demos list's load of a demo PLAY started (see RunKillstreakCommand for why it is apart)
    bool RunDemoLoadCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "demoload")
            return false;
        // demoload [expect <loading|idle|failed>]: logs whether a demo PLAY started is loading (the Demos list
        // waiting on it), and the last that didn't load with why; PASS when so
        std::string expect, wanted;
        args >> expect >> wanted;
        std::optional<std::string> demo, switching, needs;
        std::optional<std::pair<std::string, std::string>> failed;
        RunOnRenderThread([&] {
            const auto loader = UI::UIManager::Get().GetUIComponent<UI::DemoLoader>(UI::Component::DemoLoader);
            demo = loader->GetLoadingDemo();
            switching = loader->GetSwitchingMod();
            failed = loader->GetFailedDemo();
            needs = loader->GetFailedNeeds();
        });
        // (switching: the game's mod switched from the Demo tab)
        const auto state = switching ? "switching" : demo ? "loading" : failed ? "failed" : "idle";
        const auto text = std::format(
            "{}{}{}{}{}", state, switching ? " to '" + *switching + "'" : "", demo ? " " + *demo : "",
            failed ? std::format(" (last failed: {}: {})", failed->first, failed->second) : "",
            needs ? std::format(" (needs '{}')", *needs) : "");
        if (expect == "expect")
            Log(std::format("{} demoload {} (expected {})", wanted == state ? "PASS" : "FAIL", text, wanted));
        else
            Log("DEMOLOAD " + text);
        return true;
    }

    // Game mods: the mod the game runs, a switch as the Demo tab's Change... and its
    // notice's buttons start one, and the mod the demo list read for a demo
    bool RunGameModCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "gamemod")
        {
            // gamemod [expect <name|none>]: logs the mod the game runs (fs_game) and the mods the Demo tab found;
            // expect: PASS when it runs that one
            std::string expect, wanted;
            args >> expect >> wanted;
            const auto loaded = Mod::GetGameInterface()->GetLoadedFsGame().value_or("");
            const auto loadedName = std::string(Components::DemoModLogic::GetModName(loaded));
            std::string found;
            RunOnRenderThread([&] {
                for (const auto& mod : UI::UIManager::Get()
                                           .GetUIComponent<UI::DemoLoader>(UI::Component::DemoLoader)
                                           ->GetMods())
                    found += (found.empty() ? "" : ", ") + mod.name + " (" + mod.folder.string() + ")";
            });
            const auto text = std::format("'{}' (fs_game '{}'; mods: {})", loadedName.empty() ? "none" : loadedName,
                                          loaded, found.empty() ? "none" : found);
            if (expect == "expect")
                Log(std::format("{} gamemod {} (expected {})",
                                (wanted == "none" ? loadedName.empty() : _stricmp(wanted.c_str(), loadedName.c_str()) == 0)
                                    ? "PASS"
                                    : "FAIL",
                                text, wanted));
            else
                Log("GAMEMOD " + text);
            return true;
        }
        if (name == "modswitch")
        {
            // modswitch <name|none> [play <demo>]: switches the game's mod as the Demo tab's Change... does (with a
            // demo of the game's demos folder, or at a path of its own, played after it as the notice's "and Play
            // Demo" does); demoload says switching until it is done
            std::string mod, play, file;
            args >> mod >> play;
            std::getline(args >> std::ws, file);
            if (mod == "wait")
            {
                // modswitch wait [ms]: waits up to ms (120000) until no switch runs; PASS when none does then
                DWORD timeout = 120000;
                if (!play.empty())
                    timeout = static_cast<DWORD>(std::stoul(play));
                const auto start = GetTickCount64();
                std::optional<std::string> switching;
                do
                {
                    Sleep(200);
                    RunOnRenderThread([&] {
                        switching = UI::UIManager::Get()
                                        .GetUIComponent<UI::DemoLoader>(UI::Component::DemoLoader)
                                        ->GetSwitchingMod();
                    });
                } while (switching && GetTickCount64() - start < timeout);
                Log(std::format("{} modswitch wait: {} after {} ms", switching ? "FAIL" : "PASS",
                                switching ? "still switching to '" + *switching + "'" : "done",
                                GetTickCount64() - start));
                return true;
            }
            std::optional<std::filesystem::path> thenPlay;
            if (play == "play" && !file.empty())
            {
                const auto homePath = Functions::FindDvar("fs_homepath");
                thenPlay = std::filesystem::path(file).is_absolute()
                               ? std::filesystem::path(file)
                               : std::filesystem::path(homePath ? homePath->current.string : "") / "demos" / file;
            }
            bool started = false;
            RunOnRenderThread([&] {
                started = UI::UIManager::Get().GetUIComponent<UI::DemoLoader>(UI::Component::DemoLoader)->SwitchMod(
                    mod == "none" ? "" : mod, thenPlay);
            });
            Log(std::format("MODSWITCH {}{}{}", mod, thenPlay ? " then " + thenPlay->string() : "",
                            started ? "" : " refused: a demo or a mod is loading"));
            return true;
        }
        if (name == "demolist")
        {
            // demolist expect mod <demo> <name|none>: PASS when the demo list read that mod for a demo of the game's
            // demos folder (or at a path of its own), waiting up to 10 s for the list to read it
            std::string expect, what, file, wanted;
            args >> expect >> what >> file >> wanted;
            const auto homePath = Functions::FindDvar("fs_homepath");
            const auto path = std::filesystem::path(file).is_absolute()
                                  ? std::filesystem::path(file)
                                  : std::filesystem::path(homePath ? homePath->current.string : "") / "demos" / file;
            std::optional<std::string> fsGame;
            for (const auto start = GetTickCount64(); !fsGame && GetTickCount64() - start < 10000; Sleep(100))
                RunOnRenderThread([&] {
                    fsGame = UI::UIManager::Get()
                                 .GetUIComponent<UI::DemoLoader>(UI::Component::DemoLoader)
                                 ->GetDemoFsGame(path);
                });
            const auto modName = fsGame ? std::string(Components::DemoModLogic::GetModName(*fsGame)) : "";
            const bool pass = fsGame && (wanted == "none" ? modName.empty() : modName == wanted);
            Log(std::format("{} demolist mod {} is '{}' (expected {})", pass ? "PASS" : "FAIL", file,
                            fsGame ? (modName.empty() ? "none" : modName) : "not read", wanted));
            return true;
        }
        return false;
    }

    // fxwatch <n>, fxwatch: records the arguments of the next n effects spawned (FX_SpawnEffect, whatever spawns
    // them), and logs them (FXSPAWN: each argument in hex, the effect's name, the three floats the fourth points at)
    // the sun's shadow map's size while the last capture ran (the capture command), for shadowmap captured
    std::int32_t capturedShadowMapSize = 0;

    bool RunAreaCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "areas")
            return false;
        // areas, areas expect <least>, areas go <n>: logs the parts of the campaign level loaded;
        // PASS when it has at least that many; puts the free camera at one, at eye height, as a demo's picks will
        const auto areas = CampaignMaps::GetAreas();
        std::string what;
        std::size_t number = 0;
        args >> what;
        if (what == "state")
        {
            // areas state [expect <open|closed> [<area gone to, from 1>]]: the map's state
            const auto state = UI::AreaMap::GetState();
            const auto line = std::format("map {}, {} areas, at {}", state.open ? "open" : "closed", state.areas,
                                          state.current ? *state.current + 1 : 0);
            std::string expect, open;
            args >> expect >> open;
            const bool anyArea = !(args >> number);
            if (expect == "expect")
                Log(std::format("{} areas: {}", (open == "open") == state.open && (anyArea || number == (state.current ? *state.current + 1 : 0))
                                                     ? "PASS"
                                                     : "FAIL",
                                line));
            else
                Log("AREAS " + line);
            return true;
        }
        if (what == "player")
        {
            // areas player: PASS when the live game's player is at the spot of the area gone to, noclip on
            number = UI::AreaMap::GetState().current.value_or(0);
            const auto cg = Structures::GetClientGlobals();
            if (cg && number < areas.size())
            {
                const auto& ps = cg->predictedPlayerState;
                const glm::vec3 at(ps.origin[0], ps.origin[1], ps.origin[2]);
                const auto distance = glm::distance(at, areas[number].spot);
                Log(std::format("{} areas player at {:.0f} {:.0f} {:.0f}, {:.0f} from area {}, noclip {}",
                                distance < 100.0f && ps.pmType == 2 ? "PASS" : "FAIL", at.x, at.y, at.z, distance, number,
                                ps.pmType == 2));
            }
            return true;
        }
        args >> number;
        if (what == "expect")
        {
            Log(std::format("{} areas: {} (at least {})", areas.size() >= number ? "PASS" : "FAIL", areas.size(), number));
            return true;
        }
        if (what == "go")
        {
            if (number >= areas.size())
            {
                Log(std::format("FAIL areas go {}: the level has {}", number, areas.size()));
                return true;
            }
            auto& camera = Components::CameraManager::Get().GetActiveCamera();
            camera->GetPosition() = areas[number].spot + glm::vec3(0.0f, 0.0f, 60.0f);
            camera->GetRotation() = glm::vec3(0.0f, areas[number].yaw, 0.0f);
            Log(std::format("AREAS at {}", number));
            return true;
        }
        for (std::size_t i = 0; i < areas.size(); ++i)
        {
            const auto size = areas[i].maxs - areas[i].mins;
            Log(std::format("AREA {} at {:.0f} {:.0f} {:.0f} yaw {:.0f}, {} nodes, {:.0f} x {:.0f}", i, areas[i].spot.x,
                            areas[i].spot.y, areas[i].spot.z, areas[i].yaw, areas[i].nodes, size.x, size.y));
        }
        return true;
    }

    bool RunZoneTexturesCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "zonetextures")
            return false;
        // zonetextures file <path>|default, zonetextures <zone> [expect present|missing]: rereads the character zones
        // with that file as the textures'; logs whether a zone can load, and PASS when
        // it is as expected
        std::string first, what, state;
        args >> first >> what >> state;
        if (first == "file")
        {
            Components::Actors::SetTexturesFile(what == "default" ? std::filesystem::path() : std::filesystem::path(what));
            Components::Actors::RescanCharacterZones();
            Log(std::format("ZONETEXTURES file {}", what));
            return true;
        }
        const auto missing = Components::Actors::IsCharacterZoneMissingTextures(first);
        const auto available = Components::Actors::IsCharacterZoneAvailable(first);
        if (what == "expect")
            Log(std::format("{} zonetextures {}: {}{}", (state == "missing") == missing ? "PASS" : "FAIL", first,
                            missing ? "missing" : "present", available ? ", available" : ""));
        else
            Log(std::format("ZONETEXTURES {}: {}{}", first, missing ? "missing" : "present",
                            available ? ", available" : ""));
        return true;
    }

    // entscan <x> <y> <z> <radius>: logs the entities near a point, as the server has them (in a game of one's own) and
    // as the client got them: number, type, where, whether drawn; entscan wrapped [expect <most>]: how many of the
    // entities the client got aren't where the server has them, more than half a range out;
    // entscan near <x> <y> <z> <radius> expect <least>: PASS when the client has that many there
    bool RunEntScanCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "entscan")
            return false;
        if (args.peek() == ' ')
            args >> std::ws;
        if (args.peek() == 'w' || args.peek() == 'n')
        {
            std::string what, word;
            args >> what;
            if (what == "wrapped")
            {
                std::int32_t most = 0;
                args >> word >> most;
                RunOnMainThread([&] {
                    const auto server = reinterpret_cast<const std::uint8_t*>(0x32E5640);
                    const auto entities = Structures::GetEntities();
                    std::int32_t got = 0, wrong = 0, sent = 0;
                    for (std::size_t i = 0; entities && i < Structures::MAX_GENTITIES; ++i)
                    {
                        const auto& cent = entities[i];
                        const auto& state = *reinterpret_cast<const Structures::entityState_s*>(server + i * 0x2F8);
                        const auto& theirs = state.pos;
                        sent += state.eType != 0 && i >= Structures::MAX_CLIENTS ? 1 : 0;
                        if (!(cent.flags & Structures::CENTITY_IN_SNAPSHOT) || i < Structures::MAX_CLIENTS)
                            continue;
                        ++got;
                        const glm::vec3 a(cent.nextState.pos.trBase[0], cent.nextState.pos.trBase[1], cent.nextState.pos.trBase[2]);
                        const glm::vec3 b(theirs.trBase[0], theirs.trBase[1], theirs.trBase[2]);
                        wrong += glm::distance(a, b) > 1000.0f ? 1 : 0;
                    }
                    Log(std::format("{}ENTSCAN wrapped {} of {} ({} on the server), moved back {} at most in a snapshot",
                                    word != "expect" ? "" : (wrong <= most && got > 0 ? "PASS " : "FAIL "), wrong, got, sent,
                                    CampaignMaps::TakeSnapshotEntitiesMoved()));
                });
                return true;
            }
            glm::vec3 at{0.0f};
            float radius = 0.0f;
            std::int32_t least = 0;
            args >> at.x >> at.y >> at.z >> radius >> word >> least;
            RunOnMainThread([&] {
                const auto entities = Structures::GetEntities();
                std::int32_t got = 0;
                for (std::size_t i = 0; entities && i < Structures::MAX_GENTITIES; ++i)
                {
                    const auto& cent = entities[i];
                    const auto& base = cent.nextState.pos.trBase;
                    if ((cent.flags & Structures::CENTITY_IN_SNAPSHOT) &&
                        glm::distance(glm::vec3(base[0], base[1], base[2]), at) <= radius)
                        ++got;
                }
                Log(std::format("{} entscan near: {} there, at least {}", got >= least ? "PASS" : "FAIL", got, least));
            });
            return true;
        }
        glm::vec3 at{0.0f};
        float radius = 100.0f;
        args >> at.x >> at.y >> at.z >> radius;
        RunOnMainThread([&] {
            constexpr std::size_t GENTITY_SIZE = 0x2F8;
            const auto server = reinterpret_cast<const std::uint8_t*>(0x32E5640);
            std::size_t serverCount = 0, clientCount = 0;
            for (std::size_t i = 0; i < Structures::MAX_GENTITIES; ++i)
            {
                const auto& es = *reinterpret_cast<const Structures::entityState_s*>(server + i * GENTITY_SIZE);
                const glm::vec3 pos(es.pos.trBase[0], es.pos.trBase[1], es.pos.trBase[2]);
                if (es.eType == 0 || glm::distance(pos, at) > radius)
                    continue;
                ++serverCount;
                std::string raw;
                const auto bytes = reinterpret_cast<const std::uint32_t*>(&es);
                for (const auto offset : {0x54, 0x58, 0x5C, 0x60, 0x64, 0x90, 0x94, 0x98})
                    raw += std::format(" {:08x}", bytes[offset / 4]);
                Log(std::format("ENTSCAN server {} type {} flags {:08x} at {:.0f} {:.0f} {:.0f}{}", i, static_cast<int>(es.eType),
                                es.eFlags, pos.x, pos.y, pos.z, raw));
            }
            const auto entities = Structures::GetEntities();
            for (std::size_t i = 0; entities && i < Structures::MAX_GENTITIES; ++i)
            {
                const auto& cent = entities[i];
                const auto& es = cent.nextState;
                const glm::vec3 pos(es.pos.trBase[0], es.pos.trBase[1], es.pos.trBase[2]);
                if (es.eType == 0 || glm::distance(pos, at) > radius)
                    continue;
                ++clientCount;
                Log(std::format("ENTSCAN client {} type {} in snapshot {} flags {:08x} at {:.0f} {:.0f} {:.0f}", i,
                                static_cast<int>(es.eType), (cent.flags & Structures::CENTITY_IN_SNAPSHOT) ? 1 : 0, es.eFlags,
                                pos.x, pos.y, pos.z));
            }
            Log(std::format("ENTSCAN {} on the server, {} on the client", serverCount, clientCount));
        });
        return true;
    }

    bool RunClientEntityCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "cliententities")
            return false;
        // cliententities [expect <most>]: logs how many of the client's 512 entities are script models (client
        // scripts' Spawn, multiplayer WMD's train); expect: PASS when at most that many
        constexpr std::size_t CLIENT_ENTITY_SIZE = 0x32C;
        constexpr std::size_t CLIENT_ENTITY_TYPE = 4 + 2;
        constexpr std::uint8_t ENTITY_SCRIPT_MODEL = 6;
        const auto table = *reinterpret_cast<const std::uint8_t**>(GetGameAddresses().clientEntities());
        std::size_t models = 0;
        for (std::size_t i = 0; table && i < 512; ++i)
            models += table[i * CLIENT_ENTITY_SIZE + CLIENT_ENTITY_TYPE] == ENTITY_SCRIPT_MODEL ? 1 : 0;
        std::string what;
        std::size_t most = 0;
        args >> what >> most;
        if (what == "expect")
            Log(std::format("{} cliententities: {} script models (at most {})", models <= most ? "PASS" : "FAIL", models,
                            most));
        else
            Log(std::format("CLIENTENTITIES {} script models", models));
        return true;
    }

    bool RunModelLightingCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "modellighting")
            return false;
        // modellighting [reset | expect <listed> <shared>]: logs the frame's dynamic models and the lighting cache;
        // expect: PASS when the list reached that many since reset, that many shared a
        // lit slot and the last frame left none unlit
        std::string what;
        args >> what;
        if (what == "reset")
        {
            ModelLighting::ResetStats();
            return true;
        }
        const auto stats = ModelLighting::GetStats();
        const auto line = std::format(
            "{} dynamic models, {} in the frame's list: {} lit, {} shared, {} unlit; most since reset: {} listed, {} "
            "shared, {} unlit, the cache full in {} frames",
            stats.dynamicModels, stats.listed, stats.lit, stats.shared, stats.unlit, stats.mostListed, stats.mostShared,
            stats.mostUnlit, stats.cacheFull);
        if (what == "expect")
        {
            std::uint32_t listed = 0, shared = 0;
            args >> listed >> shared;
            const auto pass = stats.mostListed >= listed && stats.mostShared >= shared && stats.unlit == 0;
            Log(std::format("{} modellighting: {} (expected at least {} listed, {} shared)", pass ? "PASS" : "FAIL",
                            line, listed, shared));
        }
        else
            Log(std::format("MODELLIGHTING {}", line));
        return true;
    }

    bool RunShadowMapCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "shadowmap")
            return false;
        // shadowmap [<size> [wait] | expect <size> | captured <size>]: logs the size of the sun's shadow map (each half;
        // the game's 1,024) and the video memory the device says is free; with a size, sets it, restarting the renderer
        // (vid_restart), and with wait waits up to 30 s for the map to have it; expect: PASS when it has that size (it
        // waits up to 5 s for it), captured: when the last capture ran with it
        std::string what, wait;
        args >> what >> wait;
        if (what == "captured")
        {
            const auto size = std::stoi(wait);
            Log(std::format("{} shadowmap {} while the last capture ran (expected {})",
                            capturedShadowMapSize == size ? "PASS" : "FAIL", capturedShadowMapSize, size));
            return true;
        }
        const auto memory = [] {
            const auto device = Mod::GetGameInterface()->GetGameDevicePtr();
            return device ? device->GetAvailableTextureMem() >> 20 : 0u;
        };
        if (what == "expect")
        {
            const auto size = std::stoi(wait);
            for (int i = 0; i < 50 && Hooks::Rendering::GetSunShadowMapSize() != size; ++i)
                Sleep(100);
            const auto now = Hooks::Rendering::GetSunShadowMapSize();
            Log(std::format("{} shadowmap {} (expected {}), {} MB free", now == size ? "PASS" : "FAIL", now, size,
                            memory()));
            return true;
        }
        if (!what.empty())
        {
            const auto size = std::stoi(what);
            const auto before = memory();
            const auto started = std::chrono::steady_clock::now();
            Hooks::Rendering::SetSunShadowMapSize(size, true);
            if (wait == "wait")
                for (int i = 0; i < 300 && Hooks::Rendering::GetSunShadowMapSize() != size; ++i)
                    Sleep(100);
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            Log(std::format("SHADOWMAP set {}: now {} after {} ms, {} MB free before, {} MB after", size,
                            Hooks::Rendering::GetSunShadowMapSize(), took.count(), before, memory()));
            return true;
        }
        Log(std::format("SHADOWMAP {}, {} MB free", Hooks::Rendering::GetSunShadowMapSize(), memory()));
        return true;
    }

    bool RunSurfaceCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "surface")
            return false;
        // surface: logs Surface depth's settings and what it made (Components::SurfaceDepth);
        // surface shadow <x>: how dark the height's shadow falls (8, a tuning); surface heights: what each of the map's
        // normal maps that may take depth got; surface height <image> <width> pack|game: PASS when it got that;
        // surface depth on|off [<depth>]: turns it on or off, with a depth (world units); surface bump <strength>: the
        // normal maps' Bump strength (1 the game's); both set the Look tab's rows, as a preset would; surface slope <slope>: the flattest view the depth takes (0.15, a
        // tuning); surface wait [<seconds>]: PASS when the map's heights and the
        // shaders drawn are made (up to 60 s); surface expect drawn|game: PASS when the map's surfaces drew patched in
        // a frame, or nothing did; surface normals: logs the map's normal maps as the game has them
        namespace Surface = Components::SurfaceDepth;
        std::string what;
        args >> what;
        const auto describe = [] {
            const auto stats = Surface::GetStats();
            return std::format("{} depth {} bump {} slope {} cavity {} shadow {}: {} shaders with normal maps drawn, {} patched ({} with depth, {} "
                               "failed), {} normal maps of the map, {} heights made, {} unreadable, {} not streamed "
                               "in, {} with depth ({} from the pack, {} KB on the card); last frame {} draws patched, {} with the map's",
                               Surface::IsOn() ? "on" : "off", Surface::GetDepth(), Surface::GetBumpStrength(),
                               Surface::GetMinViewSlope(), Surface::GetCavity(), Surface::GetShadow(),
                               stats.shaders, stats.patched, stats.withDepth, stats.failed, stats.normalMaps,
                               stats.heights, stats.unreadable, stats.streaming, stats.withDepthMaps, stats.fromPack,
                               stats.heightBytes / 1024, stats.patchedDraws, stats.mapDraws);
        };
        // (the Look tab's rows, which set Surface depth each frame)
        const auto visualsMenu = UI::UIManager::Get().GetUIComponent<UI::VisualsMenu>(UI::Component::VisualsMenu);
        if (what == "depth")
        {
            std::string state;
            float depth = Surface::GetDepth();
            args >> state >> depth;
            RunOnRenderThread([&] {
                visualsMenu->SetSurface(state == "on", depth, visualsMenu->GetSettings().bumpStrength);
            });
        }
        else if (what == "bump")
        {
            float strength = 1.0f;
            args >> strength;
            RunOnRenderThread([&] {
                const auto& settings = visualsMenu->GetSettings();
                visualsMenu->SetSurface(settings.surfaceDepth, settings.surfaceDepthAmount, strength);
            });
        }
        else if (what == "cavity")
        {
            float cavity = Components::SurfaceLogic::DEFAULT_CAVITY;
            args >> cavity;
            Surface::SetCavity(cavity);
        }
        else if (what == "shadow")
        {
            float shadow = Components::SurfaceLogic::DEFAULT_SHADOW;
            args >> shadow;
            Surface::SetShadow(shadow);
        }
        else if (what == "heights")
        {
            std::vector<Surface::HeightInfo> infos;
            RunOnRenderThread([&] { infos = Surface::GetHeights(); });
            for (const auto& info : infos)
                Log(std::format("SURFACE height {} {}x{} relief {:.5f} share {:.2f}{}{}", info.name, info.width,
                                info.height, info.relief, info.share, info.fromPack ? " pack" : " game",
                                info.made ? " made" : ""));
            Log(std::format("SURFACE heights: {} that may take depth", infos.size()));
            return true;
        }
        else if (what == "height")
        {
            // height <image> <width> pack|game: PASS when the image's height on the card is that wide, from there
            std::string image, source;
            int width = 0;
            args >> image >> width >> source;
            std::vector<Surface::HeightInfo> infos;
            RunOnRenderThread([&] { infos = Surface::GetHeights(); });
            const auto found = std::ranges::find(infos, image, &Surface::HeightInfo::name);
            const bool met = found != infos.end() && found->made && found->width == width &&
                             found->fromPack == (source == "pack");
            Log(std::format("{} surface height {}: {} wide, {}", met ? "PASS" : "FAIL", image,
                            found != infos.end() ? found->width : 0,
                            found != infos.end() && found->fromPack ? "pack" : "game"));
            return true;
        }
        else if (what == "slope")
        {
            float slope = Components::SurfaceLogic::MIN_VIEW_SLOPE;
            args >> slope;
            Surface::SetMinViewSlope(slope);
        }
        else if (what == "wait")
        {
            int seconds = 60;
            args >> seconds;
            const auto ready = [] {
                const auto stats = Surface::GetStats();
                return stats.heights > 0 && stats.heights + stats.unreadable + stats.streaming == stats.normalMaps &&
                       stats.patched + stats.failed == stats.shaders && stats.mapDraws > 0;
            };
            for (int i = 0; i < seconds * 10 && !ready(); ++i)
                Sleep(100);
            // (a patch that didn't assemble fails it)
            Log(std::format("{} surface ready: {}", ready() && Surface::GetStats().failed == 0 ? "PASS" : "FAIL",
                            describe()));
            return true;
        }
        else if (what == "expect")
        {
            // drawn: the map's surfaces drew patched in a frame (up to 3 s); game: no shader drew patched; frompack,
            // fromgame: the heights on the card came from a pack, or none did
            std::string state;
            args >> state;
            const auto met = [&] {
                const auto stats = Surface::GetStats();
                if (state == "frompack")
                    return stats.fromPack > 0;
                if (state == "fromgame")
                    return stats.withDepthMaps > 0 && stats.fromPack == 0;
                return state == "drawn" ? stats.mapDraws > 0 : stats.patchedDraws == 0;
            };
            for (int i = 0; i < 30 && !met(); ++i)
                Sleep(100);
            Log(std::format("{} surface {}: {}", met() ? "PASS" : "FAIL", state, describe()));
            return true;
        }
        else if (what == "normals")
        {
            std::vector<Types::NormalMap> normalMaps;
            RunOnMainThread([&] { normalMaps = Surfaces::GetMapNormalMaps(); });
            for (const auto& normalMap : normalMaps)
            {
                // its texture's first level now (smaller while streamed out)
                const auto texture = *normalMap.texture;
                D3DSURFACE_DESC desc{};
                if (texture && texture->GetType() == D3DRTYPE_TEXTURE)
                    static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc);
                Log(std::format("SURFACE normal map {} texture {:08X} {}x{} format {:08X} pool {}{}", normalMap.name,
                                reinterpret_cast<std::uintptr_t>(texture), desc.Width, desc.Height,
                                static_cast<std::uint32_t>(desc.Format), static_cast<int>(desc.Pool),
                                normalMap.reliefWeight > 0.0f ? " takes depth" : ""));
            }
        }
        Log("SURFACE " + describe());
        return true;
    }

    // the images the game has registered, by name (lowercase), with where each keeps its texture (texpack)
    std::vector<std::pair<std::string, Structures::GfxImage*>> GetRegisteredImages()
    {
        std::vector<std::pair<std::string, Structures::GfxImage*>> images;
        RunOnMainThread([&] {
            const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
            const auto entries =
                reinterpret_cast<const Structures::XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
            for (std::size_t hash = 0; hash < Structures::DB_HASH_TABLE_SIZE; ++hash)
                for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
                {
                    const auto& entry = entries[index];
                    if (entry.type != Structures::ASSET_TYPE_IMAGE || !entry.header)
                        continue;
                    const auto image =
                        const_cast<Structures::GfxImage*>(static_cast<const Structures::GfxImage*>(entry.header));
                    if (!image->name)
                        continue;
                    std::string imageName = image->name;
                    std::ranges::transform(imageName, imageName.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    images.emplace_back(std::move(imageName), image);
                }
        });
        return images;
    }

    // a texture as "<width>x<height> levels <n> format <f> pool <p>"
    std::string DescribeTexture(IDirect3DBaseTexture9* base)
    {
        if (!base)
            return "none";
        if (base->GetType() != D3DRTYPE_TEXTURE)
            return std::format("type {}", static_cast<int>(base->GetType()));
        const auto texture = static_cast<IDirect3DTexture9*>(base);
        D3DSURFACE_DESC desc{};
        texture->GetLevelDesc(0, &desc);
        return std::format("{}x{} levels {} format {:08X} pool {}", desc.Width, desc.Height, texture->GetLevelCount(),
                           static_cast<std::uint32_t>(desc.Format), static_cast<int>(desc.Pool));
    }

    // texpack's state line: the pack matched, what the card holds, the address space
    std::string DescribeTexturePack()
    {
        namespace Pack = Components::TexturePack;
        const auto stats = Pack::GetStats();
        const auto memory = GetMemoryStats(false);
        return std::format("{}: {} images ({:.0f} MB on disk), {} made, {} waiting, {} failed, {} refused, {} evicted, "
                           "{} swaps last frame ({} set, {} watched), {} drawn before made, waited {:.0f} ms on average, {:.0f} at most, making took {:.1f} ms "
                           "a frame at most, {:.0f} MB in use of a {:.0f} MB budget (card free at the match "
                           "{:.0f} MB), {} matches ({:.0f} ms); address space free {:.0f} MB, largest {:.0f} MB, "
                           "private {:.0f} MB; line: {}",
                           Pack::IsOn() ? "on" : "off", stats.images, stats.packBytes / 1048576.0, stats.made,
                           stats.waiting, stats.failed, stats.refused, stats.evicted, stats.swaps, stats.calls,
                           stats.watched, stats.drawnWaits, stats.meanWaitMs, stats.mostWaitMs, stats.mostMakeMs, stats.inUse / 1048576.0, stats.budget / 1048576.0, stats.freeAtLoad / 1048576.0,
                           stats.matches, stats.matchMs, memory.free / 1048576.0, memory.largestFree / 1048576.0,
                           memory.privateBytes / 1048576.0, Components::TextureLogic::GetStatusLine(Pack::GetStatus()));
    }

    // texpack use: a folder of packs (with v1, imported) in place of the mod's, matched again; waits for the match
    void UseTexturePack(std::istringstream& args)
    {
        namespace Pack = Components::TexturePack;
        std::string folder;
        args >> std::quoted(folder);
        const auto matches = Pack::GetStats().matches;
        Pack::SetFolder(folder == "default" || folder.empty() ? std::nullopt
                                                              : std::optional<std::filesystem::path>(folder));
        for (int i = 0; i < 300 && Pack::GetStats().matches == matches; ++i)
            Sleep(50);
        Log(std::format("TEXPACK {} {}", Pack::GetStats().matches == matches ? "no match yet" : "matched",
                        Pack::GetFolder().string()));
    }

    // texpack wait [seconds]: until nothing drawn waits to be made, PASS with the time it took
    // texpack settle [ms]: waits until the zones loaded have stayed the same for that long (2000; the worker starts a
    // match a second after they change) and no match has been under way for half a second, PASS with the time it took:
    // a match reads every pack's index (with large 2x packs, up to 9 MB of the process's heap for a second or
    // more, and the match made is held beside the last one until the render thread takes it), which a reading of the
    // heap then took for growth
    void SettleTexturePack(std::istringstream& args)
    {
        namespace Pack = Components::TexturePack;
        std::uint64_t quiet = 0;
        if (!(args >> quiet))
            quiet = 2000;
        const auto started = GetTickCount64();
        auto key = Mod::GetGameInterface()->GetLoadedZonesKey();
        auto keySince = started, idleSince = started;
        bool settled = false;
        while (!settled && GetTickCount64() - started < quiet + 30000)
        {
            Sleep(100);
            const auto now = GetTickCount64();
            if (auto currentKey = Mod::GetGameInterface()->GetLoadedZonesKey(); currentKey != key)
            {
                key = std::move(currentKey);
                keySince = now;
            }
            if (Pack::IsMatching())
                idleSince = now;
            settled = now - keySince >= quiet && now - idleSince >= 500;
        }
        const auto matches = Pack::GetStats().matches;
        Log(std::format("{} texpack settle after {} ms ({} matches)", settled ? "PASS" : "FAIL",
                        GetTickCount64() - started, matches));
    }

    void WaitForTexturePack(std::istringstream& args)
    {
        namespace Pack = Components::TexturePack;
        float seconds = 30.0f;
        args >> seconds;
        const auto started = std::chrono::steady_clock::now();
        int settled = 0;
        while (std::chrono::steady_clock::now() - started < std::chrono::duration<float>(seconds))
        {
            const auto stats = Pack::GetStats();
            settled = stats.waiting == 0 && stats.madeLastFrame == 0 ? settled + 1 : 0;
            // a quarter of a second with nothing waiting
            if (settled >= 10)
                break;
            Sleep(25);
        }
        const auto took = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
        Log(std::format("{} texpack wait {:.2f} s ({})", settled >= 10 ? "PASS" : "FAIL", took,
                        DescribeTexturePack()));
    }

    // texpack expect drawn|game: PASS when the pack's textures are drawn (within 10 s: after a restart of the renderer
    // the game streams its images in again first), or none in a few frames (game)
    void ExpectTexturePack(std::istringstream& args)
    {
        std::string what;
        args >> what;
        // repair | norepair: the Look tab's line says to repair the pack, or doesn't
        if (what == "repair" || what == "norepair")
        {
            const auto status = Components::TexturePack::GetStatus();
            Log(std::format("{} texpack expect {} ({})", status.repair == (what == "repair") ? "PASS" : "FAIL", what,
                            Components::TextureLogic::GetStatusLine(status)));
            return;
        }
        // swaps are counted a frame at a time, from after the overlay frames that take in a click just before (Game):
        // counted from at once, a slow frame still swapping before the click counted against it (capture-pack-depth,
        // full suite of 2026-11-08)
        WaitForOverlayFrames();
        WaitForOverlayFrames();
        std::uint32_t swaps = 0;
        for (int i = 0; i < (what == "game" ? 6 : 330) && (what == "game" || swaps == 0); ++i)
        {
            Sleep(30);
            swaps = std::max(swaps, Components::TexturePack::GetStats().swaps);
        }
        const bool passed = what == "game" ? swaps == 0 : swaps > 0;
        Log(std::format("{} texpack expect {} ({} swaps; {})", passed ? "PASS" : "FAIL", what, swaps,
                        DescribeTexturePack()));
    }

    // texpack groups: the images matched by group, those off too | texpack off <group>... | texpack off none |
    // texpack off file: groups off in place of use.json's, or use.json's again (matched again)
    void SetTexturePackGroups(const std::string& what, std::istringstream& args)
    {
        namespace Pack = Components::TexturePack;
        using Components::TextureLogic::Group;
        if (what == "off")
        {
            std::set<Group> groups;
            std::string name;
            bool file = false;
            while (args >> name)
            {
                file |= name == "file";
                if (const auto group = Components::TextureLogic::FindGroup(name))
                    groups.insert(*group);
            }
            const auto matches = Pack::GetStats().matches;
            Pack::SetGroupsOffOverride(file ? std::nullopt : std::optional(groups));
            for (int i = 0; i < 300 && Pack::GetStats().matches == matches; ++i)
                Sleep(50);
        }
        const auto stats = Pack::GetStats();
        std::string line;
        for (const auto group : Components::TextureLogic::GROUPS)
        {
            const auto index = static_cast<std::size_t>(group);
            line += std::format(" {} {}{}", Components::TextureLogic::GetName(group), stats.groups[index],
                                stats.groupsOff[index] ? std::format(" (off {})", stats.groupsOff[index]) : "");
        }
        Log("TEXPACK groups" + line);
    }

    // texpack stress <MB> [default|managed]: holds that much of 2048 x 2048 DXT5 textures with their levels (5.3 MB
    // each, drawn nowhere), to see what a whole map's pack takes of the address space; 0 lets them go
    void StressTexturePack(std::istringstream& args)
    {
        static std::vector<IDirect3DTexture9*> held;
        std::uint32_t megabytes = 0;
        std::string pool = "default";
        args >> megabytes >> pool;
        std::uint32_t made = 0;
        RunOnRenderThread([&] {
            for (const auto texture : held)
                texture->Release();
            held.clear();
            const auto device = D3D9::GetDevice();
            for (std::uint64_t bytes = 0; device && bytes < std::uint64_t{megabytes} << 20; bytes += 5592400)
            {
                IDirect3DTexture9* texture = nullptr;
                if (FAILED(device->CreateTexture(2048, 2048, 0, 0, D3DFMT_DXT5,
                                                 pool == "managed" ? D3DPOOL_MANAGED : D3DPOOL_DEFAULT, &texture,
                                                 nullptr)))
                    break;
                held.push_back(texture);
            }
            made = static_cast<std::uint32_t>(held.size());
        });
        Log(std::format("TEXPACK stress {} textures ({:.0f} MB) {}", made, made * 5592400 / 1048576.0, pool));
    }

    // texpack find <part> | texpack info <image>: the registered images whose name has it (or is it), with their
    // textures now (and the GfxImage's bytes)
    void FindTexturePackImages(const std::string& what, std::istringstream& args)
    {
        std::string part;
        args >> part;
        std::ranges::transform(part, part.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::size_t count = 0;
        for (const auto& [imageName, image] : GetRegisteredImages())
        {
            if (what == "find" ? imageName.find(part) == std::string::npos : imageName != part)
                continue;
            std::string texture;
            RunOnRenderThread([&] { texture = DescribeTexture(static_cast<IDirect3DBaseTexture9*>(image->texture)); });
            std::string bytes;
            if (what == "info")
                for (std::size_t i = 0; i < sizeof(Structures::GfxImage); ++i)
                    bytes += std::format("{:02X}", reinterpret_cast<const std::uint8_t*>(image)[i]);
            Log(std::format("TEXPACK image {} card {} KB texture {}{}{}", image->name, image->cardMemory >> 10, texture,
                            bytes.empty() ? "" : " bytes ", bytes));
            if (++count >= 300)
                break;
        }
    }

    // The game's frame as GrabFrame takes it, scaled down to a width: its texels as B, G, R, X bytes
    struct Pixels
    {
        UINT width = 0, height = 0;
        std::vector<std::uint8_t> bgrx;
    };
    std::optional<Pixels> GrabPixels(UINT width)
    {
        std::optional<Pixels> pixels;
        RunOnRenderThread([&] {
            const auto device = D3D9::GetDevice();
            IDirect3DSurface9* backBuffer = nullptr;
            if (!device || FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
                return;
            D3DSURFACE_DESC desc{};
            backBuffer->GetDesc(&desc);
            const auto w = std::min(width, desc.Width);
            const auto h = std::max<UINT>(1, desc.Height * w / desc.Width);
            IDirect3DSurface9 *copy = nullptr, *read = nullptr, *scaled = nullptr;
            D3DLOCKED_RECT locked{};
            if (SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                                     FALSE, &copy, nullptr)) &&
                SUCCEEDED(device->StretchRect(backBuffer, nullptr, copy, nullptr, D3DTEXF_NONE)) &&
                SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8,
                                                              D3DPOOL_SYSTEMMEM, &read, nullptr)) &&
                SUCCEEDED(device->GetRenderTargetData(copy, read)) &&
                SUCCEEDED(
                    device->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &scaled, nullptr)) &&
                SUCCEEDED(D3DXLoadSurfaceFromSurface(scaled, nullptr, nullptr, read, nullptr, nullptr,
                                                     D3DX_FILTER_TRIANGLE, 0)) &&
                SUCCEEDED(scaled->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            {
                pixels.emplace(Pixels{w, h, std::vector<std::uint8_t>(std::size_t{w} * h * 4)});
                for (UINT y = 0; y < h; ++y)
                    std::memcpy(pixels->bgrx.data() + std::size_t{y} * w * 4,
                                static_cast<const std::uint8_t*>(locked.pBits) + std::size_t{y} * locked.Pitch, w * 4);
                scaled->UnlockRect();
            }
            for (const auto surface : {scaled, read, copy, backBuffer})
                if (surface)
                    surface->Release();
        });
        return pixels;
    }

    // texpack pixels mark | texpack pixels changed|same|lit|unlit [x0 y0 x1 y1 [limit]]: the frame (640 wide) kept, or
    // compared with the one kept in a part of it (fractions of its width and height, all of it by default): PASS for
    // changed when more
    // than 1% of its pixels moved by more than 8 levels, for same when at most 0.5% did and they are 0.5 apart at most
    void CompareTexturePackPixels(std::istringstream& args)
    {
        static std::optional<Pixels> marked;
        std::string what;
        float x0 = 0.0f, y0 = 0.0f, x1 = 1.0f, y1 = 1.0f;
        args >> what >> x0 >> y0 >> x1 >> y1;
        Sleep(100);
        auto pixels = GrabPixels(640);
        if (what == "mark" || !pixels || !marked || pixels->width != marked->width || pixels->height != marked->height)
        {
            if (what == "mark")
                marked = std::move(pixels);
            Log(std::format("{} texpack pixels {}", what == "mark" && marked ? "TEXPACK" : "FAIL", what));
            return;
        }
        const auto left = static_cast<UINT>(x0 * pixels->width), right = static_cast<UINT>(x1 * pixels->width);
        const auto top = static_cast<UINT>(y0 * pixels->height), bottom = static_cast<UINT>(y1 * pixels->height);
        double sum = 0.0;
        std::size_t count = 0, moved = 0;
        for (UINT y = top; y < bottom && y < pixels->height; ++y)
            for (UINT x = left; x < right && x < pixels->width; ++x)
            {
                const auto at = (std::size_t{y} * pixels->width + x) * 4;
                int most = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const int difference = std::abs(int(pixels->bgrx[at + c]) - int(marked->bgrx[at + c]));
                    sum += difference;
                    most = std::max(most, difference);
                }
                ++count;
                moved += most > 8;
            }
        if (what == "lit" || what == "unlit")
        {
            // the lighting: the largest change of the mean brightness of a grid's cells (30 pixels a side), which
            // detail doesn't move; the limit after the part's corners, 6 levels of 255 unless given. PASS within it
            // (lit), or past it (unlit: a scenario's control, that the check sees a wrong pack)
            float limit = 6.0f;
            args >> limit;
            float largest = 0.0f, signedLargest = 0.0f;
            UINT atX = 0, atY = 0;
            constexpr UINT CELL = 30;
            for (UINT y = top; y + CELL <= bottom && y + CELL <= pixels->height; y += CELL)
                for (UINT x = left; x + CELL <= right && x + CELL <= pixels->width; x += CELL)
                {
                    double was = 0.0, now = 0.0;
                    for (UINT v = y; v < y + CELL; ++v)
                        for (UINT u = x; u < x + CELL; ++u)
                            for (int c = 0; c < 3; ++c)
                            {
                                const auto at = (std::size_t{v} * pixels->width + u) * 4 + c;
                                was += marked->bgrx[at];
                                now += pixels->bgrx[at];
                            }
                    const auto change = static_cast<float>((now - was) / (CELL * CELL * 3));
                    if (std::abs(change) > largest)
                    {
                        largest = std::abs(change);
                        signedLargest = change;
                        atX = x;
                        atY = y;
                    }
                }
            Log(std::format("{} texpack pixels {}: at most {:+.1f} levels a cell at ({}, {}), limit {:.1f}",
                            (largest <= limit) == (what == "lit") ? "PASS" : "FAIL", what, signedLargest, atX, atY,
                            limit));
            return;
        }
        const double mean = count ? sum / (count * 3.0) : 0.0;
        const double share = count ? 100.0 * moved / count : 0.0;
        const bool passed = what == "changed" ? share > 1.0 : share <= 0.5 && mean <= 0.5;
        Log(std::format("{} texpack pixels {}: {:.2f} apart, {:.2f}% moved by more than 8 ({} pixels)",
                        passed ? "PASS" : "FAIL", what, mean, share, count));
    }

    // texpack space mark | texpack space expect <MB>: PASS when the process's free address space dropped by less than
    // that since the mark (the pack is in the card's memory, not the game's)
    void CheckTexturePackSpace(std::istringstream& args)
    {
        static std::uint64_t marked = 0;
        std::string what;
        double megabytes = 0.0;
        args >> what >> megabytes;
        const auto free = GetMemoryStats(false).free;
        if (what == "mark")
        {
            marked = free;
            Log(std::format("TEXPACK space marked: {:.0f} MB free", free / 1048576.0));
            return;
        }
        const double dropped = (static_cast<double>(marked) - static_cast<double>(free)) / 1048576.0;
        Log(std::format("{} texpack space: {:.0f} MB free, {:.0f} MB less than at the mark (at most {:.0f})",
                        dropped < megabytes ? "PASS" : "FAIL", free / 1048576.0, dropped, megabytes));
    }

    // texpack blame <x0 y0 x1 y1> [most]: what each image of the pack the frame draws does alone to a part of the frame
    // (fractions): the mean change of its brightness, largest first
    void BlameTexturePackImages(std::istringstream& args)
    {
        namespace Pack = Components::TexturePack;
        float x0 = 0.0f, y0 = 0.0f, x1 = 1.0f, y1 = 1.0f;
        std::size_t most = 12;
        args >> x0 >> y0 >> x1 >> y1 >> most;
        std::vector<std::string> drawn;
        RunOnRenderThread([&] {
            drawn = Pack::GetDrawnImages();
            Pack::SetSwappedImages(std::set<std::string>{});
        });
        Sleep(200);
        const auto base = GrabPixels(640);
        if (!base)
        {
            Log("FAIL texpack blame: no frame");
            return;
        }
        struct Effect
        {
            std::string name;
            float mean = 0.0f, most = 0.0f;
        };
        const auto measure = [&](const Pixels& frame, const std::string& name) {
            Effect effect{name};
            std::size_t count = 0;
            double sum = 0.0;
            for (UINT y = UINT(y0 * base->height); y < UINT(y1 * base->height); ++y)
                for (UINT x = UINT(x0 * base->width); x < UINT(x1 * base->width); ++x)
                {
                    const auto at = (std::size_t{y} * base->width + x) * 4;
                    float difference = 0.0f;
                    for (int c = 0; c < 3; ++c)
                        difference += float(frame.bgrx[at + c]) - float(base->bgrx[at + c]);
                    difference /= 3.0f;
                    sum += difference;
                    effect.most = std::max(effect.most, std::abs(difference));
                    ++count;
                }
            effect.mean = count ? float(sum / count) : 0.0f;
            return effect;
        };
        std::vector<Effect> effects;
        for (const auto& name : drawn)
        {
            RunOnRenderThread([&] { Pack::SetSwappedImages(std::set<std::string>{name}); });
            Sleep(120);
            if (const auto frame = GrabPixels(640))
                effects.push_back(measure(*frame, name));
        }
        RunOnRenderThread([&] { Pack::SetSwappedImages(std::nullopt); });
        Sleep(200);
        if (const auto frame = GrabPixels(640))
        {
            const auto all = measure(*frame, "all");
            Log(std::format("TEXPACK blame {} images, all of them {:+.2f} (at most {:.0f})", drawn.size(), all.mean,
                            all.most));
        }
        std::ranges::sort(effects, std::greater{}, [](const Effect& e) { return std::abs(e.mean); });
        for (std::size_t i = 0; i < std::min(most, effects.size()); ++i)
            Log(std::format("TEXPACK blame {:+.2f} (at most {:.0f}) {}", effects[i].mean, effects[i].most,
                            effects[i].name));
    }

    // texpack drawn [part]: the images of the pack the last frame drew | texpack only <image>... | texpack only all |
    // texpack shared: images of the pack whose texture the game shares with another
    void DescribeTexturePackDraws(const std::string& what, const std::string& rest)
    {
        namespace Pack = Components::TexturePack;
        if (what == "drawn")
        {
            std::vector<std::string> drawn;
            RunOnRenderThread([&] { drawn = Pack::GetDrawnImages(); });
            Log(std::format("TEXPACK drawn {}", drawn.size()));
            for (const auto& name : drawn)
                if (rest.empty() || name.find(rest) != std::string::npos)
                    Log("TEXPACK drawn " + name);
        }
        else if (what == "shared")
        {
            std::vector<std::vector<std::string>> shared;
            RunOnRenderThread([&] { shared = Pack::GetSharedImages(); });
            Log(std::format("TEXPACK shared {} textures", shared.size()));
            for (const auto& names : shared)
            {
                std::string line;
                for (std::size_t i = 0; i < std::min<std::size_t>(names.size(), 8); ++i)
                    line += " " + names[i];
                Log(std::format("TEXPACK shared by {}:{}", names.size(), line));
            }
        }
        else if (rest == "all")
            RunOnRenderThread([] { Pack::SetSwappedImages(std::nullopt); });
        else
        {
            std::set<std::string> names;
            std::istringstream list(rest);
            for (std::string name; list >> name;)
                names.insert(name);
            RunOnRenderThread([&] { Pack::SetSwappedImages(names); });
        }
    }

    // texpack material <part>: the materials with an image whose name has it: their technique set, images (the
    // semantic, the sampler's bytes) and constants
    void DescribeTexturePackMaterials(const std::string& part)
    {
        std::vector<std::string> lines;
        RunOnMainThread([&] {
            const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
            const auto entries =
                reinterpret_cast<const Structures::XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
            for (std::size_t hash = 0; hash < Structures::DB_HASH_TABLE_SIZE; ++hash)
                for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
                {
                    const auto& entry = entries[index];
                    if (entry.type != Structures::ASSET_TYPE_MATERIAL || !entry.header)
                        continue;
                    const auto material = static_cast<const Structures::Material*>(entry.header);
                    if (!material->name || !material->textureTable)
                        continue;
                    bool has = false;
                    for (std::uint8_t i = 0; i < material->textureCount; ++i)
                        if (const auto image = material->textureTable[i].image; image && image->name)
                        {
                            std::string lower = image->name;
                            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                                return static_cast<char>(std::tolower(c));
                            });
                            has = has || lower.find(part) != std::string::npos;
                        }
                    if (!has || lines.size() > 400)
                        continue;
                    lines.push_back(std::format("TEXPACK material {} set {} textures {} constants {}", material->name,
                                                material->techniqueSet ? material->techniqueSet->name : "none",
                                                material->textureCount, material->constantCount));
                    for (std::uint8_t i = 0; i < material->textureCount; ++i)
                    {
                        const auto& texture = material->textureTable[i];
                        lines.push_back(std::format(
                            "TEXPACK   image {:08X} semantic {} state {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
                            "{:02X} {}",
                            texture.nameHash, texture.semantic, static_cast<int>(texture.nameStart), static_cast<int>(texture.nameEnd),
                            texture.samplerState, texture._unk08[0], texture._unk08[1], texture._unk08[2],
                            texture._unk08[3], texture.image && texture.image->name ? texture.image->name : "none"));
                    }
                    for (std::uint8_t i = 0; material->constantTable && i < material->constantCount; ++i)
                    {
                        const auto& constant = material->constantTable[i];
                        lines.push_back(std::format("TEXPACK   constant {:.12s} {:.3f} {:.3f} {:.3f} {:.3f}",
                                                    constant.name, constant.literal[0], constant.literal[1],
                                                    constant.literal[2], constant.literal[3]));
                    }
                }
        });
        Log(std::format("TEXPACK material {}: {} lines", part, lines.size()));
        for (const auto& line : lines)
            Log(line);
    }

    bool RunTexturePackCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "texpack")
            return false;
        // Texture packs (Components::TexturePack):
        //   texpack use <folder>|default: the packs of a folder (v1, imported\<name>, use.json) in place of the mod's
        //   texpack on|off: the Look tab's Textures (Pack, Game)
        //   texpack budget <MB>|card: the video memory the pack may take, in place of the card's free memory less 1 GB
        //   texpack wait [s] | texpack settle [ms] | texpack expect drawn|game | texpack groups | texpack off <group>...|none|file
        //   texpack stress <MB> [default|managed] | texpack find <part> | texpack info <image>
        //   texpack pixels mark | texpack pixels changed|same|lit|unlit [x0 y0 x1 y1] | texpack space mark|expect <MB>
        //   texpack blame <x0 y0 x1 y1> [most] | texpack drawn [part] | texpack only <image>...|all | texpack shared
        //   texpack material <part>
        //   texpack: logs the pack matched, what the card holds, the line under the row and the address space
        namespace Pack = Components::TexturePack;
        std::string what;
        args >> what;
        std::string rest;
        std::getline(args >> std::ws, rest);
        std::istringstream more(rest);
        if (what == "use")
            UseTexturePack(more);
        else if (what == "on" || (what == "off" && rest.empty()))
            Pack::SetOn(what == "on");
        else if (what == "budget")
        {
            Pack::SetBudgetOverride(rest == "card" ? std::nullopt : std::optional(std::stoull(rest) << 20));
            Sleep(100);
        }
        else if (what == "wait")
            WaitForTexturePack(more);
        else if (what == "settle")
            SettleTexturePack(more);
        else if (what == "expect")
            ExpectTexturePack(more);
        else if (what == "groups" || what == "off")
            SetTexturePackGroups(what, more);
        else if (what == "stress")
            StressTexturePack(more);
        else if (what == "find" || what == "info")
            FindTexturePackImages(what, more);
        else if (what == "pixels")
            CompareTexturePackPixels(more);
        else if (what == "space")
            CheckTexturePackSpace(more);
        else if (what == "blame")
            BlameTexturePackImages(more);
        else if (what == "drawn" || what == "only" || what == "shared")
            DescribeTexturePackDraws(what, rest);
        else if (what == "material")
            DescribeTexturePackMaterials(rest);
        if (what != "expect" && what != "wait" && what != "settle" && what != "groups" && what != "off" && what != "pixels" &&
            what != "space" && what != "blame" && what != "drawn" && what != "only" && what != "shared" &&
            what != "material")
            Log("TEXPACK " + DescribeTexturePack());
        return true;
    }

    // the effects fxfill spawned and keeps
    std::vector<std::int32_t> filledEffects;

    // the free camera's FOV and tilt readouts over the picture
    bool RunReadoutsCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "readouts")
            return false;
        // readouts show: shows them as a scroll does, for 4 s; readouts: logs where they are and the picture;
        // readouts expect inside: PASS when both were drawn inside the picture
        std::string what, expected;
        args >> what >> expected;
        if (what == "show")
        {
            RunOnRenderThread([] { Components::FreeCamera::ShowReadouts(); });
            Log("READOUTS shown");
            return true;
        }
        std::array<std::array<ImVec2, 2>, 2> readouts{};
        ImVec2 position, size;
        float fontSize = 0.0f;
        RunOnRenderThread([&] {
            readouts = Components::FreeCamera::GetReadouts();
            const auto view = UI::UIManager::Get().GetUIComponent<UI::GameView>(UI::Component::GameView);
            position = view->GetViewportPosition();
            size = view->GetViewportSize();
            fontSize = ImGui::GetFontSize();
        });
        const auto inside = [&](const std::array<ImVec2, 2>& rect) {
            return rect[0].x >= position.x && rect[0].y >= position.y && rect[1].x <= position.x + size.x &&
                   rect[1].y <= position.y + size.y;
        };
        const auto text = std::format(
            "fov {:.0f},{:.0f} to {:.0f},{:.0f}; tilt {:.0f},{:.0f} to {:.0f},{:.0f}; picture {:.0f},{:.0f} size "
            "{:.0f}x{:.0f}; font {:.1f}",
            readouts[0][0].x, readouts[0][0].y, readouts[0][1].x, readouts[0][1].y, readouts[1][0].x, readouts[1][0].y,
            readouts[1][1].x, readouts[1][1].y, position.x, position.y, size.x, size.y, fontSize);
        if (what == "expect" && expected == "inside")
            Log(std::format("{} readouts inside the picture: {}",
                            inside(readouts[0]) && inside(readouts[1]) ? "PASS" : "FAIL", text));
        else
            Log("READOUTS " + text);
        return true;
    }

    bool RunMarkersCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "markers")
            return false;
        // markers clear: takes away the timeline's markers, as a film recorded again under a name keeps those its scene
        // had
        std::string what;
        args >> what;
        if (what != "clear")
            return false;
        std::size_t count = 0;
        RunOnRenderThread([&] {
            count = Components::Markers::GetAll().size();
            Components::Markers::Clear();
        });
        Log(std::format("MARKERS cleared {}", count));
        return true;
    }

    // commands that share a key (the Controls window's flag)
    bool RunCommandConflictsCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "conflicts")
            return false;
        // conflicts: logs each command a press of its keys could also run, with the keys, in the order Dispatch tries
        // them, whether each can run now and which runs (as the Controls window says it), then how many there are and
        // how many keys more than one has; conflicts all: every command's keys and where they work; conflicts expect
        // <commands> [<keys>]: PASS when that many commands share keys (and that many keys are shared); conflicts keys
        // <id> <keys>: PASS when the command's keys read that ("Shift+G", "G, H", "-" for none); conflicts reset: every
        // command's keys back to its defaults
        std::string what;
        args >> what;
        if (what == "reset")
        {
            RunOnRenderThread([] { UI::Commands::ResetAll(); });
            Log("CONFLICTS reset");
            return true;
        }
        if (what == "keys")
        {
            std::string id, expected, word;
            args >> id;
            while (args >> word)
                expected += (expected.empty() ? "" : " ") + word;
            std::string keys;
            RunOnRenderThread([&] {
                for (const auto chord : UI::Commands::GetChords(id))
                    keys += (keys.empty() ? "" : ", ") + UI::Commands::FormatChord(chord);
            });
            if (keys.empty())
                keys = "-";
            Log(std::format("{} conflicts keys {}: {} (expected {})", keys == expected ? "PASS" : "FAIL", id, keys,
                            expected));
            return true;
        }
        if (what == "words")
        {
            // conflicts words <id> <words>: PASS when the Controls window's tooltip says that of which runs
            std::string id, expected, word;
            args >> id;
            while (args >> word)
                expected += (expected.empty() ? "" : " ") + word;
            std::string words;
            RunOnRenderThread(
                [&] { words = UI::Commands::DescribeClash(id, UI::Commands::GetConflicts(id)); });
            Log(std::format("{} conflicts words {}: \"{}\"", words == expected ? "PASS" : "FAIL", id, words));
            return true;
        }
        std::vector<std::string> lines;
        std::size_t shared = 0;
        RunOnRenderThread([&] {
            shared = UI::Commands::CountSharedKeys();
            for (const auto& command : UI::Commands::GetAll())
            {
                if (what == "all")
                {
                    std::string keys;
                    for (const auto chord : UI::Commands::GetChords(command.id))
                        keys += (keys.empty() ? "" : ", ") + UI::Commands::FormatChord(chord);
                    if (!keys.empty())
                        Log(std::format("COMMAND {} [{}] {}: {}", command.id, keys, command.label,
                                        UI::Commands::DescribeContexts(command.contexts)));
                    continue;
                }
                const auto others = UI::Commands::GetConflicts(command.id);
                if (others.empty())
                    continue;
                const auto reason = UI::Commands::GetUnavailableReason(command.id);
                std::string line = std::format("{} [{}] {}:", command.id, UI::Commands::GetShortcut(command.id),
                                               reason ? "unavailable" : "available");
                for (const auto other : others)
                    line += std::format(" {} [{}]", other->id, UI::Commands::GetShortcut(other->id));
                lines.push_back(line + " \"" + UI::Commands::DescribeClash(command.id, others) + "\"");
            }
        });
        if (what == "all")
        {
            Log("CONFLICTS all listed");
            return true;
        }
        for (const auto& line : lines)
            Log("CONFLICT " + line);
        if (what == "expect")
        {
            std::size_t commands = 0, keys = 0;
            args >> commands;
            const bool anyKeys = !(args >> keys);
            Log(std::format("{} conflicts: {} commands share {} keys (expected {}{})",
                            lines.size() == commands && (anyKeys || shared == keys) ? "PASS" : "FAIL", lines.size(),
                            shared, commands, anyKeys ? "" : std::format(" and {}", keys)));
            return true;
        }
        Log(std::format("CONFLICTS {}, {} keys used twice", lines.size(), shared));
        return true;
    }

    bool RunFxFillCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "fxfill")
            return false;
        // fxfill <count> <effect> | fxfill clear: spawns an effect that many times in the world and keeps them (as the
        // rotor wash's dust was kept), to fill the game's 1,024 effects for the
        // note that they ran out; clear kills them
        std::string what, effect;
        args >> what >> effect;
        if (what == "clear")
        {
            RunOnMainThread([&] {
                for (const auto handle : filledEffects)
                    Functions::FX_KillEffect(0, handle);
            });
            Log(std::format("FXFILL cleared {}", filledEffects.size()));
            filledEffects.clear();
            return true;
        }
        const auto count = std::atoi(what.c_str());
        std::size_t spawned = 0;
        RunOnMainThread([&] {
            typedef std::int32_t(__cdecl * FX_SpawnEffect_t)(
                void* system, void* def, std::int32_t msecBegin, const float* origin, const float(*axis)[3],
                std::uint32_t dobjHandle, std::uint32_t bone, std::uint32_t sortOrder, std::uint32_t owner,
                std::uint32_t markEntnum, const float* offset);
            static constexpr float AXIS[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            static constexpr float NO_OFFSET[3] = {};
            const auto system = *reinterpret_cast<void**>(GetGameAddresses().fx_system());
            const auto def = ContentPacks::FindAsset(Structures::ASSET_TYPE_FX, effect.c_str());
            const auto cg = Structures::GetClientGlobals();
            if (!system || !def || !cg)
                return;
            for (int i = 0; i < count; ++i)
            {
                const float origin[3] = {static_cast<float>(i % 32) * 64.0f, static_cast<float>(i / 32) * 64.0f, 0.0f};
                if (const auto handle = reinterpret_cast<FX_SpawnEffect_t>(GetGameAddresses().FX_SpawnEffect())(
                        system, def, cg->time, origin, AXIS, 0x7FF, 0x1FF, 0xFF, 0xFFFF, Structures::ENTITYNUM_NONE,
                        NO_OFFSET))
                {
                    filledEffects.push_back(handle);
                    ++spawned;
                }
            }
        });
        Log(std::format("FXFILL spawned {} of {} {}", spawned, count, effect));
        return true;
    }

    bool RunDeathSuitsCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "deathsuits")
            return false;
        // deathsuits <actor> <tick|now|clip<index>> [expect <what it does> [<a part of the first suited death's
        // name>]]: logs what the deaths Die offers suit at the tick (the clip before the
        // death, the path's stretch while the walk plays it; clip<index>: 300 ms into that clip, or halfway through a
        // shorter one) and the deaths that suit it, then the others; with expect, PASS when the heading's words are
        // those (underscores for spaces: crouched, crouched,_walking) and the first suited death has the part in its
        // name
        std::string id, when, mode, words, first;
        args >> id >> when >> mode >> words >> first;
        const auto actor = Components::Actors::GetActor(ParseActorId(id));
        if (!actor)
        {
            Log("FAIL deathsuits: no such actor");
            return true;
        }
        std::uint32_t tick = Components::Playback::GetActorTick();
        if (when.starts_with("clip"))
        {
            const auto index = std::strtoul(when.c_str() + 4, nullptr, 10);
            if (index >= actor->clips.size())
            {
                Log("FAIL deathsuits: no such clip");
                return true;
            }
            const auto& clip = actor->clips[index];
            tick = clip.start + std::min(300u, clip.length / 2);
        }
        else if (!when.empty() && when != "now")
            tick = static_cast<std::uint32_t>(std::stoul(when));
        const auto candidates = Components::Actors::GetDeathCandidates(*actor, tick, 6);
        const auto list = [](const std::vector<std::string>& deaths) {
            std::string text;
            for (const auto& death : deaths)
                text += (text.empty() ? "" : ", ") + death;
            return text;
        };
        // (and the 16 a Hit's arrows go through first)
        const auto arrows = Components::Actors::GetDeathCandidates(*actor, tick, 16);
        const auto text = std::format("deathsuits {} at {}: Suit it {}: {} | others: {} | the Hit's arrows: {}",
                                      actor->name, tick, candidates.motion, list(candidates.suited),
                                      list(candidates.others), list(arrows.suited));
        if (mode != "expect")
        {
            Log("DEATHSUITS " + text);
            return true;
        }
        std::ranges::replace(words, '_', ' ');
        const bool passed = candidates.motion == words &&
                            (first.empty() || (!candidates.suited.empty() &&
                                               candidates.suited.front().find(first) != std::string::npos));
        Log(std::format("{} {} (expected {}{})", passed ? "PASS" : "FAIL", text, words,
                        first.empty() ? std::string() : ", first " + first));
        return true;
    }

    bool RunFxSpawnsCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "fxspawns")
            return false;
        // fxspawns <name part|-> [reset | expect <spawned at least> [<refused at most>] | live <at most>]: logs how many
        // effects with the part in their name spawned since the last reset, how many were refused (with the client's
        // effects all in use) and how many are in use now; reset starts the count again; with expect, PASS when at
        // least that many spawned and at most that many (0) were refused with the effects all in use; with live, when
        // at most that many are in use
        std::string part, mode;
        std::size_t spawned = 0, refused = 0;
        args >> part >> mode >> spawned >> refused;
        if (part == "-")
            part.clear();
        const auto counts = Hooks::Effects::CountSpawns(part, mode == "reset");
        Hooks::Effects::EffectCounts used;
        RunOnMainThread([&] { used = Hooks::Effects::CountEffects(); });
        std::size_t live = 0;
        for (const auto& [effect, count] : used.most)
            if (effect.find(part) != std::string::npos)
                live += count;
        const auto text = std::format("fxspawns {}: {} spawned, {} refused ({} with the effects all in use), {} in use",
                                      part.empty() ? "-" : part, counts.spawned, counts.refused, counts.full, live);
        if (mode == "live")
            Log(std::format("{} {} (expected at most {} in use)", live <= spawned ? "PASS" : "FAIL", text, spawned));
        else if (mode == "expect")
            Log(std::format("{} {} (expected at least {} spawned, at most {} refused with the effects all in use)",
                            counts.spawned >= spawned && counts.full <= refused ? "PASS" : "FAIL", text, spawned,
                            refused));
        else
            Log("FXSPAWNS " + text);
        return true;
    }

    bool RunFxWatchCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "fxwatch")
            return false;
        std::string what;
        args >> what;
        if (what == "peek")
        {
            // fxwatch peek <hex address> <rows>: the floats there, three a row
            std::uint32_t address = 0;
            int rows = 1;
            args >> std::hex >> address >> std::dec >> rows;
            for (int row = 0; row < rows; ++row)
                if (const auto floats = Hooks::Effects::Peek(address + row * 12))
                    Log(std::format("FXPEEK {:X}: {} {} {}", address + row * 12, (*floats)[0], (*floats)[1],
                                    (*floats)[2]));
            return true;
        }
        int count = 0;
        if (!what.empty() && (count = std::atoi(what.c_str())) > 0)
        {
            Hooks::Effects::WatchSpawns(count);
            Log(std::format("FXWATCH {}", count));
            return true;
        }
        for (const auto& spawn : Hooks::Effects::GetWatchedSpawns())
        {
            std::string text;
            for (const auto arg : spawn.args)
                text += std::format(" {:X}", arg);
            Log(std::format("FXSPAWN {}:{}{}", spawn.name, text,
                            spawn.pointed ? std::format(" | {:.1f} {:.1f} {:.1f}", (*spawn.pointed)[0],
                                                        (*spawn.pointed)[1], (*spawn.pointed)[2])
                                          : ""));
        }
        return true;
    }

    // selectevent <actor> <index>: selects an actor's event (by its index in time), which opens the Actors tab's Events
    // page on it (see RunKillstreakCommand for why it is apart)
    bool RunSelectEventCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "selectevent")
            return false;
        std::string which;
        std::size_t index = 0;
        args >> which >> index;
        const auto id = ParseActorId(which);
        const auto actor = Components::Actors::GetActor(id);
        if (!actor || index >= actor->events.size())
        {
            Log("FAIL selectevent: no such actor or event");
            return true;
        }
        Components::Actors::Select(id);
        Components::Actors::SelectEvent(actor->events[index].id);
        Log(std::format("SELECTEVENT actor {} event {} ({})", id, index, actor->events[index].id));
        return true;
    }

    // leftgun <actor> [expect held|free [tolerance]]: logs where a dual wield actor's left gun hangs (tag_weapon_left)
    // from its left wrist, and how far that is from the grip the dual wield animations hold it at
    // (ActorLogic::LEFT_GUN_GRIP_POSITION); PASS when it is within the tolerance (2 units) of the grip, or farther
    // (free: an animation of its own moves it, as a dual wield melee does)
    bool RunLeftGunCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "leftgun")
            return false;
        std::string which, word, expect;
        float tolerance = 2.0f;
        args >> which >> word >> expect >> tolerance;
        std::vector<std::optional<glm::mat4>> matrices;
        RunOnMainThread(
            [&] { matrices = Actors::GetBoneMatrices(ParseActorId(which), {"tag_weapon_left", "j_wrist_le"}); });
        if (matrices.size() < 2 || !matrices[0] || !matrices[1])
        {
            Log(std::format("FAIL leftgun {}: no such actor or bones", which));
            return true;
        }
        const auto local = glm::vec3(glm::inverse(*matrices[1]) * glm::vec4(glm::vec3((*matrices[0])[3]), 1.0f));
        const auto off = glm::distance(local, ActorLogic::LEFT_GUN_GRIP_POSITION);
        auto text = std::format("LEFTGUN {} from j_wrist_le ({:.1f} {:.1f} {:.1f}), {:.2f} from the grip", which,
                                local.x, local.y, local.z, off);
        if (expect == "held")
            text = std::format("{} {}", off <= tolerance ? "PASS" : "FAIL", text);
        else if (expect == "free")
            text = std::format("{} {}", off > tolerance ? "PASS" : "FAIL", text);
        Log(text);
        return true;
    }

    // the game's IK state of an entity (see grip), null for none
    const std::uint8_t* FindIkState(std::int32_t entity)
    {
        const auto states = *reinterpret_cast<const std::uint8_t* const*>(0x3A73978);
        return states && entity >= 0 && entity < 32 ? states + entity * 0xE70 : nullptr;
    }

    // where an entity's bone is
    std::optional<glm::mat4> GetEntityBone(std::int32_t entity, const char* tag)
    {
        const auto obj = Functions::Com_GetClientDObj(entity, 0);
        float axis[3][3], origin[3];
        if (!obj || !Functions::CG_DObjGetWorldTagMatrix(Functions::CG_GetPose(0, entity), obj,
                                                         Functions::SL_GetString(tag), axis, origin))
            return std::nullopt;
        glm::mat4 m(1.0f);
        for (int c = 0; c < 3; ++c)
            m[c] = glm::vec4(axis[c][0], axis[c][1], axis[c][2], 0.0f);
        m[3] = glm::vec4(origin[0], origin[1], origin[2], 1.0f);
        return m;
    }

    // what a player plays and how it stands at a moment, from its origin along its facing (x ahead, y left, z up)
    struct PlayerPoseSample
    {
        std::string legs;
        float phase = 0.0f;
        glm::vec3 origin{0.0f};
        float yaw = 0.0f;
        std::map<std::string, glm::vec3> bones;
    };
    std::map<std::string, std::vector<PlayerPoseSample>> playerPoses;

    std::optional<PlayerPoseSample> TakePlayerPose(std::int32_t client)
    {
        const auto cg = Structures::GetClientGlobals();
        const auto entities = Structures::GetEntities();
        if (!cg || !entities || !cg->nextSnap || !cg->animScriptData)
            return std::nullopt;
        const bool own = cg->nextSnap->ps.clientNum == client;
        auto& cent = own ? cg->predictedPlayerEntity : entities[client];
        const auto pose = reinterpret_cast<Structures::cpose_t*>(&cent);
        const auto obj = Functions::Com_GetClientDObj(client, 0);
        if (!obj || !obj->tree)
            return std::nullopt;
        const auto legs = cent.nextState.legsAnim & ~static_cast<std::uint32_t>(Structures::ANIM_TOGGLEBIT);
        const auto& data = *cg->animScriptData;

        PlayerPoseSample sample;
        sample.legs = legs < data.numAnimations ? data.animations[legs].name : "?";
        sample.phase = Functions::XAnimGetTime(obj->tree, legs);
        sample.origin = glm::vec3(pose->origin[0], pose->origin[1], pose->origin[2]);
        sample.yaw = pose->angles[1];
        const auto yaw = glm::radians(sample.yaw);
        const glm::vec2 ahead(std::cos(yaw), std::sin(yaw)), left(-ahead.y, ahead.x);
        for (const auto tag : {"j_mainroot", "j_spine4", "j_head", "tag_weapon_right", "j_wrist_le", "j_ankle_le"})
        {
            float axis[3][3], at[3];
            if (!Functions::CG_DObjGetWorldTagMatrix(pose, obj, Functions::SL_GetString(tag), axis, at))
                continue;
            const auto offset = glm::vec3(at[0], at[1], at[2]) - sample.origin;
            sample.bones[tag] = {glm::dot(glm::vec2(offset), ahead), glm::dot(glm::vec2(offset), left), offset.z};
        }
        return sample;
    }

    // playerpose log <client> <samples> [<ms>]: logs what a player plays and where its bones are; playerpose record
    // <label> <client> <ms>: keeps that for every frame of a stretch of the demo; playerpose same <a> <b> <distance>:
    // PASS when a player recorded twice stands the same at the same moments of the demo (the same animation at the same
    // time at the same place), within that distance on average, for each bone; playerpose bodies on|off: off, players'
    // campaign bodies get the spine rig as they did (for a look with the fix off); ikstate <client> [expect 0|1]: logs the
    // game's IK state of a player, PASS when it is valid or not: all of its bones in the DObj's first model; ikstate times
    // [expect]: each player's times the IK keeps (its gun's last change, its last move and stand), from its state's time,
    // PASS when none is ahead of it; ikstate forget on|off: off, a rewind leaves those times as they were
    bool RunIkTimesCommand(const std::string& what, std::istringstream& args)
    {
        std::string word;
        args >> word;
        if (what == "forget")
        {
            RunOnMainThread([&] { Hooks::PlayerAnimation::SetIkTimesForgotten(word != "off"); });
            Log("IKSTATE forget " + word);
            return true;
        }
        const bool expect = word == "expect";
        std::string text;
        std::size_t ahead = 0, active = 0;
        RunOnMainThread([&] {
            const auto states = Structures::GetIKStates();
            for (std::size_t i = 0; states && i < Structures::MAX_CLIENTS; ++i)
            {
                const auto& state = states[i];
                if (!state.active)
                    continue;
                ++active;
                const auto gun = state.gripChangeTime - state.time;
                const auto moved = state.lastMoveTime - state.time;
                const auto still = state.lastStillTime - state.time;
                if (gun > 0 || moved > 0 || still > 0)
                    ++ahead;
                text += std::format(" {}:{},{},{}", state.entityNum, state.gripChangeTime > 1 ? gun : 0,
                                    state.lastMoveTime ? moved : 0, state.lastStillTime ? still : 0);
            }
        });
        Log(std::format("{}IKSTATE times of {} players, {} ahead (gun change, move, stand from now):{}",
                        expect ? (active && !ahead ? "PASS " : "FAIL ") : "", active, ahead, text));
        return true;
    }

    bool RunPlayerPoseCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "ikstate")
        {
            std::string first, word;
            args >> first;
            if (first == "times" || first == "forget")
                return RunIkTimesCommand(first, args);
            const auto client = static_cast<std::int32_t>(std::strtol(first.c_str(), nullptr, 10));
            std::optional<std::int32_t> expected;
            args >> word;
            if (word == "expect")
                args >> expected.emplace();
            std::int32_t valid = -1;
            RunOnMainThread([&] {
                if (const auto state = FindIkState(client))
                    valid = *reinterpret_cast<const std::int32_t*>(state + 0x14);
            });
            Log(std::format("{}IKSTATE {} valid {}", expected ? (valid == *expected ? "PASS " : "FAIL ") : "", client, valid));
            return true;
        }
        if (name != "playerpose")
            return false;
        std::string what;
        args >> what;
        if (what == "log")
        {
            std::int32_t client = 0, samples = 1, step = 50;
            args >> client >> samples >> step;
            for (std::int32_t i = 0; i < samples; ++i)
            {
                RunOnMainThread([&] {
                    const auto sample = TakePlayerPose(client);
                    if (!sample)
                        return;
                    std::string text = std::format("PLAYERPOSE {} {:.3f} at {:.1f} {:.1f} {:.1f} yaw {:.1f}", sample->legs,
                                                   sample->phase, sample->origin.x, sample->origin.y, sample->origin.z,
                                                   sample->yaw);
                    for (const auto& [bone, at] : sample->bones)
                        text += std::format(" {} {:.1f},{:.1f},{:.1f}", bone, at.x, at.y, at.z);
                    Log(text);
                });
                Sleep(step);
            }
            return true;
        }
        if (what == "bodies")
        {
            std::string state;
            args >> state;
            RunOnMainThread([&] { Actors::SetPlayerBodiesOn(state != "off"); });
            Log("PLAYERPOSE bodies " + state);
            return true;
        }
        if (what == "record")
        {
            std::string label;
            std::int32_t client = 0, ms = 1000;
            args >> label >> client >> ms;
            auto& samples = playerPoses[label];
            samples.clear();
            const auto end = GetTickCount() + static_cast<DWORD>(ms);
            while (GetTickCount() < end)
                RunOnMainThread([&] {
                    if (const auto sample = TakePlayerPose(client))
                        samples.push_back(*sample);
                });
            Log(std::format("PLAYERPOSE {} {} samples", label, samples.size()));
            return true;
        }
        if (what == "same")
        {
            std::string a, b;
            float distance = 0.0f;
            args >> a >> b >> distance;
            std::map<std::string, std::pair<float, std::uint32_t>> sums;
            std::uint32_t matched = 0;
            for (const auto& sample : playerPoses[b])
            {
                const PlayerPoseSample* found = nullptr;
                float best = 0.004f;
                for (const auto& other : playerPoses[a])
                {
                    const auto phase = std::abs(other.phase - sample.phase);
                    if (other.legs == sample.legs && phase < best && glm::distance(other.origin, sample.origin) < 8.0f)
                    {
                        best = phase;
                        found = &other;
                    }
                }
                if (!found)
                    continue;
                ++matched;
                for (const auto& [bone, at] : sample.bones)
                    if (const auto there = found->bones.find(bone); there != found->bones.end())
                    {
                        sums[bone].first += glm::distance(at, there->second);
                        ++sums[bone].second;
                    }
            }
            bool same = matched >= 10;
            std::string text;
            for (const auto& [bone, sum] : sums)
            {
                const auto mean = sum.first / static_cast<float>(sum.second);
                same = same && mean <= distance;
                text += std::format(" {} {:.2f}", bone, mean);
            }
            Log(std::format("{} playerpose {} and {}: {} moments{}", same ? "PASS" : "FAIL", a, b, matched, text));
            return true;
        }
        return false;
    }

    // a bone of an actor's left arm on its gun, or of a player's
    std::optional<glm::mat4> GetBoneOnGun(const std::function<std::optional<glm::mat4>(const char*)>& bone, const char* tag)
    {
        const auto gun = bone("tag_weapon_right");
        const auto found = bone(tag);
        return gun && found ? std::optional(glm::inverse(*gun) * *found) : std::nullopt;
    }

    std::function<std::optional<glm::mat4>(const char*)> ActorBones(const std::string& which)
    {
        return [id = ParseActorId(which)](const char* tag) -> std::optional<glm::mat4> {
            const auto matrices = Actors::GetBoneMatrices(id, {tag});
            return matrices.empty() ? std::nullopt : matrices[0];
        };
    }

    // gripsurvey <list file> <out file> <body> [samples]: where the animations of the list keep the hands on
    // tag_weapon_right, decoded on the body: a line per sample, with the left wrist's
    // place and axes and the right wrist's place, for tools to sum up
    bool RunGripSurveyCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "gripsurvey")
            return false;
        std::string list, out, body;
        int samples = 5;
        args >> list >> out >> body >> samples;
        std::ifstream in(list);
        std::ofstream file(out);
        const auto game = Mod::GetGameInterface();
        std::size_t animations = 0, missing = 0;
        for (std::string animation; std::getline(in, animation);)
        {
            AnimationStore::Found found;
            RunOnMainThread([&] { found = AnimationStore::Find(animation); });
            bool any = false;
            for (int i = 0; i < samples && found.parts; ++i)
            {
                const auto time = static_cast<float>(i) / static_cast<float>(samples - 1);
                auto pose = game->GetAnimationPoseByName(animation, body, time);
                for (int tries = 0; !pose && tries < 3; ++tries)
                {
                    Sleep(20);
                    pose = game->GetAnimationPoseByName(animation, body, time);
                }
                if (!pose)
                    continue;
                const auto gunBone = pose->FindBone("tag_weapon_right"), left = pose->FindBone("j_wrist_le"),
                           right = pose->FindBone("j_wrist_ri");
                if (!gunBone || !left || !right)
                    continue;
                const auto gun = glm::inverse(PoseLogic::GetTransform(*pose, *gunBone));
                const auto wrist = gun * PoseLogic::GetTransform(*pose, *left);
                const auto other = gun * PoseLogic::GetTransform(*pose, *right);
                file << std::format("{} {:.2f} {:.2f} {:.2f} {:.2f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} "
                                    "{:.2f} {:.2f} {:.2f}\n",
                                    animation, time, wrist[3][0], wrist[3][1], wrist[3][2], wrist[0][0], wrist[0][1],
                                    wrist[0][2], wrist[1][0], wrist[1][1], wrist[1][2], other[3][0], other[3][1],
                                    other[3][2]);
                any = true;
            }
            ++animations;
            missing += any ? 0 : 1;
            if (animations % 500 == 0)
                Log(std::format("GRIPSURVEY {} done, {} not decoded", animations, missing));
        }
        Log(std::format("GRIPSURVEY {} done, {} not decoded", animations, missing));
        return true;
    }

    // wrists on the gun recorded by grip mark, by label
    std::map<std::string, glm::vec3> gripMarks;

    // grip weapons [name...] | grip ik <0|1> | grip <on|off> | grip entity <number> | grip actor <actor> | grip rest
    // <model>... | grip gun <actor> <entity> | grip match <actor> <entity> <distance> [ms] | grip mark <label> <actor> |
    // grip moved <label> <actor> <x y z> <distance> | grip hand <actor> <x y z> <distance> | grip gunhand <actor> <x y z>
    // <distance>: the left hand's grip per gun. weapons: every gun's ikLeftHand fields;
    // ik: the game's own IK on or off; on, off: actors' grips; entity and actor: where the left arm is from
    // tag_weapon_right; gun: gives an actor the gun a player has; match: PASS when the actor's left wrist is that close
    // to the player's on the gun, its axes too (the elbow, logged, goes with the body's pose, which needn't be the
    // player's); mark, moved: PASS when the wrist on the gun moved that much
    // since it was marked; hand: PASS when the wrist is there on the gun, and gunhand when the right one is
    // grip clip <actor> <index> [expect on|off]: logs a clip's Left hand (ActorClip::grip), or PASS when it is so
    void RunGripClipCommand(std::istringstream& args)
    {
        std::string which, word, expected;
        std::size_t index = 0;
        args >> which >> index >> word >> expected;
        RunOnMainThread([&] {
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || index >= actor->clips.size())
            {
                Log(std::format("FAIL grip clip: actor {} has no clip {}", which, index));
                return;
            }
            const auto& clip = actor->clips[index];
            const auto state = clip.grip ? "on" : "off";
            const auto text = std::format("grip clip {} {} {}: {}", which, index, clip.animation, state);
            Log(word == "expect" ? std::format("{} {}", expected == state ? "PASS" : "FAIL", text) : "GRIP" + text.substr(4));
        });
    }

    bool RunGripCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "grip")
            return false;
        std::string what;
        args >> what;
        if (what == "clip")
        {
            RunGripClipCommand(args);
            return true;
        }
        if (what == "on" || what == "off")
        {
            RunOnMainThread([&] { Actors::SetGripsOn(what == "on"); });
            Log("GRIP " + what);
            return true;
        }
        if (what == "places")
        {
            std::string mode;
            args >> mode;
            RunOnMainThread([&] { Actors::SetGripPlaces(mode != "off"); });
            Log("GRIP places " + mode);
            return true;
        }
        if (what == "rig")
        {
            // one line per model of an actor: the arm's lengths and the gun tag's rest, from what its bones say
            std::string which;
            args >> which;
            RunOnMainThread([&] {
                const auto actor = Components::Actors::GetActor(ParseActorId(which));
                if (!actor)
                    return;
                for (const auto& name : actor->models)
                {
                    const auto model = static_cast<const Structures::XModel*>(
                        ContentPacks::FindAsset(Structures::ASSET_TYPE_XMODEL, name.c_str()));
                    if (!model)
                    {
                        Log(std::format("GRIP rig {} {} not loaded", which, name));
                        continue;
                    }
                    const auto bones = AnimationPose::GetRestPose(*model);
                    const auto length = [&](const char* tag) {
                        const auto bone = AnimationPose::FindBone(*model, tag);
                        if (bone < 0 || bones.parents[bone] < 0)
                            return -1.0f;
                        return glm::distance(bones.positions[bone], bones.positions[bones.parents[bone]]);
                    };
                    const auto gun = AnimationPose::FindBone(*model, "tag_weapon_right");
                    if (gun < 0 || AnimationPose::FindBone(*model, "j_wrist_le") < 0)
                        continue;
                    const auto parent = bones.parents[gun];
                    const auto rest = glm::inverse(bones.rotations[parent]) * (bones.positions[gun] - bones.positions[parent]);
                    Log(std::format("GRIP rig {} {} bones {} clavicle {:.2f} shoulder {:.2f} elbow {:.2f} wrist {:.2f} tag on {} "
                                    "{:.2f} {:.2f} {:.2f}", which, name, model->numBones, length("j_clavicle_le"),
                                    length("j_shoulder_le"), length("j_elbow_le"), length("j_wrist_le"),
                                    bones.names[parent], rest.x, rest.y, rest.z));
                }
            });
            return true;
        }
        if (what == "scan")
        {
            // one line of how the game's left hand stands on each player: its grip's weight, the fade and channel 7
            RunOnMainThread([&] {
                std::string text;
                for (std::int32_t entity = 0; entity < 32; ++entity)
                {
                    const auto state = FindIkState(entity);
                    if (!state || !*reinterpret_cast<const std::int32_t*>(state) || !Functions::Com_GetClientDObj(entity, 0))
                        continue;
                    const auto f = [&](std::size_t at) { return *reinterpret_cast<const float*>(state + at); };
                    const auto wrist = GetBoneOnGun([&](const char* tag) { return GetEntityBone(entity, tag); }, "j_wrist_le");
                    const auto hand = wrist ? glm::vec3((*wrist)[3]) + 2.3f * glm::vec3((*wrist)[0]) : glm::vec3(0.0f);
                    text += std::format(" {}:{:.2f},{:.2f},{:.2f},{},{:.1f}", entity, f(0x3B8 + 11 * 12 + 4), f(0xE00),
                                        f(0x3B8 + 7 * 12 + 4), state[0x600], glm::distance(hand, glm::vec3(-4.0f, 0.1f, -3.4f)));
                }
                Log("GRIP scan" + text);
            });
            return true;
        }
        if (what == "gun")
        {
            std::string which;
            std::int32_t entity = 0;
            args >> which >> entity;
            RunOnMainThread([&] {
                const auto entities = Structures::GetEntities();
                auto actor = Components::Actors::GetActor(ParseActorId(which));
                const auto variants =
                    reinterpret_cast<Structures::WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
                const auto weapon = entities ? entities[entity].nextState.weapon : 0;
                if (!actor || !weapon || !variants[weapon]->szInternalName)
                {
                    Log(std::format("FAIL grip gun: actor {} entity {}", which, entity));
                    return;
                }
                // (ak47_elbit_silencer_mp is ak47_mp with those attachments, as the Actors tab has it)
                std::vector<std::string> words;
                std::istringstream name(variants[weapon]->szInternalName);
                for (std::string word; std::getline(name, word, '_');)
                    words.push_back(word);
                actor->weapon = words.front() + "_mp";
                actor->attachments.assign(words.begin() + 1, words.end() - 1);
                std::ranges::sort(actor->attachments);
                actor->camo = 0;
                Components::Actors::Update(*actor);
                if (Components::Actors::ResolveWeapon(*actor) != variants[weapon]->szInternalName)
                {
                    actor->weapon = variants[weapon]->szInternalName;
                    actor->attachments.clear();
                    Components::Actors::Update(*actor);
                }
                Log(std::format("GRIP gun actor {} holds {}", which, Components::Actors::ResolveWeapon(*actor)));
            });
            return true;
        }
        if (what == "match")
        {
            std::string which;
            std::int32_t entity = 0, ms = 0;
            float tolerance = 0.1f;
            args >> which >> entity >> tolerance >> ms;
            // (the game runs a player's IK only as it draws it, so with a time this looks until a frame has it)
            std::string text;
            bool matched = false;
            for (std::int32_t waited = 0;; waited += 100)
            {
                RunOnMainThread([&] {
                    const auto player = [&](const char* tag) { return GetEntityBone(entity, tag); };
                    const auto mine = ActorBones(which);
                    const auto wrist = GetBoneOnGun(mine, "j_wrist_le"), wristThere = GetBoneOnGun(player, "j_wrist_le");
                    const auto elbow = GetBoneOnGun(mine, "j_elbow_le"), elbowThere = GetBoneOnGun(player, "j_elbow_le");
                    if (!wrist || !wristThere || !elbow || !elbowThere)
                    {
                        text = std::format("actor {} entity {} has no bones", which, entity);
                        return;
                    }
                    const auto wristOff = PoseLogic::GetTransformDistance(*wrist, *wristThere, 4.0f);
                    matched = wristOff <= tolerance;
                    text = std::format("actor {} entity {}: wrist {:.2f} (hand {:.2f}), elbow {:.2f}", which, entity,
                                       glm::distance(glm::vec3((*wrist)[3]), glm::vec3((*wristThere)[3])), wristOff,
                                       glm::distance(glm::vec3((*elbow)[3]), glm::vec3((*elbowThere)[3])));
                });
                if (matched || waited >= ms)
                    break;
                Sleep(100);
            }
            Log(std::format("{} grip match {}", matched ? "PASS" : "FAIL", text));
            return true;
        }
        if (what == "entitymoved")
        {
            // grip entitymoved <entity> <x> <y> <z> <tolerance> [ms]: PASS when a film's player has its left wrist that
            // far from the animations' spot on its gun (GripLogic::PLAYER_WRIST): its gun's grip, where the game's IK
            // moves it. With a time it looks for a frame with it, the demo playing, as the IK runs as it draws
            std::int32_t entity = 0, ms = 0;
            glm::vec3 expected(0.0f);
            float tolerance = 0.2f;
            args >> entity >> expected.x >> expected.y >> expected.z >> tolerance >> ms;
            std::string text;
            bool matched = false;
            for (std::int32_t waited = 0;; waited += 100)
            {
                RunOnMainThread([&] {
                    const auto wrist =
                        GetBoneOnGun([&](const char* tag) { return GetEntityBone(entity, tag); }, "j_wrist_le");
                    if (!wrist)
                    {
                        text = std::format("entity {} has no bones", entity);
                        return;
                    }
                    const auto moved = glm::vec3((*wrist)[3]) - GripLogic::PLAYER_WRIST;
                    const auto off = glm::distance(moved, expected);
                    matched = off <= tolerance;
                    text = std::format("entity {} {:.2f} {:.2f} {:.2f}, {:.2f} from {:.2f} {:.2f} {:.2f}", entity, moved.x,
                                       moved.y, moved.z, off, expected.x, expected.y, expected.z);
                });
                if (matched || waited >= ms)
                    break;
                Sleep(100);
            }
            Log(std::format("{} grip entitymoved {}", matched ? "PASS" : "FAIL", text));
            return true;
        }
        if (what == "axes")
        {
            // grip axes <actor> <x axis> <y axis> [tolerance]: the left wrist's axes on the gun
            std::string which;
            glm::vec3 xAxis(0.0f), yAxis(0.0f);
            float tolerance = 0.02f;
            args >> which >> xAxis.x >> xAxis.y >> xAxis.z >> yAxis.x >> yAxis.y >> yAxis.z >> tolerance;
            RunOnMainThread([&] {
                const auto bone = GetBoneOnGun(ActorBones(which), "j_wrist_le");
                if (!bone)
                {
                    Log(std::format("FAIL grip axes: actor {} has no bones", which));
                    return;
                }
                const auto x = glm::vec3((*bone)[0]), y = glm::vec3((*bone)[1]);
                const auto off = std::max(glm::distance(x, xAxis), glm::distance(y, yAxis));
                Log(std::format("{} grip axes {} x {:.3f} {:.3f} {:.3f} y {:.3f} {:.3f} {:.3f}, {:.3f} off",
                                off <= tolerance ? "PASS" : "FAIL", which, x.x, x.y, x.z, y.x, y.y, y.z, off));
            });
            return true;
        }
        if (what == "mark" || what == "moved" || what == "hand" || what == "gunhand")
        {
            std::string label, which;
            if (what == "mark" || what == "moved")
                args >> label;
            args >> which;
            glm::vec3 expected(0.0f);
            float tolerance = 0.1f;
            args >> expected.x >> expected.y >> expected.z >> tolerance;
            RunOnMainThread([&] {
                const auto bone = GetBoneOnGun(ActorBones(which), what == "gunhand" ? "j_wrist_ri" : "j_wrist_le");
                if (!bone)
                {
                    Log(std::format("FAIL grip {}: actor {} has no bones", what, which));
                    return;
                }
                const auto at = glm::vec3((*bone)[3]);
                if (what == "mark")
                {
                    gripMarks[label] = at;
                    Log(std::format("GRIP mark {} {:.2f} {:.2f} {:.2f}", label, at.x, at.y, at.z));
                    return;
                }
                const auto got = what == "moved" ? at - gripMarks[label] : at;
                const auto off = glm::distance(got, expected);
                Log(std::format("{} grip {} {} {:.2f} {:.2f} {:.2f}, {:.2f} from {:.2f} {:.2f} {:.2f}",
                                off <= tolerance ? "PASS" : "FAIL", what, which, got.x, got.y, got.z, off, expected.x,
                                expected.y, expected.z));
            });
            return true;
        }
        if (what == "weapons")
        {
            std::set<std::string> names;
            for (std::string word; args >> word;)
                names.insert(word);
            RunOnMainThread([&] {
                const auto variants =
                    reinterpret_cast<Structures::WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
                for (std::size_t i = 1; i < Functions::GetWeaponVariantCount(); ++i)
                {
                    const auto variant = variants[i];
                    if (!variant || !variant->szInternalName || !variant->weapDef ||
                        (!names.empty() && !names.contains(variant->szInternalName)))
                        continue;
                    const auto& o = variant->ikLeftHandOffset;
                    const auto& r = variant->ikLeftHandRotation;
                    const auto& po = variant->ikLeftHandProneOffset;
                    const auto& pr = variant->ikLeftHandProneRotation;
                    const auto def = reinterpret_cast<const std::int32_t*>(variant->weapDef);
                    Log(std::format("GRIP {} class {} type {} anim {} inv {} flag68 {} off {:.2f} {:.2f} {:.2f} rot {:.2f} "
                                    "{:.2f} {:.2f} prone {:.2f} {:.2f} {:.2f} rot {:.2f} {:.2f} {:.2f}",
                                    variant->szInternalName, variant->weapDef->weapClass, variant->weapDef->weapType,
                                    variant->weapDef->playerAnimType, def[0x2C / 4], def[0x68 / 4], o[0], o[1], o[2],
                                    r[0], r[1], r[2], po[0], po[1], po[2], pr[0], pr[1], pr[2]));
                }
            });
            return true;
        }
        if (what == "ik")
        {
            int on = 1;
            args >> on;
            RunOnMainThread([&] {
                const auto dvar = Functions::FindDvar("ik_enable");
                if (dvar)
                    *reinterpret_cast<bool*>(&dvar->current) = on != 0;
                Log(std::format("GRIP ik {}", dvar ? on : -1));
            });
            return true;
        }
        if (what == "rest")
        {
            std::vector<std::string> models;
            for (std::string word; args >> word;)
                models.push_back(word);
            RunOnMainThread([&] {
                for (const auto& name : models)
                {
                    const auto model = static_cast<const Structures::XModel*>(
                        ContentPacks::FindAsset(Structures::ASSET_TYPE_XMODEL, name.c_str()));
                    if (!model)
                    {
                        Log(std::format("GRIP rest {} not loaded", name));
                        continue;
                    }
                    const auto pose = AnimationPose::GetRestPose(*model);
                    const auto parentOf = [&](std::int32_t bone) {
                        return bone < 0 || pose.parents[bone] < 0 ? std::string("-") : pose.names[pose.parents[bone]];
                    };
                    Log(std::format("GRIP rest {} bones {} roots {}", name, model->numBones, model->numRootBones));
                    for (const auto bone_name :
                         {"j_mainroot", "j_spine4", "j_clavicle_le", "j_shoulder_le", "j_elbow_le", "j_wrist_le",
                          "j_wristtwist_le", "j_clavicle_ri", "j_shoulder_ri", "j_elbow_ri", "j_wrist_ri",
                          "tag_weapon_right", "tag_weapon_left", "tag_inhand", "tag_flash"})
                    {
                        const auto bone = AnimationPose::FindBone(*model, bone_name);
                        if (bone < 0)
                            continue;
                        const auto world = pose.positions[bone];
                        const auto q = pose.rotations[bone];
                        const auto parent = pose.parents[bone];
                        const auto local = parent >= 0 ? glm::inverse(pose.rotations[parent]) * (world - pose.positions[parent])
                                                       : world;
                        const auto turn = parent >= 0 ? glm::inverse(pose.rotations[parent]) * q : q;
                        Log(std::format("GRIP rest {} {} on {} from it {:.3f} {:.3f} {:.3f} q {:.4f} {:.4f} {:.4f} {:.4f} model "
                                        "{:.2f} {:.2f} {:.2f} q {:.4f} {:.4f} {:.4f} {:.4f}",
                                        name, bone_name, parentOf(bone), local.x, local.y, local.z, turn.x, turn.y, turn.z,
                                        turn.w, world.x, world.y, world.z, q.x, q.y, q.z, q.w));
                    }
                }
            });
            return true;
        }
        const auto report = [&](const std::string& label, const auto& bone, const std::uint8_t* state) {
            const auto gun = bone("tag_weapon_right");
            if (!gun)
            {
                Log(std::format("GRIP {} no gun tag", label));
                return;
            }
            const auto inverse = glm::inverse(*gun);
            auto text = std::format("GRIP {}", label);
            for (const auto bone_name : {"j_clavicle_le", "j_shoulder_le", "j_elbow_le", "j_wrist_le", "j_wrist_ri"})
                if (const auto m = bone(bone_name))
                {
                    const auto local = inverse * *m;
                    text += std::format(" {} {:.2f} {:.2f} {:.2f}", bone_name + 2, local[3][0], local[3][1], local[3][2]);
                }
            if (const auto m = bone("j_wrist_le"))
            {
                const auto local = inverse * *m;
                const auto hand = glm::vec3(local[3]) + 2.3f * glm::vec3(local[0]);
                text += std::format(" | hand {:.2f} {:.2f} {:.2f} x {:.3f} {:.3f} {:.3f} y {:.3f} {:.3f} {:.3f}", hand.x,
                                    hand.y, hand.z, local[0][0], local[0][1], local[0][2], local[1][0], local[1][1],
                                    local[1][2]);
            }
            for (const auto bone_name : {"j_shoulder_le", "j_elbow_le", "j_wristtwist_le"})
                if (const auto m = bone(bone_name))
                {
                    const auto local = inverse * *m;
                    text += std::format(" | {} x {:.3f} {:.3f} {:.3f} z {:.3f} {:.3f} {:.3f}", bone_name + 2, local[0][0],
                                        local[0][1], local[0][2], local[2][0], local[2][1], local[2][2]);
                }
            Log(text);
            if (!state)
                return;
            const auto f = [&](std::size_t at) { return *reinterpret_cast<const float*>(state + at); };
            const auto i = [&](std::size_t at) { return *reinterpret_cast<const std::int32_t*>(state + at); };
            const auto b = [&](std::size_t at) { return static_cast<int>(state[at]); };
            std::string weights;
            for (std::size_t channel = 0; channel < 22; ++channel)
                weights += std::format(" {:.2f}", f(0x3B8 + channel * 12 + 4));
            Log(std::format("GRIP {} state active {} ent {} server {} on {} {} mode {} kind {} fade {:.3f} hands {} feet {} "
                            "grip {:.2f} {:.2f} {:.2f} weights{}",
                            label, i(0), i(4), b(8), b(0x600), b(0x620), i(0x640), i(0xE04), f(0xE00), b(0xE5D), b(0xE5C),
                            f(0xDCC), f(0xDD0), f(0xDD4), weights));
        };
        if (what == "entity")
        {
            std::int32_t entity = 0;
            args >> entity;
            RunOnMainThread([&] {
                const auto obj = Functions::Com_GetClientDObj(entity, 0);
                const auto entities = Structures::GetEntities();
                if (!obj || !entities)
                {
                    Log(std::format("GRIP entity {} none", entity));
                    return;
                }
                const auto bone = [&](const char* tag) { return GetEntityBone(entity, tag); };
                const auto& es = entities[entity].nextState;
                const auto variants =
                    reinterpret_cast<Structures::WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
                const auto variant = es.weapon ? variants[es.weapon] : nullptr;
                const auto pose = Functions::CG_GetPose(0, entity);
                const auto number = es.legsAnim & ~Structures::ANIM_TOGGLEBIT;
                const auto& anims = Components::PlayerAnimation::GetAnimations();
                const auto named = std::ranges::find(anims, number, &std::pair<std::string_view, std::uint32_t>::second);
                Log(std::format("GRIP entity {} at {:.1f} {:.1f} {:.1f} yaw {:.1f} anim {} {}", entity, pose->origin[0],
                                pose->origin[1], pose->origin[2], pose->angles[1], number,
                                named != anims.end() ? std::string(named->first) : "?"));
                report(std::format("entity {} {} anim {}", entity, variant ? variant->szInternalName : "none", number), bone,
                       FindIkState(entity));
            });
            return true;
        }
        if (what == "actor")
        {
            std::string which;
            args >> which;
            RunOnMainThread([&] {
                report(std::format("actor {}", which), ActorBones(which), nullptr);
            });
            return true;
        }
        Log("FAIL grip: weapons, ik, entity or actor");
        return true;
    }

    // muzzles [reset] | muzzles <bone> <min> <max>: logs the shots actors fired since the last reset by the bone they
    // came from (tag_flash, a dual wield gun's left one tag_flash1); with a range, PASS when that bone's count is
    // in it
    bool RunMuzzlesCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "muzzles")
            return false;
        std::string what;
        args >> what;
        std::map<std::string, std::size_t> counts;
        RunOnMainThread([&] { counts = Actors::GetMuzzleCounts(what == "reset"); });
        std::size_t low = 0, high = 0;
        if (what != "reset" && !what.empty() && args >> low >> high)
        {
            const auto count = counts.contains(what) ? counts[what] : 0;
            Log(std::format("{} muzzle {} fired {} (from {} to {})", count >= low && count <= high ? "PASS" : "FAIL",
                            what, count, low, high));
            return true;
        }
        std::string text;
        for (const auto& [bone, count] : counts)
            text += std::format(" {} {}", bone, count);
        Log("MUZZLES" + text);
        return true;
    }

    // The bank an alias plays from, searched as the game does (0x9002B0: the last bank added first; in each, the
    // alias lists of a bank at +8, 0x14 bytes each with the alias' hash at +4, as many as +4 says), with its name
    // (+0); empty for none
    std::string FindAliasBank(const std::string& alias)
    {
        typedef std::uint32_t(__cdecl * SND_HashName_t)(const char* name);
        typedef void(__cdecl * Sys_CriticalSection_t)(std::int32_t section);
        const auto& addresses = GetGameAddresses();
        const auto hash = reinterpret_cast<SND_HashName_t>(addresses.SND_HashName())(alias.c_str());
        reinterpret_cast<Sys_CriticalSection_t>(addresses.Sys_EnterCriticalSection())(8);
        const auto banks = reinterpret_cast<std::uint8_t* const*>(addresses.snd_banks());
        const auto count = *reinterpret_cast<const std::int32_t*>(addresses.snd_bankCount());
        std::string found;
        for (auto i = count - 1; i >= 0 && found.empty(); --i)
        {
            const auto bank = banks[i];
            const auto aliases = bank ? *reinterpret_cast<std::uint8_t* const*>(bank + 8) : nullptr;
            const auto entries = bank ? *reinterpret_cast<const std::uint32_t*>(bank + 4) : 0;
            for (std::uint32_t j = 0; aliases && j < entries; ++j)
                if (*reinterpret_cast<const std::uint32_t*>(aliases + j * 0x14 + 4) == hash)
                {
                    found = *reinterpret_cast<const char* const*>(bank);
                    break;
                }
        }
        reinterpret_cast<Sys_CriticalSection_t>(addresses.Sys_LeaveCriticalSection())(8);
        return found;
    }

    // The campaign's and Zombies' weapons. weaponcheck <variant> [expect
    // registered|zone|none]: where the actors' look-up finds a weapon (the map registered it, a weapon zone loaded
    // it, or nowhere), with its world model, projectile shots, shot sound and the bank that plays it, and its left
    // gun. soundbanks: the sound system's banks, the one searched last first. soundalias <alias> [expect <bank>|none]:
    // the bank an alias plays from. playerweapon [expect <variant>]: what the local player holds in a live game.
    bool RunWeaponCatalogCommand(const std::string& name, std::istringstream& args)
    {
        using namespace Structures;
        if (name == "weapondamage")
        {
            // weapondamage <variant> [expect|set <direct> <inner> <outer> <radius> [<explodes on impact>]]: logs what its def holds for a hit and
            // its explosion, PASS when those, or writes them (to see what the game does with
            // the def's own: a turned-off check)
            std::string weapon, word;
            std::array<std::int32_t, 4> wanted{};
            std::int32_t explodes = -1;
            args >> weapon >> word >> wanted[0] >> wanted[1] >> wanted[2] >> wanted[3] >> explodes;
            std::string text;
            RunOnMainThread([&] {
                const auto variant = WeaponRig::FindVariant(weapon);
                if (!variant || !variant->weapDef)
                {
                    text = std::format("FAIL weapondamage {}: no such weapon", weapon);
                    return;
                }
                const auto def = variant->weapDef;
                if (word == "set")
                {
                    def->iDamage = wanted[0];
                    def->iExplosionInnerDamage = wanted[1];
                    def->iExplosionOuterDamage = wanted[2];
                    def->iExplosionRadius = wanted[3];
                    if (explodes >= 0)
                        def->bProjImpactExplode = explodes != 0;
                }
                const std::array<std::int32_t, 4> values{def->iDamage, def->iExplosionInnerDamage,
                                                         def->iExplosionOuterDamage, def->iExplosionRadius};
                text = std::format("weapondamage {}: {} {} {} {}, explodes on impact {}", weapon, values[0], values[1],
                                   values[2], values[3], def->bProjImpactExplode);
                if (word == "expect")
                    text = std::format("{} {}", values == wanted ? "PASS" : "FAIL", text);
            });
            Log(text);
            return true;
        }
        if (name == "weaponcheck")
        {
            std::string weapon, word, expect;
            args >> weapon >> word >> expect;
            std::string text;
            RunOnMainThread([&] {
                const auto variant = WeaponRig::FindVariant(weapon);
                const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
                const auto registeredEnd = variants + Functions::GetWeaponVariantCount();
                const bool registered = variant && std::find(variants + 1, registeredEnd, variant) != registeredEnd;
                const auto where = !variant ? "none" : registered ? "registered" : "zone";
                if (!variant)
                {
                    text = std::format("weapon {} found {}", weapon, where);
                    return;
                }
                const auto def = variant->weapDef;
                const auto model = def && def->worldModel && def->worldModel[0] ? def->worldModel[0]->name : "-";
                const auto sound = WeaponRig::GetShotSound(*variant);
                const auto left = WeaponRig::FindLeftHandVariant(*variant);
                const auto magazine = def && def->worldModel && def->worldModel[0]
                                          ? WeaponRig::FindMagazineTag(*def->worldModel[0], *variant)
                                          : std::uint16_t{0};
                text = std::format(
                    "weapon {} found {}: model {} magazine {} projectile {} sound '{}' from '{}' left {}", weapon,
                    where, model, magazine ? Functions::SL_ConvertToString(magazine) : "-",
                    WeaponRig::FiresProjectiles(*variant), sound ? sound : "",
                    sound && *sound ? FindAliasBank(sound) : "",
                    left && left->szInternalName ? left->szInternalName : "-");
                if (!expect.empty())
                    text = std::format("{} {} (expected {})", expect == where ? "PASS" : "FAIL", text, expect);
            });
            Log(text);
            return true;
        }
        if (name == "soundbanks")
        {
            std::string text;
            RunOnMainThread([&] {
                const auto banks = reinterpret_cast<std::uint8_t* const*>(GetGameAddresses().snd_banks());
                const auto count = *reinterpret_cast<const std::int32_t*>(GetGameAddresses().snd_bankCount());
                for (std::int32_t i = 0; i < count; ++i)
                    text += std::format(" {}", banks[i] ? *reinterpret_cast<const char* const*>(banks[i]) : "-");
            });
            Log("SOUNDBANKS" + text);
            return true;
        }
        if (name == "soundalias")
        {
            std::string alias, word, expect;
            args >> alias >> word >> expect;
            std::string bank;
            RunOnMainThread([&] { bank = FindAliasBank(alias); });
            const auto text = std::format("sound '{}' plays from '{}'", alias, bank.empty() ? "none" : bank);
            if (expect.empty())
                Log(text);
            else
                Log(std::format("{} {} (expected {})", (bank.empty() ? "none" : bank) == expect ? "PASS" : "FAIL",
                                text, expect));
            return true;
        }
        if (name == "playerweapon")
        {
            // (expect: PASS when the player holds it or has it among its weapons: give doesn't switch to it; holds: PASS
            // when it's in hand; lacks: PASS when it has it neither way; a number of ms after it waits that long to be
            // so, as a give first loads the gun's zone and its sounds)
            std::string word, expect;
            std::uint32_t wait = 0;
            args >> word >> expect >> wait;
            std::string weapon, held;
            bool has = false;
            const auto read = [&] {
                held.clear();
                const auto cg = GetClientGlobals();
                const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
                const auto nameOf = [&](std::uint32_t index) -> std::string {
                    return index > 0 && index < MAX_WEAPON_VARIANTS && variants[index] &&
                                   variants[index]->szInternalName
                               ? variants[index]->szInternalName
                               : "none";
                };
                weapon = nameOf(cg && cg->snap ? cg->snap->ps.weapon : 0);
                has = weapon == expect;
                if (word == "holds")
                    return;
                for (std::size_t i = 0; cg && cg->snap && i < MAX_HELD_WEAPONS; ++i)
                    if (const auto other = cg->snap->ps.heldWeapons[i].weapon)
                    {
                        held += " " + nameOf(other);
                        has |= nameOf(other) == expect;
                    }
            };
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait);
            RunOnMainThread(read);
            while (has == (word == "lacks") && std::chrono::steady_clock::now() < end)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                RunOnMainThread(read);
            }
            const auto text = std::format("player holds {}, has{}", weapon, held);
            Log(expect.empty()
                    ? text
                    : std::format("{} {} ({} {})", has != (word == "lacks") ? "PASS" : "FAIL", text,
                                  word == "lacks" ? "without" : "expected", expect));
            return true;
        }
        return false;
    }

    // What the player of a live game asks of it (Components::LiveGame). you give <variant> [keep]
    // [camo <n>]: as the You tab's gun, in place of the gun in hand unless keep; you killstreak <type>; you canswap on|off;
    // you bullet on|off|last, you bullet radius <n>, you bullet kills on|off, you bullet expect <hits> [client]: PASS with
    // that many hits since the start, the last on that client; you bullet shots expect <shots> <direct>; you raises
    // [reset]; you raises expect server|client <first> <plain> <empty>; you clip <n>: the gun in hand's magazine;
    // you ammo: the guns held, with their magazine and stock; you ammo expect <clip> <stock>: of the gun in hand; you
    // problem [expect <text>]; you hosted [expect yes|no]; you hand expect <variant> [ms]: PASS once the gun in hand is
    // that; you settings: what the You page and the bodies' rows have chosen, you settings expect <text>: PASS when
    // that line has it (canswap on, bullet off, radius 200, damage kill, from aim, timer on, seconds 12)
    bool RunYouCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "you")
            return false;
        namespace Game = Components::LiveGame;
        std::string word;
        args >> word;
        const auto onOff = [&](const char* what, auto change) {
            std::string value;
            args >> value;
            auto settings = Game::GetSettings();
            change(settings, value == "on");
            Game::SetSettings(settings);
            Log(std::format("YOU {} {}", what, value));
        };
        const auto describeGun = [](const Structures::playerState_s& ps, std::uint32_t index) {
            using namespace Structures;
            const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
            if (index == 0 || index >= MAX_WEAPON_VARIANTS || !variants[index])
                return std::make_tuple(std::string("none"), 0, 0);
            const auto count = [](const auto& slots, std::int32_t wanted) {
                const auto found = std::ranges::find(slots, wanted, &PlayerAmmoSlot::index);
                return found != std::end(slots) ? found->count : 0;
            };
            return std::make_tuple(std::string(variants[index]->szInternalName),
                                   count(ps.ammoInClip, variants[index]->iClipIndex),
                                   count(ps.ammoNotInClip, variants[index]->iAmmoIndex));
        };

        if (word == "give")
        {
            Game::GunRequest gun;
            args >> gun.weapon;
            std::string option;
            while (args >> option)
            {
                if (option == "keep")
                    gun.replace = false;
                else if (option == "camo")
                    args >> gun.camo;
            }
            Game::RequestGun(gun);
            Log(std::format("YOU give {}", gun.weapon));
        }
        else if (word == "killstreak")
        {
            std::string type;
            args >> type;
            const auto known = Components::LiveGameLogic::FindKillstreak(type);
            Game::RequestKillstreak(type);
            Log(std::format("YOU killstreak {} {}", type, known ? known->name : "(not in the list)"));
        }
        else if (word == "canswap")
        {
            onOff("canswap", [](Game::Settings& settings, bool on) { settings.canswap = on; });
        }
        else if (word == "raises")
        {
            std::string what, side;
            args >> what;
            if (what == "reset")
            {
                LiveGame::ResetRaises();
                Log("YOU raises reset");
            }
            else if (what == "expect")
            {
                LiveGame::Raises wanted;
                args >> side >> wanted.first >> wanted.plain >> wanted.empty;
                const auto raises = LiveGame::GetRaises(side == "client");
                Log(std::format("{} raises {} first {} plain {} empty {}", raises == wanted ? "PASS" : "FAIL", side,
                                raises.first, raises.plain, raises.empty));
            }
            else
            {
                for (const auto client : {false, true})
                {
                    const auto raises = LiveGame::GetRaises(client);
                    Log(std::format("RAISES {} first {} plain {} empty {}", client ? "client" : "server", raises.first,
                                    raises.plain, raises.empty));
                }
                std::string animations;
                for (const auto animation : LiveGame::GetAnimations())
                    animations += std::format(" {:#x}", animation);
                Log("ANIMATIONS" + animations);
            }
        }
        else if (word == "clip")
        {
            std::int32_t count = 0;
            args >> count;
            LiveGame::QueueOnServer([count] {
                const auto cg = Structures::GetClientGlobals();
                const auto client = cg && cg->snap ? cg->snap->ps.clientNum : -1;
                const auto ps = client >= 0 ? LiveActors::GetPlayerState(client) : nullptr;
                const auto variants =
                    reinterpret_cast<Structures::WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
                if (ps && ps->weapon && variants[ps->weapon])
                    LiveActors::CallMethod(client, "setweaponammoclip",
                                           {std::string(variants[ps->weapon]->szInternalName), count});
            });
            Log(std::format("YOU clip {}", count));
        }
        else if (word == "ammo")
        {
            std::string what;
            args >> what;
            std::int32_t clip = 0, stock = 0;
            args >> clip >> stock;
            RunOnMainThread([&] {
                const auto cg = Structures::GetClientGlobals();
                if (!cg || !cg->snap)
                {
                    Log("FAIL ammo: no game");
                    return;
                }
                const auto& ps = cg->snap->ps;
                const auto [hand, handClip, handStock] = describeGun(ps, ps.weapon);
                if (what == "expect")
                {
                    Log(std::format("{} ammo {} clip {} stock {}", handClip == clip && handStock == stock ? "PASS" : "FAIL",
                                    hand, handClip, handStock));
                    return;
                }
                std::string text = std::format("AMMO hand {}", hand);
                for (const auto& held : ps.heldWeapons)
                    if (held.weapon)
                    {
                        const auto [gun, inClip, inStock] = describeGun(ps, held.weapon);
                        text += std::format(" | {} {}/{} had {}", gun, inClip, inStock, held.hadWeapon ? 1 : 0);
                    }
                Log(text);
            });
        }
        else if (word == "bullet")
        {
            std::string what;
            args >> what;
            if (what == "on" || what == "off")
            {
                auto settings = Game::GetSettings();
                settings.explosiveBullet = what == "on";
                Game::SetSettings(settings);
                Log(std::format("YOU bullet {}", what));
            }
            else if (what == "radius")
            {
                float radius = 0.0f;
                args >> radius;
                auto settings = Game::GetSettings();
                settings.radius = radius;
                Game::SetSettings(settings);
                Log(std::format("YOU bullet radius {}", Game::GetSettings().radius));
            }
            else if (what == "kills")
            {
                onOff("bullet kills", [](Game::Settings& settings, bool on) { settings.bulletKills = on; });
            }
            else if (what == "reset")
            {
                LiveGame::ResetBulletCounts();
                Log("YOU bullet reset");
            }
            else if (what == "walls")
            {
                std::string value;
                args >> value;
                LiveGame::SetWallsStopBullets(value == "on");
                Log(std::format("YOU bullet walls {}", value));
            }
            else if (what == "kind")
            {
                std::int32_t kind = 0;
                args >> kind;
                const auto hit = LiveGame::GetLastBulletHit();
                Log(std::format("{} bullet kind {} ({})", hit.kind == kind ? "PASS" : "FAIL", hit.kind, hit.weapon));
            }
            else if (what == "weapon")
            {
                std::string weapon;
                args >> weapon;
                const auto hit = LiveGame::GetLastBulletHit();
                Log(std::format("{} bullet weapon {}", hit.weapon == weapon ? "PASS" : "FAIL", hit.weapon));
            }
            else if (what == "from")
            {
                std::string from;
                args >> from;
                auto settings = Game::GetSettings();
                settings.fromAim = from == "aim";
                Game::SetSettings(settings);
                Log(std::format("YOU bullet from {}", from));
            }
            else if (what == "shots")
            {
                std::string expect;
                std::int32_t shots = 0, direct = 0;
                args >> expect >> shots >> direct;
                const auto counted = LiveGame::GetBulletShots();
                Log(std::format("{} bullet shots {} direct {}", counted[0] == shots && counted[1] == direct ? "PASS" : "FAIL",
                                counted[0], counted[1]));
            }
            else
            {
                const auto hit = LiveGame::GetLastBulletHit();
                const auto text = std::format("hits {} last client {} damage {} kind {} weapon {} at {:.0f} {:.0f} {:.0f}",
                                              hit.count, hit.client, hit.damage, hit.kind, hit.weapon, hit.point[0],
                                              hit.point[1], hit.point[2]);
                if (what == "expect")
                {
                    std::int32_t hits = 0, client = -1;
                    args >> hits;
                    const bool named = static_cast<bool>(args >> client);
                    Log(std::format("{} bullet {}", hit.count == hits && (!named || hit.client == client) ? "PASS" : "FAIL",
                                    text));
                }
                else
                {
                    Log("BULLET " + text);
                }
            }
        }
        else if (word == "slot")
        {
            // the fourth action slot, where the game puts a killstreak
            std::string what, wanted;
            std::uint32_t wait = 0;
            args >> what >> wanted >> wait;
            std::string held;
            const auto read = [&] {
                RunOnMainThread([&] {
                    const auto cg = Structures::GetClientGlobals();
                    held = "none";
                    if (!cg || !cg->snap || cg->snap->ps.actionSlotType[3] != 1)
                        return;
                    const auto& ps = cg->snap->ps;
                    held = std::get<0>(describeGun(ps, ps.actionSlotWeapon[3]));
                });
            };
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait);
            read();
            while (what == "expect" && held != wanted && std::chrono::steady_clock::now() < end)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                read();
            }
            Log(what == "expect" ? std::format("{} slot {}", held == wanted ? "PASS" : "FAIL", held)
                                 : std::format("SLOT {}", held));
        }
        else if (word == "problem")
        {
            std::string what, wanted, part;
            args >> what;
            while (args >> part)
                wanted += (wanted.empty() ? "" : " ") + part;
            const auto problem = Game::GetProblem();
            Log(what == "expect" ? std::format("{} problem '{}'", problem == wanted ? "PASS" : "FAIL", problem)
                                 : std::format("PROBLEM '{}'", problem));
        }
        else if (word == "hand")
        {
            std::string what, wanted;
            std::uint32_t wait = 0;
            args >> what >> wanted >> wait;
            std::string hand;
            const auto read = [&] {
                RunOnMainThread([&] {
                    const auto cg = Structures::GetClientGlobals();
                    hand = cg && cg->snap ? std::get<0>(describeGun(cg->snap->ps, cg->snap->ps.weapon)) : "none";
                });
            };
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait);
            read();
            while (hand != wanted && std::chrono::steady_clock::now() < end)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                read();
            }
            Log(std::format("{} hand {} (expected {})", hand == wanted ? "PASS" : "FAIL", hand, wanted));
        }
        else if (word == "settings")
        {
            const auto settings = Game::GetSettings();
            const auto onOffText = [](bool on) { return on ? "on" : "off"; };
            const auto line = std::format("canswap {} bullet {} radius {:.0f} damage {} from {} timer {} seconds {}",
                                          onOffText(settings.canswap), onOffText(settings.explosiveBullet),
                                          settings.radius, settings.bulletKills ? "kill" : "gun",
                                          settings.fromAim ? "aim" : "landing", onOffText(settings.bodyTimer),
                                          settings.bodySeconds);
            std::string what, wanted;
            args >> what >> std::ws;
            std::getline(args, wanted);
            if (what == "expect")
                Log(std::format("{} settings '{}' (expected {})",
                                (" " + line + " ").find(" " + wanted + " ") != std::string::npos ? "PASS" : "FAIL", line,
                                wanted));
            else
                Log("SETTINGS " + line);
        }
        else if (word == "hosted")
        {
            std::string what, wanted;
            args >> what >> wanted;
            const auto hosted = Mod::GetGameInterface()->IsHostingLiveGame();
            Log(what == "expect" ? std::format("{} hosted {}", hosted == (wanted == "yes") ? "PASS" : "FAIL", hosted ? "yes" : "no")
                                 : std::format("HOSTED {}", hosted ? "yes" : "no"));
        }
        else
        {
            Log(std::format("FAIL you {}: no such command", word));
        }
        return true;
    }

    // A live game's bodies: bodies: how many the server has and the client was sent; bodies
    // clear, bodies timer <seconds>|off: as the live page's; bodies expect <n> [ms], bodies drawn expect <n> [ms]: PASS
    // when the server has, or the client's snapshot (a live game's or a film's) holds, that many within that long
    std::int32_t CountSnapshotBodies()
    {
        std::int32_t count = 0;
        RunOnMainThread([&] {
            const auto cg = Structures::GetClientGlobals();
            if (!cg || !cg->snap)
                return;
            for (std::int32_t i = 0; i < cg->snap->numEntities; ++i)
                count += cg->snap->entities[i].eType == Structures::ET_PLAYER_CORPSE;
        });
        return count;
    }

    bool RunBodiesCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "bodies")
            return false;
        namespace Game = Components::LiveGame;
        std::string word;
        args >> word;
        const auto expectBodies = [&](const char* what, const std::function<std::int32_t()>& read) {
            std::int32_t wanted = 0;
            std::uint32_t wait = 0;
            args >> wanted >> wait;
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait);
            auto count = read();
            while (count != wanted && std::chrono::steady_clock::now() < end)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                count = read();
            }
            Log(std::format("{} bodies {} {}", count == wanted ? "PASS" : "FAIL", what, count));
        };
        if (word == "clear")
        {
            Game::RequestClearBodies();
            Log("BODIES clear");
        }
        else if (word == "timer")
        {
            std::string value;
            args >> value;
            auto settings = Game::GetSettings();
            settings.bodyTimer = value != "off";
            if (settings.bodyTimer)
                settings.bodySeconds = std::stoi(value);
            Game::SetSettings(settings);
            Log(std::format("BODIES timer {}", value));
        }
        else if (word == "expect")
        {
            expectBodies("on the server", [] { return LiveGame::GetBodyCount(); });
        }
        else if (word == "drawn")
        {
            args >> word;
            expectBodies("in the snapshot", [] { return CountSnapshotBodies(); });
        }
        else
        {
            Log(std::format("BODIES server {} snapshot {}", LiveGame::GetBodyCount(), CountSnapshotBodies()));
        }
        return true;
    }

    // Live bots. live add [still|guard|roam] [enemy|friend] [at <x y z yaw>]: adds one
    // and logs its id; live set <id> mode|side|weapon|camo <value>, live set <id> at <x y z yaw>; live skill
    // <easy|normal|hard|veteran>; live remove <id>|all; live list: each with its client, health, place, gun and legs'
    // animation; live wait <id> [ms]: PASS once it is alive; live deaths <id> ... see below
    std::map<std::uint32_t, std::array<float, 3>> liveMarks;

    bool RunLiveCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "live")
            return false;
        namespace Live = Components::LiveActors;
        std::string word;
        args >> word;

        const auto describe = [](std::uint32_t id) {
            const auto state = Live::GetState(id);
            std::string legs = "-";
            if (state.client >= 0)
                RunOnMainThread([&] {
                    using namespace Structures;
                    const auto cg = GetClientGlobals();
                    const auto entities = GetEntities();
                    if (!cg || !entities || !cg->animScriptData)
                        return;
                    const std::uint32_t anim = entities[state.client].nextState.legsAnim & ~ANIM_TOGGLEBIT;
                    if (anim < cg->animScriptData->numAnimations)
                        legs = cg->animScriptData->animations[anim].name;
                });
            return std::format("live {} client {} '{}' {} health {} at {:.0f} {:.0f} {:.0f} weapon {} legs {} {}", id,
                               state.client, state.name, state.alive ? "alive" : "dead", state.health, state.origin[0],
                               state.origin[1], state.origin[2], state.weapon.empty() ? "-" : state.weapon, legs,
                               state.problem);
        };
        const auto parseMode = [](const std::string& text) {
            return text == "guard"  ? Types::LiveActorMode::Guard
                   : text == "roam" ? Types::LiveActorMode::Roam
                                    : Types::LiveActorMode::Still;
        };
        // an actor's id, last: the one added last, as the panel or a key adds them too, or first: the oldest there
        const auto newest = [] {
            const auto actors = Live::GetActors();
            return actors.empty() ? 0u : actors.back().id;
        };
        const auto oldest = [] {
            const auto actors = Live::GetActors();
            return actors.empty() ? 0u : actors.front().id;
        };
        const auto readId = [&] {
            std::string id;
            args >> id;
            return id == "last" ? newest() : id == "first" ? oldest() : static_cast<std::uint32_t>(std::stoul(id));
        };
        const auto readPlace = [&](Types::LiveActor& actor) {
            std::array<float, 3> origin{};
            args >> origin[0] >> origin[1] >> origin[2] >> actor.yaw;
            actor.origin = origin;
        };

        if (word == "add")
        {
            Types::LiveActor actor;
            std::string part;
            while (args >> part)
            {
                if (part == "at")
                    readPlace(actor);
                else if (part == "enemy" || part == "friend")
                    actor.side = part == "enemy" ? Types::LiveActorSide::Enemy : Types::LiveActorSide::Friend;
                else
                    actor.mode = parseMode(part);
            }
            Log(std::format("LIVE added {}", Live::Add(std::move(actor))));
            return true;
        }
        if (word == "set")
        {
            const auto id = readId();
            std::string field, value;
            args >> field;
            auto actor = Live::GetActor(id);
            if (!actor)
            {
                Log(std::format("FAIL no live actor {}", id));
                return true;
            }
            if (field == "at")
            {
                readPlace(*actor);
            }
            else
            {
                args >> value;
                if (value == "-")
                    value.clear();
                if (field == "mode")
                    actor->mode = parseMode(value);
                else if (field == "side")
                    actor->side = value == "friend" ? Types::LiveActorSide::Friend : Types::LiveActorSide::Enemy;
                else if (field == "weapon")
                    actor->weapon = value;
                else if (field == "camo")
                    actor->camo = std::stoi(value);
                else
                    Log(std::format("FAIL live set: no field {}", field));
            }
            Live::Update(*actor);
            Log(std::format("live {} {} set", id, field));
            return true;
        }
        if (word == "skill")
        {
            std::string skill;
            args >> skill;
            Live::SetSkill(skill == "easy"      ? Types::LiveActorSkill::Easy
                           : skill == "hard"    ? Types::LiveActorSkill::Hard
                           : skill == "veteran" ? Types::LiveActorSkill::Veteran
                                                : Types::LiveActorSkill::Normal);
            return true;
        }
        if (word == "remove")
        {
            std::string which;
            args >> which;
            if (which == "all")
                Live::Clear();
            else
                Live::Remove(which == "last" ? newest() : static_cast<std::uint32_t>(std::stoul(which)));
            Log(std::format("live {} removed", which));
            return true;
        }
        if (word == "list")
        {
            const auto actors = Live::GetActors();
            Log(std::format("LIVE {} actors", actors.size()));
            for (const auto& actor : actors)
                Log(describe(actor.id));
            return true;
        }
        if (word == "wait")
        {
            const auto id = readId();
            std::int32_t most = 15000;
            args >> most;
            const auto started = std::chrono::steady_clock::now();
            while (!Live::GetState(id).alive &&
                   std::chrono::steady_clock::now() - started < std::chrono::milliseconds(most))
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const bool alive = Live::GetState(id).alive;
            Log(std::format("{} {}", alive ? "PASS" : "FAIL", describe(id)));
            return true;
        }
        if (word == "expect")
        {
            // live expect <id> alive|dead|weapon <name>|legs <name>|at <x y z> <tolerance>|health below <n>
            const auto id = readId();
            std::string what, value;
            args >> what;
            const auto state = Live::GetState(id);
            bool pass = false;
            if (what == "alive" || what == "dead")
            {
                pass = state.alive == (what == "alive");
            }
            else if (what == "weapon")
            {
                args >> value;
                pass = state.weapon == value;
            }
            else if (what == "legs")
            {
                args >> value;
                pass = describe(id).find(" legs " + value + " ") != std::string::npos;
            }
            else if (what == "at")
            {
                float x = 0, y = 0, z = 0, tolerance = 0;
                args >> x >> y >> z >> tolerance;
                pass = std::hypot(state.origin[0] - x, state.origin[1] - y, state.origin[2] - z) <= tolerance;
            }
            else if (what == "health")
            {
                std::int32_t most = 0;
                args >> value >> most;
                pass = value == "is" ? state.health == most : state.health < most;
            }
            else if (what == "chosen")
            {
                // live expect <id> chosen mode|side|weapon <value>: what is chosen
                std::string field;
                args >> field >> std::ws;
                std::getline(args, value);
                const auto actor = Live::GetActor(id);
                std::string chosen = "no such actor";
                if (actor)
                {
                    constexpr std::array MODES = {"still", "guard", "roam"};
                    constexpr std::array SIDES = {"enemy", "friend"};
                    chosen = field == "mode"   ? MODES[static_cast<std::size_t>(actor->mode)]
                             : field == "side" ? SIDES[static_cast<std::size_t>(actor->side)]
                                               : actor->weapon;
                }
                pass = (chosen.empty() ? "-" : chosen) == value;
                value = std::format("{} {} is {}", field, value, chosen);
            }
            else if (what == "shown")
            {
                // live expect <id> shown <name>: the name in the kill feed and scoreboard, as the client has it
                args >> value;
                std::string shown;
                RunOnMainThread([&] {
                    const auto cg = Structures::GetClientGlobals();
                    if (cg && state.client >= 0)
                        shown = cg->clientInfo[state.client].name;
                });
                pass = shown == value;
                value += " shows " + shown;
            }
            else if (what == "named")
            {
                // live expect <id> named: the game's bot name, the same in the kill feed and scoreboard
                std::string shown;
                RunOnMainThread([&] {
                    const auto cg = Structures::GetClientGlobals();
                    if (cg && state.client >= 0)
                        shown = cg->clientInfo[state.client].name;
                });
                pass = !state.name.empty() && state.name != "Joining..." && shown == state.name;
                value = std::format("'{}' shows '{}'", state.name, shown);
            }
            else if (what == "moved")
            {
                float most = 0;
                args >> most;
                const auto& from = liveMarks[id];
                pass = std::hypot(state.origin[0] - from[0], state.origin[1] - from[1]) >= most;
                value = std::to_string(most);
            }
            else if (what == "stayed")
            {
                float most = 0;
                args >> most;
                const auto& from = liveMarks[id];
                pass = std::hypot(state.origin[0] - from[0], state.origin[1] - from[1]) <= most;
                value = std::to_string(most);
            }
            Log(std::format("{} {} (expected {} {})", pass ? "PASS" : "FAIL", describe(id), what, value));
            return true;
        }
        if (word == "selected")
        {
            // live selected <id|all>: PASS when the panel shows that one
            std::string which;
            args >> which;
            const auto selected = Live::GetSelected();
            const bool pass = which == "all"    ? selected == Live::ALL_ACTORS
                              : which == "last" ? selected == newest()
                                                : selected == static_cast<std::uint32_t>(std::stoul(which));
            Log(std::format("{} selected {} (expected {})", pass ? "PASS" : "FAIL",
                            selected ? std::to_string(*selected) : "none", which));
            return true;
        }
        if (word == "count")
        {
            // live count <n>: PASS when there are that many actors
            std::size_t expected = 0;
            args >> expected;
            const auto count = Live::GetActors().size();
            Log(std::format("{} {} actors (expected {})", count == expected ? "PASS" : "FAIL", count, expected));
            return true;
        }
        if (word == "strings")
        {
            // live strings <id>: logs the text in the client's state, by its offset (to find the clan tag)
            const auto state = Live::GetState(readId());
            RunOnMainThread([&] {
                using namespace Structures;
                const auto entities = *reinterpret_cast<std::uint8_t**>(GetGameAddresses().sv_gentities());
                const auto size = *reinterpret_cast<std::int32_t*>(GetGameAddresses().sv_gentitySize());
                const auto client = *reinterpret_cast<std::uint8_t**>(entities + state.client * size + GENTITY_CLIENT);
                for (std::size_t i = GCLIENT_TEAM - 4; i < GCLIENT_TEAM + 0xD4; ++i)
                {
                    const auto c = client[i];
                    if (c >= 32 && c < 127 && (client[i - 1] < 32 || client[i - 1] >= 127))
                        Log(std::format("  +{:x} {}", i, reinterpret_cast<const char*>(client + i)));
                }
            });
            return true;
        }
        if (word == "view")
        {
            // live view <id> <distance>: puts the player that far south of the actor, looking at it (cheats on)
            const auto id = readId();
            float distance = 200.0f;
            args >> distance;
            const auto state = Live::GetState(id);
            Functions::Cbuf_AddText(std::format("setviewpos {} {} {} 90 3", state.origin[0], state.origin[1] - distance,
                                                state.origin[2] + 60.0f));
            Log(std::format("live view {} from {}", id, distance));
            return true;
        }
        if (word == "nodes")
        {
            // live nodes: how many path nodes the map has (read where the bot code reads it, this build's address)
            Log(std::format("LIVE path nodes {}", *reinterpret_cast<std::uint32_t*>(0x32C3F84)));
            return true;
        }
        if (word == "walked")
        {
            // live walked <id> <ms> <distance>: PASS when its path over that time was at least that long
            const auto id = readId();
            std::int32_t most = 0;
            float wanted = 0;
            args >> most >> wanted;
            auto last = Live::GetState(id).origin;
            float walked = 0;
            const auto started = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(most))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                const auto now = Live::GetState(id).origin;
                walked += std::hypot(now[0] - last[0], now[1] - last[1]);
                last = now;
            }
            Log(std::format("{} {} walked {:.0f} (expected {:.0f})", walked >= wanted ? "PASS" : "FAIL", describe(id),
                            walked, wanted));
            return true;
        }
        if (word == "clients")
        {
            // live clients <n>: PASS when the game has that many test clients in it
            std::int32_t expected = 0, count = 0;
            args >> expected;
            RunOnMainThread([&] {
                using namespace Structures;
                const auto clients = *reinterpret_cast<std::uint8_t**>(GetGameAddresses().svs_clients());
                const auto maxClients = Functions::FindDvar("sv_maxclients");
                for (std::int32_t i = 0; clients && maxClients && i < maxClients->current.integer; ++i)
                {
                    const auto client = clients + i * SERVER_CLIENT_SIZE;
                    count += *reinterpret_cast<std::int32_t*>(client) == SERVER_CLIENT_ACTIVE &&
                             *reinterpret_cast<std::int32_t*>(client + SERVER_CLIENT_IS_BOT) != 0;
                }
            });
            Log(std::format("{} {} test clients (expected {})", count == expected ? "PASS" : "FAIL", count, expected));
            return true;
        }
        if (word == "mark")
        {
            const auto id = readId();
            liveMarks[id] = Live::GetState(id).origin;
            Log(std::format("live {} marked", id));
            return true;
        }
        if (word == "host")
        {
            // live host: logs the player's health; live host wait <ms>: answers the team and class menus for it until
            // it has spawned; live host expect hurt|dead [ms]: PASS once it has been hurt or killed
            std::string what, how;
            std::int32_t most = 20000;
            args >> what;
            if (what == "expect")
                args >> how;
            args >> most;
            const auto health = [] {
                std::int32_t value = 0;
                RunOnMainThread([&] { value = LiveActors::GetHostHealth(); });
                return value;
            };
            const auto started = std::chrono::steady_clock::now();
            const auto expired = [&] {
                return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(most);
            };
            if (what == "wait")
            {
                while (health() <= 0 && !expired())
                {
                    RunOnMainThread([] { LiveActors::JoinHost(Components::LiveActorLogic::MENU_TEAM, "autoassign"); });
                    std::this_thread::sleep_for(std::chrono::milliseconds(700));
                    RunOnMainThread([] {
                        LiveActors::JoinHost(Components::LiveActorLogic::MENU_CLASS,
                                             Components::LiveActorLogic::CLASS_RESPONSE);
                    });
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                }
                Log(std::format("{} player health {}", health() > 0 ? "PASS" : "FAIL", health()));
            }
            else if (what == "expect")
            {
                const auto seen = [&] {
                    return how == "dead" ? health() <= 0 : how == "alive" ? health() >= 100 : health() < 100;
                };
                while (!seen() && !expired())
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                Log(std::format("{} player {} (health {})", seen() ? "PASS" : "FAIL", how, health()));
            }
            else
            {
                Log(std::format("player health {}", health()));
            }
            return true;
        }
        return false;
    }

    // Auto Ease (see RunKillstreakCommand for why it is apart)
    bool RunAutoEaseCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "autoease")
            return false;
        // autoease: logs Auto Ease's settings and the campath's nodes' eases (percent, "a" when Auto Ease's);
        // autoease on|off, autoease set <start %> <end %>, autoease nodes firstandlast|allnodes: sets them as the
        // Camera tab does (one step each); autoease expect <on|off> [<start %> <end %>], autoease nodes expect
        // <firstandlast|allnodes>, autoease node <index> expect <in> <out>: PASS when so (a node's eases as logged:
        // "30a" follows Auto Ease, "30" is its own)
        auto& keyframes = Components::KeyframeManager::Get();
        const auto& property = keyframes.GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        std::string action;
        args >> action;
        const auto describe = [](float amount, bool follows) {
            return std::format("{:.0f}{}", amount * 100.0f, follows ? "a" : "");
        };
        if (action == "on" || action == "off" || action == "set")
        {
            RunOnRenderThread([&] {
                auto autoEase = keyframes.GetAutoEase();
                if (action == "set")
                {
                    float start = 0.0f, end = 0.0f;
                    args >> start >> end;
                    autoEase.start = start / 100.0f;
                    autoEase.end = end / 100.0f;
                }
                else
                    autoEase.on = action == "on";
                keyframes.SetAutoEase(autoEase, "Auto Ease (harness)");
            });
        }
        if (action == "nodes")
        {
            std::string which, expected;
            args >> which >> expected;
            const auto parse = [](std::string text) {
                std::ranges::transform(text, text.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
                return text == "allnodes" ? Types::AutoEaseNodes::AllNodes : Types::AutoEaseNodes::FirstAndLast;
            };
            if (which == "expect")
            {
                Types::AutoEaseNodes nodes{};
                RunOnRenderThread([&] { nodes = keyframes.GetAutoEase().nodes; });
                Log(std::format("{} autoease nodes {} (expected {})", nodes == parse(expected) ? "PASS" : "FAIL",
                                magic_enum::enum_name(nodes), expected));
                return true;
            }
            RunOnRenderThread([&] {
                auto autoEase = keyframes.GetAutoEase();
                autoEase.nodes = parse(which);
                keyframes.SetAutoEase(autoEase, "Auto Ease nodes (harness)");
            });
        }
        if (action == "node")
        {
            std::size_t index = 0;
            std::string expect, in, out;
            args >> index >> expect >> in >> out;
            std::optional<std::pair<std::string, std::string>> eases;
            RunOnRenderThread([&] {
                const auto& nodes = keyframes.GetKeyframes(property);
                if (index < nodes.size())
                    eases = std::pair(describe(nodes[index].easeIn, nodes[index].autoEaseIn),
                                      describe(nodes[index].easeOut, nodes[index].autoEaseOut));
            });
            Log(std::format("{} autoease node {}: in {} out {} (expected {} {})",
                            eases && eases->first == in && eases->second == out ? "PASS" : "FAIL", index,
                            eases ? eases->first : "none", eases ? eases->second : "none", in, out));
            return true;
        }
        std::string text;
        Components::KeyframeManager::AutoEase autoEase;
        RunOnRenderThread([&] {
            autoEase = keyframes.GetAutoEase();
            const auto& nodes = keyframes.GetKeyframes(property);
            for (std::size_t i = 0; i < nodes.size(); ++i)
                text += std::format(" node {} in {} out {};", i, describe(nodes[i].easeIn, nodes[i].autoEaseIn),
                                    describe(nodes[i].easeOut, nodes[i].autoEaseOut));
        });
        const auto settings = std::format("{} start {:.0f} end {:.0f} nodes {}", autoEase.on ? "on" : "off",
                                          autoEase.start * 100.0f, autoEase.end * 100.0f,
                                          magic_enum::enum_name(autoEase.nodes));
        if (action != "expect")
        {
            Log(std::format("AUTOEASE {}:{}", settings, text));
            return true;
        }
        std::string on;
        float start = -1.0f, end = -1.0f;
        args >> on >> start >> end;
        const bool pass = (on == "on") == autoEase.on &&
                          (start < 0.0f || std::abs(autoEase.start * 100.0f - start) < 0.5f) &&
                          (end < 0.0f || std::abs(autoEase.end * 100.0f - end) < 0.5f);
        Log(std::format("{} autoease {} (expected {}{}):{}", pass ? "PASS" : "FAIL", settings, on,
                        start < 0.0f ? "" : std::format(" start {} end {}", start, end), text));
        return true;
    }

    // killstreak effectrow <actor> <key> on|off, killstreak damage <actor> none|light|heavy [<from tick>], killstreak
    // extras [reset] [expect <part of an effect's name> <least>]: a killstreak's effects
    // as its Look page sets them: a row of them on or off (its key: rotors, lights, exhaust, rotorwash,
    // treads, afterburners, contrails), Damaged (from the playhead, or the tick given); logs how many of each dust and
    // tread effect the killstreaks played since the last reset (PASS when those with the part in their name are at
    // least that many, or none for 0)
    void RunKillstreakEffectsCommand(const std::string& what, std::istringstream& args)
    {
        namespace Killstreaks = Components::KillstreakLogic;
        if (what == "extras")
        {
            std::string first, part;
            std::size_t least = 0;
            args >> first;
            const bool reset = first == "reset";
            if (reset)
                args >> first;
            if (first == "forward")
            {
                // killstreak extras forward <part of a name> [<x y z> <degrees>]: which way the effect with that in
                // its name last played; with a direction, PASS when within those degrees of it
                glm::vec3 expected{};
                float degrees = 0.0f;
                args >> part;
                const bool expecting = static_cast<bool>(args >> expected.x >> expected.y >> expected.z >> degrees);
                std::optional<glm::vec3> forward;
                RunOnMainThread([&] { forward = Actors::GetKillstreakEffectForward(part); });
                const auto cosine = forward ? glm::dot(glm::normalize(*forward), glm::normalize(expected)) : -1.0f;
                const auto off = glm::degrees(std::acos(std::clamp(cosine, -1.0f, 1.0f)));
                const auto text = forward ? std::format("{:.2f} {:.2f} {:.2f}", forward->x, forward->y, forward->z)
                                          : std::string("never played");
                if (expecting)
                    Log(std::format("{} killstreak extras forward {}: {}, {:.1f} degrees off",
                                    off <= degrees ? "PASS" : "FAIL", part, text, off));
                else
                    Log(std::format("KILLSTREAK extras forward {}: {}", part, text));
                return;
            }
            args >> part >> least;
            std::map<std::string, std::size_t> counts;
            RunOnMainThread([&] { counts = Actors::GetKillstreakEffectCounts(reset); });
            std::string text;
            std::size_t matching = 0;
            for (const auto& [effect, count] : counts)
            {
                text += std::format(" {} {};", effect, count);
                if (!part.empty() && effect.find(part) != std::string::npos)
                    matching += count;
            }
            // (at least none: none)
            if (first == "expect")
                Log(std::format("{} killstreak extras: {} of {} (expected {}{}):{}",
                                (least == 0 ? matching == 0 : matching >= least) ? "PASS" : "FAIL", matching, part,
                                least == 0 ? "none" : "at least ", least == 0 ? "" : std::to_string(least), text));
            else
                Log(std::format("KILLSTREAK extras{}:{}", reset ? " (reset)" : "", text));
            return;
        }
        std::string which, value;
        args >> which;
        auto actor = Components::Actors::GetActor(ParseActorId(which));
        const auto killstreak = actor ? Killstreaks::FindByModels(actor->models) : std::nullopt;
        if (!killstreak)
        {
            Log(std::format("FAIL killstreak {}: {} is no killstreak", what, which));
            return;
        }
        if (what == "effectrow")
        {
            std::string state;
            args >> value >> state;
            const auto rows = Killstreaks::GetEffectRows(Killstreaks::Get(*killstreak));
            const auto row = std::ranges::find_if(
                rows, [&](Killstreaks::EffectRow each) { return Killstreaks::GetEffectRowKey(each) == value; });
            if (row == rows.end())
            {
                Log(std::format("FAIL killstreak effectrow: actor {} has no row {}", actor->id, value));
                return;
            }
            Killstreaks::SetEffectRowOn(*actor, *row, state != "off");
            Components::Actors::Update(*actor);
            Log(std::format("KILLSTREAK effectrow {} of actor {} {}", value, actor->id, state != "off" ? "on" : "off"));
            return;
        }
        // damage
        args >> value;
        std::uint32_t from = Components::Playback::GetActorTick();
        args >> from;
        actor->damage = value == "heavy"   ? Types::ActorDamage::Heavy
                        : value == "light" ? Types::ActorDamage::Light
                                           : Types::ActorDamage::None;
        actor->damagedFrom = from;
        Components::Actors::Update(*actor);
        Log(std::format("KILLSTREAK damage of actor {} {} from {}", actor->id, value, from));
    }

    // killstreak flight <actor> <from> <to> [expect <speed> <pitch> <drawn>]: how smoothly a killstreak flies between
    // two ticks: core's most change of its speed between 10 ms samples and of its
    // pitch between 16 ms frames, its fastest and its speed at the end; then its pitch as drawn every 32 ms (the
    // playhead seeking there, frozen), its most change between two; PASS when each is at most the value given
    void RunKillstreakFlight(std::istringstream& args)
    {
        std::string which, expect;
        std::uint32_t from = 0, to = 0;
        float speedStep = 0.0f, pitchStep = 0.0f, drawnStep = 0.0f;
        args >> which >> from >> to >> expect >> speedStep >> pitchStep >> drawnStep;
        const auto id = ParseActorId(which);
        const auto actor = Components::Actors::GetActor(id);
        if (!actor || !Components::Actors::GetVehicleBody(*actor, from) || to <= from + 32)
        {
            Log(std::format("FAIL killstreak flight {}: not a killstreak, or no span", which));
            return;
        }
        const auto at = [&](std::uint32_t tick) {
            return Components::Actors::EvaluatePlacement(*actor, tick).position;
        };
        const auto speedAt = [&](std::uint32_t tick) { return glm::distance(at(tick + 10), at(tick)) * 100.0f; };
        float speeds = 0.0f, fastest = 0.0f, pitches = 0.0f, drawn = 0.0f;
        std::uint32_t speedsAt = from, pitchesAt = from, drawnAt = from;
        for (auto tick = from, last = from; tick < to; last = tick, tick += 10)
            if (const auto step = std::abs(speedAt(tick) - speedAt(last)); step > speeds)
                std::tie(speeds, speedsAt) = std::pair(step, tick);
        for (auto tick = from; tick < to; tick += 10)
            fastest = std::max(fastest, speedAt(tick));
        auto pitch = Components::Actors::GetVehicleBody(*actor, from)->pitch;
        for (auto tick = from + 16; tick < to; tick += 16)
        {
            const auto now = Components::Actors::GetVehicleBody(*actor, tick)->pitch;
            if (std::abs(now - pitch) > pitches)
                std::tie(pitches, pitchesAt) = std::pair(std::abs(now - pitch), tick);
            pitch = now;
        }
        std::optional<float> before;
        for (auto tick = from; tick < to; tick += 32)
        {
            Components::Playback::MoveFrozenTick(tick);
            Sleep(60);
            std::optional<Types::TargetBone> bone;
            RunOnMainThread([&] { bone = Actors::GetDrawnBone(id, "tag_origin"); });
            if (!bone)
                continue;
            const auto now = glm::degrees(std::asin(std::clamp(-bone->axes[0].z, -1.0f, 1.0f)));
            if (before && std::abs(now - *before) > drawn)
                std::tie(drawn, drawnAt) = std::pair(std::abs(now - *before), tick);
            before = now;
        }
        const auto text = std::format(
            "speed step {:.1f} at {}, pitch step {:.2f} at {}, drawn pitch step {:.2f} at {}, "
            "fastest {:.0f}, at the end {:.1f} u/s",
            speeds, speedsAt, pitches, pitchesAt, drawn, drawnAt, fastest, speedAt(to));
        if (expect != "expect")
            Log(std::format("KILLSTREAK flight of actor {} from {} to {}: {}", id, from, to, text));
        else
            Log(std::format(
                "{} killstreak flight of actor {} from {} to {} (at most {} {} {}): {}",
                speeds <= speedStep && pitches <= pitchStep && drawn <= drawnStep && before ? "PASS" : "FAIL", id, from,
                to, speedStep, pitchStep, drawnStep, text));
    }

    // Fire and what it drops
    void RunKillstreakFireCommand(const std::string& what, std::istringstream& args)
    {
        std::string which;
        args >> which;
        const auto id = ParseActorId(which);
        if (what == "fire")
        {
            // killstreak fire <actor> [rockets]: Fire from the playhead, as the Events page's + Event adds it; with
            // rockets, its Fire row's Rockets (the Gunship's)
            std::string rockets;
            args >> rockets;
            RunOnRenderThread([&] { UI::ActorActions::AddKillstreakFire(id); });
            auto actor = Components::Actors::GetActor(id);
            const auto killstreak = actor ? Components::KillstreakLogic::FindByModels(actor->models) : std::nullopt;
            if (rockets == "rockets" && killstreak && !actor->events.empty())
            {
                const auto selected = Components::Actors::GetSelectedEvent();
                auto event = std::ranges::find(actor->events, selected.value_or(0), &Types::ActorEvent::id);
                if (event == actor->events.end())
                    event = actor->events.end() - 1;
                Components::KillstreakLogic::SetRockets(Components::KillstreakLogic::Get(*killstreak), *event, true);
                Components::Actors::Update(*actor);
                actor = Components::Actors::GetActor(id);
            }
            const auto events = actor ? actor->events.size() : 0;
            Log(std::format("KILLSTREAK fire of actor {}: {} events, last at {}{}", id, events,
                            events ? actor->events.back().tick : 0,
                            events && actor->events.back().rockets ? ", rockets" : ""));
            return;
        }
        // killstreak drops <actor> [expect <n> | expect drawn <n> | expect begun <n> | expect land <x y z> <distance> |
        // expect ground <height> <distance> | expect from <x y z> <distance> | expect at <x y z> <distance>]: logs what
        // its Fire events drop as the game shows it; PASS when it has that many, that many are drawn, that many of
        // their effects have begun, the first lands that close to a point or that height, leaves that close to a
        // point, or is drawn that close to one
        std::string expect, kind;
        std::vector<Actors::DropState> drops;
        RunOnMainThread([&] { drops = Actors::GetDropStates(id); });
        std::size_t drawn = 0, begun = 0;
        for (const auto& drop : drops)
        {
            drawn += drop.drawnAt ? 1 : 0;
            begun += drop.begun;
            Log(std::format(
                "DROP event {} shot {} {} from {:.0f} {:.0f} {:.0f} at {} lands {} after {:.3f} s, drawn {}, plays {} "
                "begun {}",
                drop.event, drop.shot, drop.model, drop.release.x, drop.release.y, drop.release.z, drop.releaseTick,
                drop.landing ? std::format("{:.0f} {:.0f} {:.0f}", drop.landing->x, drop.landing->y, drop.landing->z)
                             : "nowhere",
                drop.seconds,
                drop.drawnAt ? std::format("{:.0f} {:.0f} {:.0f}", drop.drawnAt->x, drop.drawnAt->y, drop.drawnAt->z)
                             : "no",
                drop.plays, drop.begun));
        }
        if (!(args >> expect >> kind) || expect != "expect")
        {
            Log(std::format("DROPS of actor {}: {}, {} drawn, {} effects begun", id, drops.size(), drawn, begun));
            return;
        }
        const auto report = [&](bool passed, const std::string& text) {
            Log(std::format("{} killstreak drops of actor {}: {}", passed ? "PASS" : "FAIL", id, text));
        };
        const auto readPoint = [&](glm::vec3& point, float& distance) {
            args >> point.x >> point.y >> point.z >> distance;
        };
        if (kind == "drawn" || kind == "begun")
        {
            std::size_t want = 0;
            args >> want;
            const auto got = kind == "drawn" ? drawn : begun;
            report(got == want, std::format("{} {} (expected {})", got, kind, want));
        }
        else if (kind == "land" || kind == "from" || kind == "at")
        {
            glm::vec3 point{};
            float distance = 0.0f;
            readPoint(point, distance);
            std::optional<glm::vec3> got;
            if (!drops.empty())
                got = kind == "land"   ? drops.front().landing
                      : kind == "from" ? std::optional(drops.front().release)
                                       : drops.front().drawnAt;
            const auto off = got ? glm::distance(*got, point) : FLT_MAX;
            report(off <= distance, std::format("{} {:.1f} off (within {:.1f})", kind, off, distance));
        }
        else if (kind == "ground")
        {
            float height = 0.0f, distance = 0.0f;
            args >> height >> distance;
            const bool has = !drops.empty() && drops.front().landing;
            const auto landed = has ? drops.front().landing->z : 0.0f;
            report(has && std::abs(landed - height) <= distance,
                   std::format("lands at height {:.1f} (expected {:.1f})", landed, height));
        }
        else
        {
            const auto want = std::strtoul(kind.c_str(), nullptr, 10);
            report(drops.size() == want, std::format("{} drops (expected {})", drops.size(), want));
        }
    }

    // A prop on its own (see RunCharacterZoneCommand for why they are apart)
    bool RunPlacedPropCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "prop")
            return false;
        std::string what;
        args >> what;
        if (what == "list")
        {
            // prop list [part]: how many models a browser could offer, and those with the part in their name
            std::string part;
            args >> part;
            const auto catalog = Components::Actors::GetCatalog();
            if (!catalog)
            {
                Log("FAIL prop list: no catalog");
                return true;
            }
            std::size_t shown = 0;
            std::ofstream all(directory / "prop-models.txt");
            for (const auto& model : catalog->props)
            {
                all << model << "\n";
                if (!part.empty() && model.find(part) != std::string::npos && ++shown <= 40)
                    Log("PROPMODEL " + model);
            }
            Log(std::format("PROPS {} models loaded, {} with '{}'", catalog->props.size(), shown, part));
            return true;
        }
        if (what == "add")
        {
            // prop add <model> crosshair | <x y z> [yaw]: as Add Prop will (on the surface where the camera looks,
            // ActorActions::AddProp), or at a point, the latest becoming last
            std::string model, where;
            args >> model >> where;
            const auto catalog = Components::Actors::GetCatalog();
            if (!catalog || !std::ranges::binary_search(catalog->props, model))
            {
                Log(std::format("FAIL prop add: no model {}", model));
                return true;
            }
            if (where == "crosshair")
            {
                RunOnRenderThread([&] {
                    const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                    UI::ActorActions::AddProp(model, camera->GetPosition(), camera->GetForwardVector());
                });
                const auto actors = Components::Actors::GetActors();
                if (!actors.empty())
                    previousActorId = std::exchange(lastActorId, actors.back().id);
            }
            else
            {
                glm::vec3 point{std::strtof(where.c_str(), nullptr), 0.0f, 0.0f};
                float yaw = 0.0f;
                args >> point.y >> point.z >> yaw;
                auto actor = ActorLogic::MakeProp(model, point, yaw, Components::Playback::GetActorTick(),
                                                  Mod::GetGameInterface()->GetDemoInfo().endTick);
                actor.name = Components::Actors::MakeName(ActorLogic::GetPropBaseName(model));
                previousActorId = std::exchange(lastActorId, Components::Actors::Add(actor));
            }
            Log(std::format("PROP added {} as actor {}", model, lastActorId));
            return true;
        }
        if (what == "many")
        {
            // prop many <n> <model> <x> <y> <z>: that many, in rows 100 apart from the point
            std::size_t count = 0;
            std::string model;
            glm::vec3 point{};
            args >> count >> model >> point.x >> point.y >> point.z;
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto row = glm::vec3(static_cast<float>(i % 8), static_cast<float>(i / 8), 0.0f);
                auto actor = ActorLogic::MakeProp(model, point + row * 100.0f, 0.0f,
                                                  Components::Playback::GetActorTick(),
                                                  Mod::GetGameInterface()->GetDemoInfo().endTick);
                actor.name = Components::Actors::MakeName(ActorLogic::GetPropBaseName(model));
                previousActorId = std::exchange(lastActorId, Components::Actors::Add(actor));
            }
            return true;
        }
        if (what == "clearkeys")
        {
            // prop clearkeys: every prop's keys go, those of props not in the scene too (a demo's file keeps them), so
            // the next prop's id is 1
            std::size_t cleared = 0;
            RunOnRenderThread([&] {
                auto& keyframeManager = Components::KeyframeManager::Get();
                for (const auto type : Components::RuntimeProperties::GetAll())
                {
                    if (!Components::RuntimeProperties::IsProp(type) || !keyframeManager.HasProperty(type))
                        continue;
                    const auto& property = keyframeManager.GetProperty(type);
                    if (keyframeManager.GetKeyframes(property).empty())
                        continue;
                    keyframeManager.ClearKeyframes(property);
                    ++cleared;
                }
            });
            Log(std::format("PROP keys cleared of {} properties", cleared));
            return true;
        }
        if (what == "preview")
        {
            // prop preview [expect drawn|none]: where the browser's preview of the row hovered stands
            std::optional<glm::vec3> drawn;
            RunOnMainThread([&] { drawn = Actors::GetPropPreviewDrawn(); });
            const auto preview = Components::Props::GetPreview();
            Log(std::format("PROP preview {} drawn {}", preview ? preview->model : "none",
                            drawn ? std::format("{:.0f} {:.0f} {:.0f}", drawn->x, drawn->y, drawn->z) : "no"));
            std::string expect, kind;
            if ((args >> expect >> kind) && expect == "expect")
                Log(std::format("{} prop preview ({})", (kind == "drawn") == drawn.has_value() ? "PASS" : "FAIL",
                                kind));
            return true;
        }
        if (what == "groups")
        {
            // prop groups [expect <group> <least>]: how many models each of the browser's groups has; PASS
            // when that group has at least that many
            const auto catalog = Components::Actors::GetCatalog();
            if (!catalog)
            {
                Log("FAIL prop groups: no catalog");
                return true;
            }
            const auto counts = Components::PropLogic::CountGroups(catalog->props, catalog->mapProps);
            std::string line;
            for (const auto group : Components::PropLogic::ALL_GROUPS)
                line += std::format(" {} {}", Components::PropLogic::GetGroupKey(group),
                                    counts[static_cast<std::size_t>(group)]);
            Log("PROP groups" + line);
            std::string expect, key;
            std::size_t least = 0;
            if ((args >> expect >> key >> least) && expect == "expect")
            {
                const auto group = std::ranges::find_if(Components::PropLogic::ALL_GROUPS, [&](auto each) {
                    return Components::PropLogic::GetGroupKey(each) == key;
                });
                const auto got =
                    group != Components::PropLogic::ALL_GROUPS.end() ? counts[static_cast<std::size_t>(*group)] : 0;
                Log(std::format("{} prop groups: {} has {} (at least {})", got >= least ? "PASS" : "FAIL", key, got,
                                least));
            }
            return true;
        }
        if (what == "drawn")
        {
            // prop drawn [expect <n>]: how many models on their own the game draws
            std::size_t models = 0, want = 0;
            RunOnMainThread([&] { models = Actors::CountSceneModels(); });
            std::string expect;
            if ((args >> expect >> want) && expect == "expect")
                Log(std::format("{} prop drawn: {} (expected {})", models == want ? "PASS" : "FAIL", models, want));
            else
                Log(std::format("PROPS drawn {}", models));
            return true;
        }
        std::string which;
        args >> which;
        const auto id = ParseActorId(which);
        auto actor = Components::Actors::GetActor(id);
        if (!actor || !actor->placedProp)
        {
            Log(std::format("FAIL prop {}: actor {} is no prop", what, id));
            return true;
        }
        if (what == "set")
        {
            // prop set <actor> <pitch> <yaw> <roll> <scale>: turns and sizes it
            args >> actor->placedProp->pitch >> actor->yaw >> actor->placedProp->roll >> actor->placedProp->scale;
            Components::Actors::Update(*actor);
            return true;
        }
        if (what == "hide")
        {
            // prop hide <actor> <0|1>: as the row's eye
            args >> actor->hidden;
            Components::Actors::Update(*actor);
            return true;
        }
        if (what == "bar")
        {
            // prop bar <actor> <start> <end>: its bar on the timeline
            args >> actor->placedProp->start >> actor->placedProp->end;
            Components::Actors::Update(*actor);
            return true;
        }
        if (what == "key")
        {
            // prop key <actor> position|turn <tick|now> <x y z>: a key of its place there, as its row's diamond keys
            // it
            std::string channel, at;
            glm::vec3 value{};
            args >> channel >> at >> value.x >> value.y >> value.z;
            const auto tick = at == "now" ? Components::Playback::GetTimelineTick()
                                          : static_cast<std::uint32_t>(std::strtoul(at.c_str(), nullptr, 10));
            const auto which =
                channel == "turn" ? Components::PropLogic::Channel::Turn : Components::PropLogic::Channel::Position;
            RunOnRenderThread([&] {
                auto& keyframeManager = Components::KeyframeManager::Get();
                const auto& property = Components::Props::GetProperty(actor->placedProp->id, which);
                const auto keyed =
                    which == Components::PropLogic::Channel::Turn
                        ? Components::PropLogic::GetTurnForKey(keyframeManager.GetKeyframes(property), tick, value)
                        : value;
                keyframeManager.AddKeyframe(property, Types::Keyframe(property, tick, Types::KeyframeValue(keyed)));
                keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
            });
            Log(std::format("PROP key of actor {}: {} at {}", id, channel, tick));
            return true;
        }
        if (what == "keys")
        {
            // prop keys <actor> [clear | expect <position keys> <turn keys>]: how many keys its place has; or none
            std::string kind;
            args >> kind;
            if (kind == "clear")
            {
                RunOnRenderThread([&] { Components::Props::ClearKeys(actor->placedProp->id); });
                return true;
            }
            std::size_t position = 0, turn = 0;
            RunOnRenderThread([&] {
                auto& keyframeManager = Components::KeyframeManager::Get();
                position = keyframeManager
                               .GetKeyframes(Components::Props::GetProperty(actor->placedProp->id,
                                                                            Components::PropLogic::Channel::Position))
                               .size();
                turn = keyframeManager
                           .GetKeyframes(Components::Props::GetProperty(actor->placedProp->id,
                                                                        Components::PropLogic::Channel::Turn))
                           .size();
            });
            Log(std::format("PROP keys of actor {}: position {} turn {}", id, position, turn));
            std::size_t wantPosition = 0, wantTurn = 0;
            if (kind == "expect" && (args >> wantPosition >> wantTurn))
                Log(std::format("{} prop keys of actor {}",
                                position == wantPosition && turn == wantTurn ? "PASS" : "FAIL", id));
            return true;
        }
        if (what == "state")
        {
            // prop state <actor> [expect none | expect drawn | expect at <x y z> <distance> | expect bar <start>
            // <end>]: logs its model, bar, place and where the game drew it; PASS when it isn't drawn, is, is that
            // close to a point, or has that bar
            std::optional<glm::vec3> drawn;
            std::size_t models = 0;
            RunOnMainThread([&] {
                drawn = Actors::GetPlacedPropDrawn(id);
                models = Actors::CountSceneModels();
            });
            const auto& prop = *actor->placedProp;
            // (where its keys have it at the playhead)
            const auto keyed = Components::Props::WithKeyedPlacement(*actor, Components::Playback::GetTimelineTick());
            Log(std::format(
                "PROP {} {} bar {}-{} at {:.0f} {:.0f} {:.0f} turn {:.1f} {:.1f} {:.1f} scale {:.2f} drawn {} "
                "({} models alone)",
                id, prop.model, prop.start, prop.end, keyed.position.x, keyed.position.y, keyed.position.z,
                keyed.placedProp->pitch, keyed.yaw, keyed.placedProp->roll, prop.scale,
                drawn ? std::format("{:.0f} {:.0f} {:.0f}", drawn->x, drawn->y, drawn->z) : "no", models));
            std::string expect, kind;
            if ((args >> expect >> kind) && expect == "expect")
            {
                bool passed = false;
                if (kind == "none")
                    passed = !drawn;
                else if (kind == "drawn")
                    passed = drawn.has_value();
                else if (kind == "at")
                {
                    glm::vec3 point{};
                    float distance = 0.0f;
                    args >> point.x >> point.y >> point.z >> distance;
                    passed = drawn && glm::distance(*drawn, point) <= distance;
                }
                else if (kind == "bar")
                {
                    std::uint32_t start = 0, end = 0;
                    args >> start >> end;
                    passed = prop.start == start && prop.end == end;
                }
                else if (kind == "scale")
                {
                    float scale = 0.0f, tolerance = 0.01f;
                    args >> scale >> tolerance;
                    passed = std::abs(prop.scale - scale) <= tolerance;
                }
                else if (kind == "yaw")
                {
                    float yaw = 0.0f, tolerance = 1.0f;
                    args >> yaw >> tolerance;
                    passed = std::abs(std::remainder(keyed.yaw - yaw, 360.0f)) <= tolerance;
                }
                else if (kind == "model")
                {
                    std::string model;
                    args >> model;
                    passed = prop.model == model;
                }
                Log(std::format("{} prop state of actor {} ({})", passed ? "PASS" : "FAIL", id, kind));
            }
            return true;
        }
        Log(std::format("FAIL prop: no command {}", what));
        return true;
    }

    // commands for killstreak actors (see RunCharacterZoneCommand for why they are apart)
    bool RunKillstreakCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "killstreak")
            return false;
        namespace Killstreaks = Components::KillstreakLogic;
        std::string what;
        args >> what;
        if (what == "flight")
        {
            RunKillstreakFlight(args);
            return true;
        }
        if (what == "effectrow" || what == "damage" || what == "extras")
        {
            RunKillstreakEffectsCommand(what, args);
            return true;
        }
        if (what == "fire" || what == "drops")
        {
            RunKillstreakFireCommand(what, args);
            return true;
        }
        if (what == "list")
        {
            // killstreak list: the killstreaks, their models, class and motions
            for (const auto& info : Killstreaks::GetAll())
            {
                std::string models, motions;
                for (const auto model : info.models)
                    models += (models.empty() ? "" : ",") + std::string(model);
                for (const auto motion : Killstreaks::GetMotions(info.vehicleClass))
                    motions += (motions.empty() ? "" : ",") + std::string(motion);
                Log(std::format("KILLSTREAK {} ({}) of {}: class {}, motions {}, cruise {:.0f} u/s, height {:.0f}",
                                GetKillstreakKey(info), info.name, models, static_cast<int>(info.vehicleClass),
                                motions, Killstreaks::GetCruiseSpeed(info), info.flyHeight));
            }
            return true;
        }
        if (what == "add")
        {
            // killstreak add <name> <x> <y> <z> [yaw] [enemy|trapped]: an actor of the killstreak from its starting
            // point at the playhead (KillstreakLogic::MakeStart), at the point on the ground (flying ones as high
            // above it as they fly, a plane's pass over it halfway), facing yaw; logs its id
            std::string key, look;
            glm::vec3 point{};
            float yaw = 0.0f;
            args >> key >> point.x >> point.y >> point.z >> yaw >> look;
            const auto& all = Killstreaks::GetAll();
            const auto found =
                std::ranges::find_if(all, [&](const Killstreaks::Info& info) { return GetKillstreakKey(info) == key; });
            // (the Mortar Team, which isn't one of them: its points about the point, its shells coming in along yaw,
            // its Fire at the playhead)
            if (key == "mortarteam")
            {
                auto actor = Killstreaks::MakeMortar(point, yaw, Components::Playback::GetActorTick());
                actor.name = Components::Actors::MakeName(std::string(Killstreaks::MORTAR_NAME));
                const auto id = Components::Actors::Add(actor);
                previousActorId = std::exchange(lastActorId, id);
                Log(std::format("KILLSTREAK added mortarteam as actor {}", id));
                return true;
            }
            if (found == all.end())
            {
                Log(std::format("FAIL killstreak add: no killstreak {}", key));
                return true;
            }
            const auto tick = Components::Playback::GetActorTick();
            auto actor = Killstreaks::MakeStart(found->killstreak, tick, Mod::GetGameInterface()->GetDemoInfo().endTick,
                                                yaw);
            if (look == "enemy" && found->models.size() > 1)
                actor.models = {std::string(found->models[1])};
            if (look == "trapped" && found->models.size() > 2)
                actor.models = {std::string(found->models[2])};
            actor.name = Components::Actors::MakeName(std::string(found->name));
            const auto at = Killstreaks::GetStartPlaceTick(actor, tick);
            Components::Actors::ApplyPlacement(actor, at, point + glm::vec3(0.0f, 0.0f, actor.flyHeight), yaw);
            if (actor.movement == Types::ActorMovement::Path)
                actor.yaw = yaw;
            const auto id = Components::Actors::Add(actor);
            previousActorId = std::exchange(lastActorId, id);
            Log(std::format("KILLSTREAK added {} as actor {} ({}) at tick {}", key, id, actor.models.front(), tick));
            return true;
        }
        if (what == "pose")
        {
            // killstreak pose <model> [n]: a model's first n bones (8): their rest rotation from the parent (quats) and
            // the bind pose the surfaces are skinned against (baseMat), as quaternions
            std::string model;
            std::size_t count = 8;
            args >> model >> count;
            RunOnMainThread([&] {
                const auto found = static_cast<const Structures::XModel*>(
                    ContentPacks::FindAsset(Structures::ASSET_TYPE_XMODEL, model.c_str()));
                if (!found)
                {
                    Log(std::format("FAIL killstreak pose: no model {}", model));
                    return;
                }
                for (std::size_t b = 0; b < std::min(count, static_cast<std::size_t>(found->numBones)); ++b)
                {
                    std::string rest = "root";
                    if (b >= found->numRootBones && found->quats && found->trans)
                    {
                        const auto i = b - found->numRootBones;
                        rest = std::format("q {:.3f} {:.3f} {:.3f} {:.3f} t {:.2f} {:.2f} {:.2f}",
                                           found->quats[i][0] / 32767.0f, found->quats[i][1] / 32767.0f,
                                           found->quats[i][2] / 32767.0f, found->quats[i][3] / 32767.0f,
                                           found->trans[i][0], found->trans[i][1], found->trans[i][2]);
                    }
                    const auto& base = found->baseMat[b];
                    Log(std::format("KILLSTREAK pose {} {} {}: rest {} | base q {:.3f} {:.3f} {:.3f} {:.3f} t {:.2f} "
                                    "{:.2f} {:.2f} w {:.3f}",
                                    model, b, Functions::SL_ConvertToString(found->boneNames[b]), rest, base.quat[0],
                                    base.quat[1], base.quat[2], base.quat[3], base.trans[0], base.trans[1],
                                    base.trans[2], base.transWeight));
                }
            });
            return true;
        }
        if (what == "lod")
        {
            // killstreak lod <actor> [expect <part of a material name>]: the materials its body draws at the first level
            // of detail (the free camera's); PASS when one has the part (the Gunship's outside: glass_ext)
            std::string which, expect, part;
            args >> which >> expect >> part;
            const auto id = ParseActorId(which);
            std::vector<std::string> materials;
            RunOnMainThread([&] { materials = Actors::GetFirstLodMaterials(id); });
            std::string list;
            for (const auto& material : materials)
                list += " " + material;
            Log(std::format("KILLSTREAK lod of actor {}:{}", id, list));
            if (expect == "expect")
            {
                const bool has = std::ranges::any_of(
                    materials, [&](const std::string& material) { return material.find(part) != std::string::npos; });
                Log(std::format("{} killstreak lod of actor {} (expected {})", has ? "PASS" : "FAIL", id, part));
            }
            return true;
        }
        if (what == "mesh")
        {
            // killstreak mesh <model>: writes a model's levels of detail, surfaces (material, counts, bones) and every
            // vertex's position (and its bone, for those on one) into mesh-<model>.txt in the harness folder, with its
            // bones' bind pose, to compare its mesh with its skeleton
            std::string model;
            args >> model;
            RunOnMainThread([&] {
                const auto found = static_cast<const Structures::XModel*>(
                    ContentPacks::FindAsset(Structures::ASSET_TYPE_XMODEL, model.c_str()));
                if (!found)
                {
                    Log(std::format("FAIL killstreak mesh: no model {}", model));
                    return;
                }
                std::ofstream out(directory / ("mesh-" + model + ".txt"));
                for (std::size_t b = 0; b < found->numBones; ++b)
                    out << std::format("bone {} {} {:.2f} {:.2f} {:.2f}\n", b,
                                       Functions::SL_ConvertToString(found->boneNames[b]), found->baseMat[b].trans[0],
                                       found->baseMat[b].trans[1], found->baseMat[b].trans[2]);
                for (std::size_t l = 0; l < Structures::MAX_LODS; ++l)
                    out << std::format("lod {} dist {:.1f} surfs {} from {}\n", l, found->lodInfo[l].dist,
                                       found->lodInfo[l].numsurfs, found->lodInfo[l].surfIndex);
                std::size_t vertices = 0;
                for (std::size_t s = 0; s < found->numsurfs; ++s)
                {
                    // (the fields before the vertex buffer, as OpenAssetTools' T5 XSurface has them)
                    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&found->surfs[s]);
                    const auto u8 = [&](std::size_t at) { return bytes[at]; };
                    const auto u16 = [&](std::size_t at) {
                        std::uint16_t value;
                        std::memcpy(&value, bytes + at, sizeof(value));
                        return value;
                    };
                    const auto pointer = [&](std::size_t at) {
                        const void* value;
                        std::memcpy(&value, bytes + at, sizeof(value));
                        return value;
                    };
                    const auto vertCount = u16(0x04);
                    const std::array<std::uint16_t, 4> blendCounts{u16(0x10), u16(0x12), u16(0x14), u16(0x16)};
                    const auto blend = static_cast<const std::uint16_t*>(pointer(0x18));
                    const auto verts = static_cast<const std::uint8_t*>(pointer(0x20));
                    const auto lists = static_cast<const std::uint8_t*>(pointer(0x28));
                    const auto material = found->materialHandles && found->materialHandles[s]
                                              ? found->materialHandles[s]->name
                                              : "none";
                    out << std::format("surf {} {} verts {} tris {} lists {} blend {} {} {} {}\n", s, material,
                                       vertCount, u16(0x06), u8(0x01), blendCounts[0], blendCounts[1], blendCounts[2],
                                       blendCounts[3]);
                    // the bone of each vertex on one: from its rigid lists (12 bytes each: bone offset, vertices...), or
                    // the first of its blended ones
                    std::vector<int> bones(vertCount, -1);
                    if (lists && u8(0x01) > 0)
                    {
                        std::size_t at = 0;
                        for (std::size_t l = 0; l < u8(0x01); ++l)
                        {
                            std::uint16_t offset, count;
                            std::memcpy(&offset, lists + l * 12, 2);
                            std::memcpy(&count, lists + l * 12 + 2, 2);
                            for (std::size_t v = 0; v < count && at < vertCount; ++v)
                                bones[at++] = offset / 64;
                        }
                    }
                    else if (blend)
                    {
                        std::size_t at = 0, read = 0;
                        for (std::size_t k = 0; k < 4; ++k)
                            for (std::size_t v = 0; v < blendCounts[k] && at < vertCount; ++v)
                            {
                                bones[at++] = blend[read] / 64;
                                read += 1 + 2 * k;
                            }
                    }
                    for (std::size_t v = 0; verts && v < vertCount; ++v)
                    {
                        float xyz[3];
                        std::memcpy(xyz, verts + v * 32, sizeof(xyz));
                        out << std::format("v {} {:.2f} {:.2f} {:.2f} {}\n", s, xyz[0], xyz[1], xyz[2], bones[v]);
                    }
                    vertices += vertCount;
                }
                Log(std::format("KILLSTREAK mesh {}: {} bones, {} surfaces, {} vertices", model, found->numBones,
                                found->numsurfs, vertices));
            });
            return true;
        }
        if (what == "body")
        {
            // killstreak body <actor> [expect <value> <least> <most>]: how its body moves at the timeline's tick, as
            // core works it out (VehicleLogic::GetBody) and as drawn (its tag_origin's pitch and roll); PASS when the
            // value (pitch, roll, drift, steer, wheel, bodypitch, bodyroll, drawnpitch, drawnroll) is within
            std::string which, expect, value;
            float least = 0.0f, most = 0.0f;
            args >> which >> expect >> value >> least >> most;
            const auto id = ParseActorId(which);
            const auto actor = Components::Actors::GetActor(id);
            if (!actor)
            {
                Log(std::format("FAIL killstreak body {}: no such actor", which));
                return true;
            }
            const auto tick = Components::Playback::GetActorTick();
            const auto body = Components::Actors::GetVehicleBody(*actor, tick);
            if (!body)
            {
                Log(std::format("FAIL killstreak body {}: not a killstreak", which));
                return true;
            }
            std::optional<Types::TargetBone> drawn;
            RunOnMainThread([&] { drawn = Actors::GetDrawnBone(id, "tag_origin"); });
            std::map<std::string, float> values{{"pitch", body->pitch},         {"roll", body->roll},
                                                {"drift", glm::length(body->drift)}, {"steer", body->steer},
                                                {"wheel", body->wheelTurn},     {"bodypitch", body->bodyPitch},
                                                {"bodyroll", body->bodyRoll}};
            if (drawn)
            {
                const auto forward = drawn->axes[0], left = drawn->axes[1], up = drawn->axes[2];
                values["drawnpitch"] = glm::degrees(std::asin(std::clamp(-forward.z, -1.0f, 1.0f)));
                values["drawnroll"] = glm::degrees(std::atan2(left.z, up.z));
            }
            std::string text;
            for (const auto& [name, number] : values)
                text += std::format(" {} {:.2f}", name, number);
            if (expect != "expect")
            {
                Log(std::format("KILLSTREAK body of actor {} at tick {}:{}", id, tick, text));
                return true;
            }
            const auto found = values.find(value);
            const bool pass = found != values.end() && found->second >= least && found->second <= most;
            Log(std::format("{} killstreak body of actor {} at tick {}: {} within {} to {}:{}", pass ? "PASS" : "FAIL",
                            id, tick, value, least, most, text));
            return true;
        }
        if (what == "aim")
        {
            // killstreak aim <actor> <x> <y> <z> [expect <degrees>]: how far each of its guns' muzzles points off a
            // point, as drawn; PASS when each is within the degrees
            std::string which, expect;
            glm::vec3 point{};
            float most = 0.0f;
            args >> which >> point.x >> point.y >> point.z >> expect >> most;
            const auto id = ParseActorId(which);
            const auto actor = Components::Actors::GetActor(id);
            const auto killstreak = actor ? Killstreaks::FindByModels(actor->models) : std::nullopt;
            if (!killstreak || Killstreaks::Get(*killstreak).guns.empty())
            {
                Log(std::format("FAIL killstreak aim {}: not a killstreak with guns", which));
                return true;
            }
            std::string text;
            bool pass = true;
            for (const auto& gun : Killstreaks::Get(*killstreak).guns)
            {
                std::optional<Types::TargetBone> muzzle;
                RunOnMainThread([&] { muzzle = Actors::GetDrawnBone(id, std::string(gun.flash)); });
                if (!muzzle)
                {
                    text += std::format(" {}: not drawn", gun.flash);
                    pass = false;
                    continue;
                }
                const auto off = glm::degrees(std::acos(std::clamp(
                    glm::dot(glm::normalize(muzzle->axes[0]), glm::normalize(point - muzzle->position)), -1.0f, 1.0f)));
                text += std::format(" {} {:.1f} degrees off", gun.flash, off);
                pass = pass && off <= most;
            }
            if (expect != "expect")
                Log(std::format("KILLSTREAK aim of actor {}:{}", id, text));
            else
                Log(std::format("{} killstreak aim of actor {} within {} degrees:{}", pass ? "PASS" : "FAIL", id, most,
                                text));
            return true;
        }
        if (what == "joint")
        {
            // killstreak joint <actor> <bone> <pitch> <yaw> <roll> | off: turns a bone as the game's controllers do (in
            // place of its own rotation, relative to its parent), to see what a bone moves
            std::string which, bone, first;
            args >> which >> bone >> first;
            const auto id = ParseActorId(which);
            std::optional<glm::vec3> angles;
            if (first != "off")
            {
                glm::vec3 turned(0.0f);
                turned.x = std::strtof(first.c_str(), nullptr);
                args >> turned.y >> turned.z;
                angles = turned;
            }
            RunOnMainThread([&] { Actors::SetJointOverride(id, bone, angles); });
            Log(std::format("KILLSTREAK joint {} of actor {}: {}", bone, id,
                            angles ? std::format("{} {} {}", angles->x, angles->y, angles->z) : "off"));
            return true;
        }
        if (what == "effects")
        {
            // killstreak effects <actor> [expect <n> [has|no <part of a name>]]: the running effects of a killstreak
            // actor playing now (its rotors, lights, exhaust, smoke); PASS when at least that many are (none: when none
            // are), and one has the part in its name (or none has)
            std::string which, expect, check, part;
            std::size_t expected = 0;
            args >> which >> expect >> expected >> check >> part;
            const auto id = ParseActorId(which);
            std::vector<std::string> playing;
            RunOnMainThread([&] { playing = Actors::GetRunningEffects(id); });
            std::string text;
            for (const auto& effect : playing)
                text += " " + effect;
            const bool named = std::ranges::any_of(
                playing, [&](const std::string& effect) { return effect.find(part) != std::string::npos; });
            const bool pass =
                (expected == 0 ? playing.empty() : playing.size() >= expected) && (check == "has"  ? named
                                                                                   : check == "no" ? !named
                                                                                                   : true);
            if (expect != "expect")
                Log(std::format("KILLSTREAK effects of actor {} ({}):{}", id, playing.size(), text));
            else
                Log(std::format("{} killstreak effects of actor {}: {} playing (expected {}{}{}):{}",
                                pass ? "PASS" : "FAIL", id, playing.size(), expected == 0 ? "" : "at least ", expected,
                                check.empty() ? "" : std::format(", {} {}", check, part), text));
            return true;
        }
        // killstreak <actor> [expect <name|none>]: what killstreak the actor is (by its body), its look and height
        const auto actor = Components::Actors::GetActor(ParseActorId(what));
        if (!actor)
        {
            Log(std::format("FAIL killstreak {}: no such actor", what));
            return true;
        }
        std::string expect, expected;
        args >> expect >> expected;
        const auto killstreak = Killstreaks::FindByModels(actor->models);
        const auto key = killstreak ? GetKillstreakKey(Killstreaks::Get(*killstreak)) : std::string("none");
        const auto text = killstreak
                              ? std::format("actor {} is {} ({} look), flies {:.0f} above the ground, speed {:.0f}",
                                            actor->id, key,
                                            Killstreaks::GetLookName(Killstreaks::GetLook(Killstreaks::Get(*killstreak),
                                                                                          actor->models.front())),
                                            Components::Actors::GetFlyHeight(*actor), actor->pathSpeed)
                              : std::format("actor {} is no killstreak", actor->id);
        if (expect != "expect")
            Log("KILLSTREAK " + text);
        else
            Log(std::format("{} killstreak {} (expected {})", key == expected ? "PASS" : "FAIL", text, expected));
        return true;
    }

    // the names of an actor's clips in order, those Auto transitions put in marked with a + (see TransitionLogic)
    std::string DescribeChain(const Types::Actor& actor)
    {
        std::string text;
        for (const auto& clip : actor.clips)
            text += std::format("{}{}{}", text.empty() ? "" : " ", clip.chain != Types::ActorChain::None ? "+" : "",
                                clip.animation);
        return text.empty() ? std::string("-") : text;
    }

    // commands for chaining transitions: Auto transitions, the movement set, joins, Then... and how clean the joins
    // are (see RunCharacterZoneCommand for why they are apart)
    bool RunTransitionCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "auto")
        {
            // auto <actor> on|off | expect on|off: the actor's Auto transitions (as the Animation page's toggle: on
            // joins its clips, off takes out what it put in); PASS when it is so
            std::string which, what, expected;
            args >> which >> what >> expected;
            auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor)
            {
                Log(std::format("FAIL auto {}: no such actor", which));
                return true;
            }
            if (what == "on" || what == "off")
            {
                Components::Actors::SetAutoTransitions(*actor, what == "on");
                Components::Actors::Update(*actor);
            }
            const std::string now = actor->autoTransitions ? "on" : "off";
            if (what == "expect")
                Log(std::format("{} auto actor {} {} (expected {})", now == expected ? "PASS" : "FAIL", actor->id, now,
                                expected));
            else
                Log(std::format("AUTO actor {} {}: {}", actor->id, now, DescribeChain(*actor)));
            return true;
        }
        if (name == "movementset")
        {
            // movementset <actor> [auto|players|lowready|cqb|runandgun] | expect <set>: the actor's movement set (as
            // the Movement page's Moves as) and the one it stands for; PASS when it stands for that one
            constexpr std::array NAMES{"auto", "players", "lowready", "cqb", "runandgun"};
            std::string which, what, expected;
            args >> which >> what >> expected;
            auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor)
            {
                Log(std::format("FAIL movementset {}: no such actor", which));
                return true;
            }
            if (const auto set = std::ranges::find(NAMES, what); set != NAMES.end())
            {
                Components::Actors::SetMovementSet(*actor, static_cast<Types::ActorMovementSet>(set - NAMES.begin()));
                Components::Actors::Update(*actor);
            }
            const std::string own = NAMES[static_cast<std::size_t>(actor->movementSet)];
            const std::string resolved = NAMES[static_cast<std::size_t>(Components::Actors::GetMovementSet(*actor))];
            if (what == "expect")
                Log(std::format("{} movementset actor {} {} ({}) (expected {})",
                                resolved == expected ? "PASS" : "FAIL", actor->id, own, resolved, expected));
            else
                Log(std::format("MOVEMENTSET actor {} {} ({})", actor->id, own, resolved));
            return true;
        }
        if (name == "join")
        {
            // join <actor> <from index> <to index> [turn] [expect <animation>[,<animation>] | expect none]: the chain
            // that joins two of the actor's clips (TransitionLogic::JoinClips, turning so far), with the states they
            // end and start in; PASS when it is those animations
            std::string which, expect, expected;
            std::size_t from = 0, to = 0;
            float turn = 0.0f;
            args >> which >> from >> to;
            std::string next;
            args >> next;
            if (next != "expect")
            {
                if (!next.empty())
                    turn = std::stof(next);
                args >> expect;
            }
            else
                expect = next;
            args >> expected;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || from >= actor->clips.size() || to >= actor->clips.size())
            {
                Log(std::format("FAIL join {}: no such actor or clips", which));
                return true;
            }
            const auto body = Components::Actors::GetBody(*actor);
            const auto animations = Components::Actors::GetAnimationLookup();
            const auto links = TransitionLogic::JoinClips(*actor, from, to, body, animations, turn);
            std::string chain;
            for (const auto& link : links)
                chain += std::format("{}{}{}", chain.empty() ? "" : ",", link.animation,
                                     link.turn ? std::format("(turn {:.0f})", *link.turn) : "");
            const auto names = [&] {
                std::string text;
                for (const auto& link : links)
                    text += (text.empty() ? "" : ",") + link.animation;
                return text.empty() ? std::string("none") : text;
            }();
            const auto text = std::format(
                "actor {} clips {} ({}) to {} ({}) turning {:.0f}: {}", actor->id, from,
                AnimationCatalogLogic::DescribeState(TransitionLogic::GetClipState(*actor, from, true, body, animations)),
                to, AnimationCatalogLogic::DescribeState(TransitionLogic::GetClipState(*actor, to, false, body, animations)),
                turn, chain.empty() ? "none" : chain);
            if (expect == "expect")
                Log(std::format("{} join {} (expected {})", names == expected ? "PASS" : "FAIL", text, expected));
            else
                Log("JOIN " + text);
            return true;
        }
        if (name == "chain")
        {
            // chain <actor> [expect <animation> ...]: the actor's clips in order, those Auto transitions put in marked
            // with a +; PASS when they are those (a + where the clip was put in)
            std::string which;
            args >> which;
            std::string expect;
            args >> expect;
            std::string expected;
            for (std::string each; args >> each;)
                expected += (expected.empty() ? "" : " ") + each;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor)
            {
                Log(std::format("FAIL chain {}: no such actor", which));
                return true;
            }
            const auto chain = DescribeChain(*actor);
            std::string ticks;
            for (const auto& clip : actor->clips)
                ticks += std::format("{}{}-{}", ticks.empty() ? "" : " ", clip.start, clip.End());
            if (expect == "expect")
                Log(std::format("{} chain actor {}: {} (expected {})", chain == expected ? "PASS" : "FAIL", actor->id,
                                chain, expected));
            else
                Log(std::format("CHAIN actor {}: {} at {}", actor->id, chain, ticks));
            return true;
        }
        if (name == "then")
        {
            // then <actor> <clip index> [list [expect <animation>] | play <animation>]: what can follow the clip
            // (Then..., Components::Actors::GetChainCandidates) with what each does and how far its first pose is from
            // the clip's last, or plays one after it (as the menu does); list expect: PASS when that one is listed
            std::string which, what, animation, expect, expected;
            std::size_t index = 0;
            args >> which >> index >> what;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || index >= actor->clips.size())
            {
                Log(std::format("FAIL then {}: no such actor or clip", which));
                return true;
            }
            const auto clipId = actor->clips[index].id;
            if (what == "play")
            {
                args >> animation;
                RunOnRenderThread([&] { UI::ActorActions::PlayAfter(actor->id, clipId, animation); });
                Log(std::format("THEN actor {} played {} after clip {}: {}", actor->id, animation, index,
                                DescribeChain(*Components::Actors::GetActor(actor->id))));
                return true;
            }
            args >> expect >> expected;
            const auto candidates = Components::Actors::GetChainCandidates(*actor, clipId, 60);
            std::string text;
            for (const auto& candidate : candidates)
                text += std::format("{}{}{} [{}]{}", text.empty() ? "" : "; ", candidate.table ? "" : "~",
                                    candidate.animation, candidate.label,
                                    candidate.distance ? std::format(" {:.1f}u", *candidate.distance) : "");
            if (expect == "expect")
            {
                const bool listed = std::ranges::find(candidates, expected, &TransitionLogic::Candidate::animation) !=
                                    candidates.end();
                Log(std::format("{} then actor {} clip {} lists {} ({} candidates)", listed ? "PASS" : "FAIL",
                                actor->id, index, expected, candidates.size()));
            }
            else
                Log(std::format("THEN actor {} clip {} ({}): {}", actor->id, index, actor->clips[index].animation,
                                text.empty() ? "none" : text));
            return true;
        }
        if (name == "play")
        {
            // play <actor> <animation> [tick]: plays an animation on the actor from the tick (the timeline's), as the
            // browser's Play does (with Auto transitions joined to the clips around it)
            std::string which, animation;
            args >> which >> animation;
            std::optional<std::uint32_t> tick;
            if (std::uint32_t at = 0; args >> at)
                tick = at;
            const auto id = ParseActorId(which);
            if (!Components::Actors::GetActor(id))
            {
                Log(std::format("FAIL play {}: no such actor", which));
                return true;
            }
            RunOnRenderThread([&] { UI::ActorActions::Play(id, animation, tick); });
            Log(std::format("PLAY actor {} {}: {}", id, animation, DescribeChain(*Components::Actors::GetActor(id))));
            return true;
        }
        if (name == "walkto")
        {
            // walkto <actor> <x> <y> <z> [run] [face <yaw>]: as the view menu's Walk (Run) here and stop on the ground
            // under the point, from the timeline's tick, facing a way as it stops if given; logs its clips once it did
            std::string which;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            args >> which >> x >> y >> z;
            bool run = false;
            std::optional<float> face;
            for (std::string word; args >> word;)
                if (word == "run")
                    run = true;
                else if (float yaw = 0.0f; word == "face" && args >> yaw)
                    face = yaw;
            const auto id = ParseActorId(which);
            const auto actor = Components::Actors::GetActor(id);
            if (!actor)
            {
                Log(std::format("FAIL walkto {}: no such actor", which));
                return true;
            }
            const auto before = actor->clips;
            Components::Actors::RequestWalkTo(id, {x, y, z + 64.0f}, {0.0f, 0.0f, -1.0f}, run, face);
            for (int i = 0; i < 100; ++i)
            {
                Sleep(50);
                if (const auto now = Components::Actors::GetActor(id); now && now->clips != before)
                    break;
            }
            const auto now = Components::Actors::GetActor(id);
            std::string ticks;
            for (const auto& clip : now->clips)
                ticks += std::format("{}{}-{}", ticks.empty() ? "" : " ", clip.start, clip.End());
            Log(std::format("WALKTO actor {} {}: {} at {}", id, now->clips != before ? "went" : "didn't go",
                            DescribeChain(*now), ticks));
            return true;
        }
        if (name == "joinpose")
        {
            // joinpose <actor> <clip index> [expect <distance>]: how far the clip's first pose is from the last of the
            // clip before it (the poses' signatures, Components::PoseSignatures; waits up to 5 s for them); PASS when
            // at most the distance
            std::string which, expect;
            std::size_t index = 0;
            float most = TransitionLogic::CLEAN_JOIN;
            args >> which >> index >> expect >> most;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || index == 0 || index >= actor->clips.size())
            {
                Log(std::format("FAIL joinpose {}: no such actor or clip (not the first)", which));
                return true;
            }
            const auto poses = Components::PoseSignatures::GetLookup(*actor);
            std::shared_ptr<const TransitionLogic::PoseSignature> before, now;
            for (int i = 0; i < 100 && !(before && now); ++i)
            {
                before = poses(actor->clips[index - 1].animation);
                now = poses(actor->clips[index].animation);
                if (!(before && now))
                    Sleep(50);
            }
            const auto distance = before && now ? TransitionLogic::GetPoseDistance(before->last, now->first)
                                                : std::nullopt;
            const auto text = std::format("actor {} {} -> {}: {}", actor->id, actor->clips[index - 1].animation,
                                          actor->clips[index].animation,
                                          distance ? std::format("{:.2f} units", *distance) : "not known");
            if (expect == "expect")
                Log(std::format("{} joinpose {} (at most {})", distance && *distance <= most ? "PASS" : "FAIL", text,
                                most));
            else
                Log("JOINPOSE " + text);
            return true;
        }
        if (name == "popcheck")
        {
            // popcheck <actor> <from> <to> <step> <most> [bones...]: seeks from one tick to another in steps (freeze
            // and pause first) and logs the most each bone moved from one step to the next where the actor was drawn
            // (in its own space, as it is drawn: its walking along taken out); PASS when no bone moved more than most
            // (no pops at the joins)
            std::string which;
            std::uint32_t from = 0, to = 0, step = 50;
            float most = 6.0f;
            args >> which >> from >> to >> step >> most;
            std::vector<std::string> bones;
            for (std::string bone; args >> bone;)
                bones.push_back(bone);
            if (bones.empty())
                bones = {"j_mainroot", "j_spine4", "j_head", "j_wrist_le", "j_wrist_ri"};
            const auto id = ParseActorId(which);
            if (!Components::Playback::IsGameFrozen() || step == 0 || to <= from)
            {
                Log("FAIL popcheck: freeze first, and give a step and a stretch");
                return true;
            }
            std::vector<float> largest(bones.size(), 0.0f);
            std::vector<std::uint32_t> where(bones.size(), 0);
            std::vector<std::optional<glm::vec3>> previous;
            for (auto tick = from; tick <= to; tick += step)
            {
                Components::Playback::MoveFrozenTick(tick);
                Sleep(120);
                std::vector<std::optional<glm::vec3>> positions;
                std::optional<glm::mat4> placement;
                RunOnMainThread([&] {
                    positions = Actors::GetBonePositions(id, bones);
                    placement = Actors::GetDrawnPlacement(id);
                });
                if (!placement || positions.size() != bones.size())
                {
                    Log("FAIL popcheck: the actor isn't drawn");
                    return true;
                }
                const auto toActor = glm::inverse(*placement);
                for (auto& position : positions)
                    if (position)
                        position = glm::vec3(toActor * glm::vec4(*position, 1.0f));
                for (std::size_t i = 0; i < bones.size() && !previous.empty(); ++i)
                    if (positions[i] && previous[i])
                        if (const auto moved = glm::distance(*positions[i], *previous[i]); moved > largest[i])
                        {
                            largest[i] = moved;
                            where[i] = tick;
                        }
                previous = std::move(positions);
            }
            std::string text;
            bool pass = true;
            for (std::size_t i = 0; i < bones.size(); ++i)
            {
                text += std::format("{}{} {:.2f} at {}", text.empty() ? "" : ", ", bones[i], largest[i], where[i]);
                pass = pass && largest[i] <= most;
            }
            Log(std::format("{} popcheck actor {} {}-{} every {}: {} (at most {})", pass ? "PASS" : "FAIL", id, from, to,
                            step, text, most));
            return true;
        }
        if (name == "turncheck")
        {
            // turncheck <actor> <clip index> [expect <degrees> <tolerance>]: how the actor faces (core's placement)
            // as the clip starts, at its exit_align notetrack (its start without one), at its code_move (or end), and
            // as it ends; PASS when it turns that far (to the left) between exit_align and its end
            std::string which, expect;
            std::size_t index = 0;
            float degrees = 0.0f, tolerance = 5.0f;
            args >> which >> index >> expect >> degrees >> tolerance;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            const auto catalog = Components::Actors::GetCatalog();
            if (!actor || index >= actor->clips.size() || !catalog)
            {
                Log(std::format("FAIL turncheck {}: no such actor or clip", which));
                return true;
            }
            const auto& clip = actor->clips[index];
            const auto animation = catalog->FindAnimation(clip.animation);
            const auto duration = animation ? animation->duration : 0.0f;
            const auto at = [&](std::string_view notetrack) -> std::optional<float> {
                if (!animation)
                    return std::nullopt;
                const auto found = std::ranges::find(animation->notetracks, notetrack, &Types::ActorNotetrack::name);
                return found != animation->notetracks.end() ? std::optional(found->time) : std::nullopt;
            };
            const auto tickOf = [&](float time) {
                return clip.start + static_cast<std::uint32_t>(std::lround(
                                        std::max(time * duration - clip.offset, 0.0f) / std::max(clip.speed, 0.01f) *
                                        1000.0f));
            };
            const auto exitAlign = tickOf(at("exit_align").value_or(0.0f));
            const auto codeMove = tickOf(at("code_move").value_or(1.0f));
            const auto end = tickOf(1.0f);
            const auto yaw = [&](std::uint32_t tick) { return Components::Actors::EvaluatePlacement(*actor, tick).yaw; };
            const auto turned = std::remainder(yaw(end) - yaw(exitAlign), 360.0f);
            const auto text = std::format(
                "actor {} clip {} {}: faces {:.1f} at its start ({}), {:.1f} at exit_align ({}), {:.1f} at code_move "
                "({}), {:.1f} at its end ({}): turned {:.1f} from exit_align",
                actor->id, index, clip.animation, yaw(clip.start), clip.start, yaw(exitAlign), exitAlign, yaw(codeMove),
                codeMove, yaw(end), end, turned);
            if (expect == "expect")
                Log(std::format("{} turncheck {} (expected {} +- {})",
                                std::abs(std::remainder(turned - degrees, 360.0f)) <= tolerance ? "PASS" : "FAIL", text,
                                degrees, tolerance));
            else
                Log("TURNCHECK " + text);
            return true;
        }
        return false;
    }

    // commands for where actors stand: the root motion of their clips and their paths' rounded corners
    // (see RunCharacterZoneCommand for why they are apart)
    bool RunPlacementCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "rootmotion")
        {
            // rootmotion <actor> <clip index> [moves | stays | walks <0|1> | expect <none|moves|stays> | check
            // [<distance> <degrees>] | expect end <distance> <degrees>]: logs how the clip moves the actor by its
            // animation's root motion (ActorLogic::GetRootMotionUse), the root motion measured (its samples and where a
            // play ends), the game's delta of the whole play (XAnimGetRelDelta from 0 to 1: translation and rotation as
            // it gives them) and what the catalog decoded of it; moves or stays sets the clip's Root motion, walks its
            // Walks the path; check: PASS when the measured root motion between points of a play is the game's delta
            // between them (within 0.05 units and 0.5 degrees); expect end: PASS when the actor ends up (half a second
            // after the clip's play ends) where the game's delta of the play from where the clip starts in it, summed
            // as the game sums a blending tree's (weighed by the clip's weight while it blends in and out), turned the
            // way the actor faced as the clip started, puts it from there, facing as it turned; and logs how far that
            // is from the whole play's delta unweighed
            std::string which, mode, what;
            std::size_t index = 0;
            args >> which >> index >> mode;
            auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || index >= actor->clips.size())
            {
                Log("FAIL rootmotion: no such actor or clip");
                return true;
            }
            auto& clip = actor->clips[index];
            if (mode == "moves" || mode == "stays" || mode == "walks")
            {
                if (mode == "walks")
                {
                    int walks = 1;
                    args >> walks;
                    clip.moves = walks != 0;
                }
                else
                    clip.rootMotion = mode == "moves";
                Components::Actors::Update(*actor);
                Log(std::format("ROOTMOTION actor {} clip {}: root motion {}, walks the path {}", actor->id, index,
                                clip.rootMotion ? "moves" : "stays", clip.moves));
                return true;
            }
            constexpr std::array USES{"none", "moves", "stays"};
            const auto use = USES[static_cast<std::size_t>(Components::Actors::GetRootMotionUse(*actor, clip))];
            const auto motion = Components::Actors::GetAnimationRootMotion(clip.animation);
            const auto catalog = Components::Actors::GetCatalog();
            const auto animation = catalog ? catalog->FindAnimation(clip.animation) : nullptr;
            const auto entry = animation ? animation->entry : nullptr;
            const auto id = actor->id, clipId = clip.id;
            const auto delta = [&](float from, float to) {
                std::optional<Actors::AnimationDelta> found;
                RunOnMainThread([&] { found = Actors::GetAnimationDelta(id, clipId, from, to); });
                return found;
            };
            const auto whole = delta(0.0f, 1.0f);
            const auto end = motion ? ActorLogic::GetRootMotion(*motion, 1.0f) : glm::vec3(0.0f);
            const auto text = std::format(
                "actor {} clip {} {}: root motion {}, measured {} ({} samples, a play ends at {:.2f} {:.2f} turned "
                "{:.2f}); the game's delta {}; the catalog's {:.2f} {:.2f} turned {:.2f}",
                id, index, clip.animation, use, motion ? "yes" : "no", motion ? motion->samples.size() : 0, end.x,
                end.y, end.z,
                whole ? std::format(
                            "{:.2f} {:.2f} {:.2f}, rotation {:.4f} {:.4f} (turned {:.2f} as a quaternion, {:.2f} "
                            "as an angle)",
                            whole->translation.x, whole->translation.y, whole->translation.z, whole->rotation[0],
                            whole->rotation[1], glm::degrees(2.0f * std::atan2(whole->rotation[0], whole->rotation[1])),
                            glm::degrees(std::atan2(whole->rotation[0], whole->rotation[1])))
                      : "none",
                entry ? entry->motion.x : 0.0f, entry ? entry->motion.y : 0.0f, entry ? entry->turn : 0.0f);
            if (mode == "expect" && (args >> what, what != "end"))
            {
                Log(std::format("{} {} (expected {})", use == what ? "PASS" : "FAIL", text, what));
                return true;
            }
            if (mode == "check")
            {
                float distance = 0.05f, degrees = 0.5f;
                args >> distance >> degrees;
                float worst = 0.0f, worstTurn = 0.0f;
                std::string pairs;
                for (const auto& [from, to] : std::initializer_list<std::pair<float, float>>{
                         {0.0f, 0.5f}, {0.25f, 0.75f}, {0.5f, 1.0f}, {0.1f, 0.9f}, {0.0f, 1.0f}, {0.6f, 0.65f}})
                {
                    const auto game = delta(from, to);
                    if (!motion || !game)
                    {
                        worst = FLT_MAX;
                        continue;
                    }
                    const auto ours = ActorLogic::GetRootMotionBetween(*motion, from, to);
                    const auto turn = glm::degrees(2.0f * std::atan2(game->rotation[0], game->rotation[1]));
                    const auto off = glm::distance(glm::vec2(ours), glm::vec2(game->translation));
                    const auto turnOff = std::abs(std::remainder(ours.z - turn, 360.0f));
                    worst = std::max(worst, off);
                    worstTurn = std::max(worstTurn, turnOff);
                    pairs += std::format(" {}-{}: {:.2f} {:.2f} {:.2f} / game {:.2f} {:.2f} {:.2f};", from, to, ours.x,
                                         ours.y, ours.z, game->translation.x, game->translation.y, turn);
                }
                Log(std::format("{} rootmotion check {}: off by at most {:.3f} units and {:.3f} degrees ({} and {});{}",
                                worst <= distance && worstTurn <= degrees ? "PASS" : "FAIL", text, worst, worstTurn,
                                distance, degrees, pairs));
                return true;
            }
            if (mode == "expect" && what == "end")
            {
                float distance = 1.0f, degrees = 2.0f;
                args >> distance >> degrees;
                const auto duration = animation ? animation->duration : 0.0f;
                if (!animation || duration <= 0.0f || clip.speed <= 0.0f || clip.loop)
                {
                    Log("FAIL rootmotion expect end: needs a clip that plays its animation once: " + text);
                    return true;
                }
                // The game's delta from where the clip starts in its animation to its end, summed as the game sums a
                // blending tree's: while the clip blends in (over the clip before it) and while the clip after it
                // blends in over it, step by step weighed by the clip's weight (see ActorLogic::GetClipRootMotion);
                // and the whole play's delta unweighed, which a clip blending in from standing comes short of by what
                // the blend takes
                const auto& clips = actor->clips;
                const auto* next = index + 1 < clips.size() ? &clips[index + 1] : nullptr;
                const auto endTick = clip.start + static_cast<std::uint32_t>(std::ceil(
                                                      std::max(duration - clip.offset, 0.0f) / clip.speed * 1000.0f));
                const auto fraction = [&](std::uint32_t t) {
                    const auto limit = clip.length + (next && t >= next->start ? next->blendIn : 0u);
                    const auto elapsed = std::min(t - std::min(t, clip.start), limit);
                    return std::clamp((clip.offset + elapsed / 1000.0f * clip.speed) / duration, 0.0f, 1.0f);
                };
                const auto weight = [&](double t) {
                    if (next && t >= next->start)
                        return next->blendIn > 0 && t < next->start + next->blendIn
                                   ? 1.0 - (t - next->start) / next->blendIn
                                   : 0.0;
                    if (index > 0 && clip.blendIn > 0 && t < clip.start + clip.blendIn)
                        return (t - clip.start) / clip.blendIn;
                    return 1.0;
                };
                glm::dvec2 summed(0.0);
                double turned = 0.0;
                bool known = true;
                std::optional<Actors::AnimationDelta> whole;
                RunOnMainThread([&] {
                    whole = Actors::GetAnimationDelta(id, clipId, fraction(clip.start), 1.0f);
                    for (auto t = clip.start; t < endTick + 500; t += 10)
                    {
                        const auto amount = weight(t + 5.0);
                        const auto step = Actors::GetAnimationDelta(id, clipId, fraction(t), fraction(t + 10));
                        if (!step)
                        {
                            known = false;
                            return;
                        }
                        const auto angle = glm::radians(turned);
                        const glm::dvec2 moved(step->translation.x, step->translation.y);
                        summed += amount * glm::dvec2(std::cos(angle) * moved.x - std::sin(angle) * moved.y,
                                                      std::sin(angle) * moved.x + std::cos(angle) * moved.y);
                        turned += amount * glm::degrees(2.0 * std::atan2(step->rotation[0], step->rotation[1]));
                    }
                });
                if (!known || !whole)
                {
                    Log("FAIL rootmotion expect end: the game has no delta for it: " + text);
                    return true;
                }
                const auto start = Components::Actors::EvaluatePlacement(*actor, clip.start);
                const auto ended = Components::Actors::EvaluatePlacement(*actor, endTick + 500);
                const auto turn = [&](glm::dvec2 v) {
                    const auto yaw = glm::radians(static_cast<double>(start.yaw));
                    return glm::vec2(glm::dvec2(start.position) +
                                     glm::dvec2(std::cos(yaw) * v.x - std::sin(yaw) * v.y,
                                                std::sin(yaw) * v.x + std::cos(yaw) * v.y));
                };
                const auto expected = turn(summed);
                const auto expectedYaw = start.yaw + static_cast<float>(turned);
                const auto unweighed = turn({whole->translation.x, whole->translation.y});
                const auto off = glm::distance(glm::vec2(ended.position), expected);
                const auto turnOff = std::abs(std::remainder(ended.yaw - expectedYaw, 360.0f));
                Log(std::format(
                    "{} rootmotion end of actor {} clip {} {}: from {:.2f} {:.2f} yaw {:.2f} at {} to {:.2f} "
                    "{:.2f} yaw {:.2f} at {}; the game's delta summed over its blends puts it at {:.2f} {:.2f} "
                    "yaw {:.2f}: {:.3f} units and {:.3f} degrees off (at most {} and {}); the whole play's "
                    "delta at {:.2f} {:.2f} turned {:.2f}, {:.3f} units from where it ends",
                    off <= distance && turnOff <= degrees ? "PASS" : "FAIL", id, index, clip.animation,
                    start.position.x, start.position.y, start.yaw, clip.start, ended.position.x, ended.position.y,
                    ended.yaw, endTick + 500, expected.x, expected.y, expectedYaw, off, turnOff, distance, degrees,
                    unweighed.x, unweighed.y, glm::degrees(2.0f * std::atan2(whole->rotation[0], whole->rotation[1])),
                    glm::distance(glm::vec2(ended.position), unweighed)));
                return true;
            }
            Log("ROOTMOTION " + text);
            return true;
        }
        if (name == "placetrace")
        {
            // placetrace <actor> <from> <to> [expect <distance> <degrees>]: logs the most the actor's placement (as
            // core works it out, ActorLogic::EvaluatePlacement) changes from one millisecond to the next between two
            // ticks, where, and where it is at both; with expect, PASS when it moves and turns at most that much a
            // millisecond. placetrace <actor> fit: fits its walking clip to its path (FitOnlyWalk). placetrace <actor>
            // end: logs where its walking clip's fit (FitWalk) has it get to its path's
            // end and whether it is there, PASS when it is (within 0.01) at that tick, the walk fitted again ends at
            // the same tick (within 1 ms) and it isn't there a millisecond before with the clip a millisecond shorter.
            // placetrace <actor> path: logs its rounded path's pieces
            std::string which, from;
            args >> which >> from;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor)
            {
                Log("FAIL placetrace: no such actor");
                return true;
            }
            const auto lookup = Components::Actors::GetAnimationLookup();
            if (from == "fit")
            {
                // placetrace <actor> fit: fits the clip that walks its path to it, as changing its speed does
                // (ActorLogic::FitOnlyWalk)
                auto fitted = *actor;
                Components::Actors::FitOnlyWalk(fitted);
                Components::Actors::Update(fitted);
                std::string clips;
                for (const auto& clip : fitted.clips)
                    clips += std::format(" {} {}-{};", clip.animation, clip.start, clip.End());
                Log(std::format("PLACETRACE actor {} fitted:{}", fitted.id, clips));
                return true;
            }
            if (from == "path")
            {
                const auto path = ActorLogic::GetRoundedPath(*actor, lookup);
                std::string pieces;
                for (const auto& piece : path.pieces)
                    pieces += piece.radius > 0.0f
                                  ? std::format(" {}arc r {:.2f} {:.2f} long at {:.2f};", piece.stop ? "stop " : "",
                                                piece.radius, piece.length, piece.distance)
                                  : std::format(" straight {:.2f} long at {:.2f};", piece.length, piece.distance);
                Log(std::format("PLACETRACE actor {} path of {} points, {:.2f} long:{}", actor->id, actor->path.size(),
                                path.length, pieces));
                return true;
            }
            if (from == "end")
            {
                // its walking clip: the last that walks
                std::optional<std::size_t> walking;
                for (std::size_t k = 0; k < actor->clips.size(); ++k)
                    if (Components::Actors::GetClipWalk(*actor, actor->clips[k]).angle)
                        walking = k;
                if (!walking || actor->movement != Types::ActorMovement::Path || actor->path.empty())
                {
                    Log("FAIL placetrace end: no clip walks a path");
                    return true;
                }
                const auto& walk = actor->clips[*walking];
                const auto next = *walking + 1 < actor->clips.size() && actor->clips[*walking + 1].start == walk.End()
                                      ? std::optional(*walking + 1)
                                      : std::nullopt;
                const auto end = walk.End() + (next ? actor->clips[*next].blendIn : 0u);
                const auto length = ActorLogic::GetPathLength(*actor, lookup);
                const auto reached = ActorLogic::GetPathProgress(*actor, end, lookup);
                const auto there =
                    glm::distance(Components::Actors::EvaluatePlacement(*actor, end).position,
                                  ActorLogic::PlaceOnPath(ActorLogic::GetRoundedPath(*actor, lookup), length).position);
                auto refitted = *actor;
                ActorLogic::FitWalk(refitted, walk.id, lookup, actor->pathSpeed);
                const auto refit = std::ranges::find(refitted.clips, walk.id, &Types::ActorClip::id)->End();
                auto shorter = *actor;
                shorter.clips[*walking].length -= 1;
                if (next)
                    shorter.clips[*next] = ActorLogic::TrimClipStart(shorter.clips[*next], walk.End() - 1, 1);
                const auto before = ActorLogic::GetPathProgress(shorter, end - 1, lookup);
                const bool pass =
                    reached >= length - 1e-3f && there <= 0.01f &&
                    std::abs(static_cast<std::int64_t>(refit) - static_cast<std::int64_t>(walk.End())) <= 1 &&
                    before < length;
                Log(std::format(
                    "{} placetrace end: actor {} walks its path ({:.2f} long) with clip {} to {}, at the end "
                    "at {} ({:.3f} walked, {:.3f} from its end), fitted again to {}; a millisecond shorter "
                    "{:.3f} walked at {}",
                    pass ? "PASS" : "FAIL", actor->id, length, *walking, walk.End(), end, reached, there, refit, before,
                    end - 1));
                return true;
            }
            std::string expect;
            std::uint32_t to = 0;
            float distance = FLT_MAX, degrees = FLT_MAX;
            to = static_cast<std::uint32_t>(std::stoul(from));
            std::uint32_t last = 0;
            args >> last >> expect >> distance >> degrees;
            auto before = Components::Actors::EvaluatePlacement(*actor, to);
            const auto first = before;
            float moved = 0.0f, turned = 0.0f;
            std::uint32_t movedAt = to, turnedAt = to;
            for (auto tick = to + 1; tick <= last; ++tick)
            {
                const auto now = Components::Actors::EvaluatePlacement(*actor, tick);
                if (const auto step = glm::distance(now.position, before.position); step > moved)
                {
                    moved = step;
                    movedAt = tick;
                }
                if (const auto step = std::abs(std::remainder(now.yaw - before.yaw, 360.0f)); step > turned)
                {
                    turned = step;
                    turnedAt = tick;
                }
                before = now;
            }
            const auto text = std::format(
                "actor {} from {} ({:.2f} {:.2f} {:.2f} yaw {:.2f}) to {} ({:.2f} {:.2f} {:.2f} yaw {:.2f}): moves at "
                "most {:.4f} a millisecond (at {}), turns at most {:.4f} degrees (at {})",
                actor->id, to, first.position.x, first.position.y, first.position.z, first.yaw, last, before.position.x,
                before.position.y, before.position.z, before.yaw, moved, movedAt, turned, turnedAt);
            if (expect == "expect")
                Log(std::format("{} placetrace {} (at most {} and {})",
                                moved <= distance && turned <= degrees ? "PASS" : "FAIL", text, distance, degrees));
            else
                Log("PLACETRACE " + text);
            return true;
        }
        return false;
    }

    // The differences between two copies of an animation (the game's and the animation store's):
    // the struct but its pointers (and the padding the pack zeroes), the
    // name, the bones' names (script strings), the notetracks, the delta part and every data stream; none when they are
    // the same
    std::vector<std::string> CompareParts(const Structures::XAnimParts& game, const Structures::XAnimParts& store)
    {
        std::vector<std::string> differences;
        const auto a = reinterpret_cast<const std::uint8_t*>(&game);
        const auto b = reinterpret_cast<const std::uint8_t*>(&store);
        for (std::size_t at = 0x04; at < offsetof(Structures::XAnimParts, names); ++at)
            if ((at < 0x25 || at >= 0x28) && a[at] != b[at])
                differences.push_back(std::format("byte {:#x} {}/{}", at, a[at], b[at]));
        if (std::string_view(game.name) != store.name)
            differences.push_back("name");
        const auto same = [&](std::string_view what, const void* x, const void* y, std::size_t size) {
            if (size == 0)
                return;
            if (!x || !y)
            {
                if (x != y)
                    differences.push_back(std::format("{} (one is null)", what));
                return;
            }
            if (std::memcmp(x, y, size) != 0)
                differences.push_back(std::string(what));
        };
        const std::size_t indexSize = game.numframes >= 256 ? 2 : 1;
        same("bone names", game.names, store.names, game.boneCount[Structures::PART_TYPE_ALL] * 2u);
        for (std::size_t i = 0; game.notify && store.notify && i < game.notifyCount; ++i)
            if (game.notify[i].name != store.notify[i].name || game.notify[i].time != store.notify[i].time)
                differences.push_back(std::format("notetrack {}", i));
        same("dataByte", game.dataByte, store.dataByte, game.dataByteCount);
        same("dataShort", game.dataShort, store.dataShort, game.dataShortCount * 2u);
        same("dataInt", game.dataInt, store.dataInt, game.dataIntCount * 4u);
        same("randomDataShort", game.randomDataShort, store.randomDataShort, game.randomDataShortCount * 2u);
        same("randomDataByte", game.randomDataByte, store.randomDataByte, game.randomDataByteCount);
        same("randomDataInt", game.randomDataInt, store.randomDataInt, game.randomDataIntCount * 4u);
        same("indices", game.indices, store.indices, game.indexCount * indexSize);
        // the delta part, {translation, rotation about z}: each its header, frame indices and keys (see
        // AnimationPackLogic::Plan)
        const auto pointerAt = [](const void* base, std::size_t at) {
            std::uint32_t pointer = 0;
            std::memcpy(&pointer, static_cast<const std::uint8_t*>(base) + at, sizeof(pointer));
            return reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(pointer));
        };
        if (!game.deltaPart || !store.deltaPart)
        {
            if (game.deltaPart != store.deltaPart)
                differences.push_back("delta part (one is null)");
            return differences;
        }
        const auto gameTrans = pointerAt(game.deltaPart, 0), storeTrans = pointerAt(store.deltaPart, 0);
        if (!gameTrans || !storeTrans)
        {
            if (gameTrans != storeTrans)
                differences.push_back("delta translation (one is null)");
        }
        else
        {
            std::uint16_t size = 0;
            std::memcpy(&size, gameTrans, sizeof(size));
            if (size == 0)
            {
                same("delta translation", gameTrans, storeTrans, 16);
            }
            else
            {
                same("delta translation", gameTrans, storeTrans, 0x1C);
                same("delta translation's indices", gameTrans + 0x20, storeTrans + 0x20, (size + 1u) * indexSize);
                same("delta translation's keys", pointerAt(gameTrans, 0x1C), pointerAt(storeTrans, 0x1C),
                     (size + 1u) * (gameTrans[2] ? 3u : 6u));
            }
        }
        const auto gameQuat = pointerAt(game.deltaPart, 4), storeQuat = pointerAt(store.deltaPart, 4);
        if (!gameQuat || !storeQuat)
        {
            if (gameQuat != storeQuat)
                differences.push_back("delta rotation (one is null)");
        }
        else
        {
            std::uint16_t size = 0;
            std::memcpy(&size, gameQuat, sizeof(size));
            if (size == 0)
            {
                same("delta rotation", gameQuat, storeQuat, 8);
            }
            else
            {
                same("delta rotation", gameQuat, storeQuat, 4);
                same("delta rotation's indices", gameQuat + 8, storeQuat + 8, (size + 1u) * indexSize);
                same("delta rotation's keys", pointerAt(gameQuat, 4), pointerAt(storeQuat, 4), (size + 1u) * 4u);
            }
        }
        return differences;
    }

    // An entry of the animation pack laid out as the store lays it out, without script strings (the names stay indices
    // into the entry's strings), against what the catalog says of it: its name, frames, rate, loop, delta, notetracks,
    // bones and rig; why not, empty when it agrees
    std::string CheckPackEntry(const std::string& key, const Types::AnimationEntry& entry,
                               const Types::AnimationCatalog& catalog)
    {
        namespace Pack = AnimationPackLogic;
        const auto inflated = AnimationStore::ReadEntry(key);
        if (!inflated)
            return "doesn't inflate";
        const auto read = Pack::ReadEntry(*inflated);
        if (!read)
            return "doesn't read";
        std::string why;
        const auto layout = Pack::Plan(read->parts, &why);
        if (!layout)
            return why;
        std::vector<std::uint8_t> memory(layout->size + Pack::MEMORY_ALIGNMENT);
        const auto aligned = reinterpret_cast<std::uint8_t*>(
            (reinterpret_cast<std::uintptr_t>(memory.data()) + Pack::MEMORY_ALIGNMENT - 1) &
            ~(Pack::MEMORY_ALIGNMENT - 1));
        if (!Pack::Write(*layout, read->parts, read->strings.size(), aligned,
                         [](std::uint16_t index) { return index; }))
            return "a name isn't in its strings";
        const auto& parts = *reinterpret_cast<const Structures::XAnimParts*>(aligned);
        // the names as the catalog has them: animations.py reads their bytes as latin-1, which the json makes UTF-8
        // (two notetracks of the game have curly quotes)
        std::vector<std::string> strings;
        for (const auto& raw : read->strings)
        {
            std::string text;
            for (const auto c : raw)
            {
                const auto byte = static_cast<std::uint8_t>(c);
                if (byte < 0x80)
                    text += static_cast<char>(byte);
                else
                    text += {static_cast<char>(0xC0 | byte >> 6), static_cast<char>(0x80 | (byte & 0x3F))};
            }
            strings.push_back(std::move(text));
        }
        if (layout->name != entry.AssetName())
            why += " name";
        if (parts.numframes != entry.frames || std::abs(parts.framerate - entry.rate) > 1e-3f)
            why += " frames/rate";
        if (parts.loop != entry.loop || parts.delta != entry.delta)
            why += " loop/delta";
        std::vector<Types::ActorNotetrack> notetracks;
        for (std::size_t i = 0; parts.notify && i < parts.notifyCount; ++i)
            if (!strings[parts.notify[i].name].empty())
                notetracks.push_back({strings[parts.notify[i].name], std::clamp(parts.notify[i].time, 0.0f, 1.0f)});
        std::ranges::stable_sort(notetracks, {}, &Types::ActorNotetrack::time);
        if (!std::ranges::equal(notetracks, entry.notetracks, [](const auto& x, const auto& y) {
                return x.name == y.name && std::abs(x.time - y.time) <= 1e-4f;
            }))
            why += " notetracks";
        std::vector<std::string> bones, listed;
        std::vector<std::string_view> names;
        for (std::size_t i = 0; parts.names && i < parts.boneCount[Structures::PART_TYPE_ALL]; ++i)
        {
            bones.push_back(strings[parts.names[i]]);
            names.push_back(strings[parts.names[i]]);
        }
        if (entry.boneSet < catalog.boneSets.size())
            for (const auto bone : catalog.boneSets[entry.boneSet])
                listed.push_back(catalog.bones[bone]);
        std::ranges::sort(bones);
        std::ranges::sort(listed);
        if (bones != listed)
            why += " bones";
        const auto rig = PoseLogic::ClassifyAnimation(names);
        if (rig.kind != entry.kind || rig.family != entry.family)
            why += " kind/family";
        return why;
    }

    // commands for the animation store (see RunCharacterZoneCommand for why they are
    // apart). Animations are named as the catalog names them (a copy that differs has its zone after a space), and the
    // log puts a colon after a name (send.ps1 takes a screenshot for "shot ")
    bool RunStoreCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "store")
            return false;
        std::string what;
        args >> what;
        const auto rest = [&] {
            std::string text;
            std::getline(args, text);
            const auto start = text.find_first_not_of(' ');
            return start == std::string::npos ? std::string()
                                              : text.substr(start, text.find_last_not_of(' ') - start + 1);
        };
        const auto stats = [] {
            const auto stats = AnimationStore::GetStats();
            std::uint32_t freeSlots = 0;
            RunOnMainThread([&] { freeSlots = ContentPacks::CountFreeSlots(Structures::ASSET_TYPE_XANIMPARTS); });
            return std::format(
                "ready {} ({}), pack {}, loaded {} ({:.2f} MB, {} referenced, {} wanted), retired {} "
                "({:.2f} MB), budget {:.2f} MB, loads {} in {:.1f} ms, unloads {}, frees {}, failures {}, "
                "script strings {}, xanim pool free {}",
                stats.ready ? 1 : 0, stats.error.empty() ? "ok" : stats.error, stats.packEntries, stats.loaded,
                stats.loadedBytes / 1048576.0, stats.referenced, stats.wanted, stats.retired,
                stats.retiredBytes / 1048576.0, stats.budget / 1048576.0, stats.loads, stats.loadMilliseconds,
                stats.unloads, stats.frees, stats.failures, stats.scriptStrings, freeSlots);
        };
        if (what.empty() || what == "stats")
        {
            // store [stats]: what the store has loaded and done
            Log("STORE " + stats());
        }
        else if (what == "list")
        {
            // store list: the animations it has loaded, with their memory, references and whether the scene names them
            for (const auto& loaded : AnimationStore::GetLoaded())
                Log(std::format("STORE loaded {}: {} bytes, {} references{}", loaded.key, loaded.bytes,
                                loaded.references, loaded.wanted ? ", wanted" : ""));
            Log("STORE " + stats());
        }
        else if (what == "load")
        {
            // store load <animation>: loads one from the pack (it stays loaded until it goes as the least recently
            // used); PASS when it did
            const auto key = rest();
            std::string why;
            bool loaded = false;
            std::uint32_t bytes = 0;
            const auto started = std::chrono::steady_clock::now();
            RunOnMainThread([&] {
                const auto reference = AnimationStore::Acquire(key, &why);
                loaded = static_cast<bool>(reference);
                for (const auto& each : AnimationStore::GetLoaded())
                    if (each.key == key)
                        bytes = each.bytes;
            });
            const auto took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
            Log(std::format("{} store load {}: {} ({} bytes, {:.2f} ms with the wait for the main thread)",
                            loaded ? "PASS" : "FAIL", key, loaded ? "loaded" : why, bytes, took.count()));
        }
        else if (what == "loadmany")
        {
            // store loadmany <n> [<start of their names>]: loads n animations of the pack the game hasn't loaded (nor
            // the store), one after the other; PASS when all of them did, and the game's pool of animations has as many
            // free slots after as before
            std::size_t count = 0;
            args >> count;
            const auto prefix = rest();
            std::uint32_t before = 0, after = 0;
            std::size_t loaded = 0, tried = 0;
            std::string failures;
            const auto started = std::chrono::steady_clock::now();
            RunOnMainThread([&] {
                before = ContentPacks::CountFreeSlots(Structures::ASSET_TYPE_XANIMPARTS);
                std::set<std::string> already;
                for (const auto& each : AnimationStore::GetLoaded())
                    already.insert(each.key);
                for (const auto& key : AnimationStore::GetKeys())
                {
                    if (tried >= count)
                        break;
                    if (!key.starts_with(prefix) || already.contains(key) ||
                        ContentPacks::FindAsset(Structures::ASSET_TYPE_XANIMPARTS, key.c_str()))
                        continue;
                    ++tried;
                    std::string why;
                    if (AnimationStore::Acquire(key, &why))
                        ++loaded;
                    else if (failures.size() < 300)
                        failures += std::format(" {}: {}", key, why);
                }
                after = ContentPacks::CountFreeSlots(Structures::ASSET_TYPE_XANIMPARTS);
            });
            const auto took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
            Log(std::format("{} store loadmany {} {}: loaded {} of {} in {:.1f} ms, xanim pool free {} -> {}{}",
                            loaded == count && tried == count && before == after ? "PASS" : "FAIL", count, prefix,
                            loaded, tried, took.count(), before, after, failures));
            Log("STORE " + stats());
        }
        else if (what == "budget")
        {
            // store budget <MB>|default: how much the animations nothing uses may take before the least recently used
            // go (64 MB)
            std::string value;
            args >> value;
            const auto bytes = value == "default" ? AnimationPackLogic::DEFAULT_BUDGET
                                                  : static_cast<std::uint64_t>(std::stod(value) * 1048576.0);
            AnimationStore::SetBudget(bytes);
            Log(std::format("STORE budget {:.2f} MB", bytes / 1048576.0));
        }
        else if (what == "prefer" || what == "ignore")
        {
            // store prefer on|off: the store's copy even of the animations the game has loaded (for actors built
            // after); store ignore on|off: as if there were no animation pack
            std::string value;
            args >> value;
            if (what == "prefer")
                AnimationStore::SetPreferStore(value == "on");
            else
                AnimationStore::SetIgnored(value == "on");
            Log(std::format("STORE {} {}", what, value));
        }
        else if (what == "compare")
        {
            // store compare <animation>: the game's copy against the store's, field by field and array by array; PASS
            // when they are the same
            const auto key = rest();
            std::vector<std::string> differences;
            bool both = false;
            RunOnMainThread([&] {
                const auto game = static_cast<const Structures::XAnimParts*>(
                    ContentPacks::FindAsset(Structures::ASSET_TYPE_XANIMPARTS, key.c_str()));
                const auto stored = AnimationStore::Acquire(key);
                both = game && stored;
                if (both)
                    differences = CompareParts(*game, *stored.Parts());
            });
            std::string text;
            for (const auto& difference : differences)
                text += " " + difference;
            Log(std::format("{} store compare {}: {}", both && differences.empty() ? "PASS" : "FAIL", key,
                            !both                 ? "the game or the store hasn't it"
                            : differences.empty() ? "the same"
                                                  : "differ in" + text));
        }
        else if (what == "check")
        {
            // store check [<n>|all]: lays out the pack's first n animations (or all of them) as the store does, without
            // script strings, against the catalog (name, frames, rate, loop, delta, notetracks, bones, rig); PASS when
            // all but those that don't read as an XAnimParts agree, and logs those
            std::string count;
            args >> count;
            const auto limit = count == "all" || count.empty() ? SIZE_MAX : std::stoul(count);
            const auto& catalog = Components::Actors::GetAnimationCatalog();
            std::size_t checked = 0, unreadable = 0, differ = 0;
            const auto started = std::chrono::steady_clock::now();
            for (const auto& key : AnimationStore::GetKeys())
            {
                if (checked >= limit)
                    break;
                const auto entry = catalog.Find(key);
                if (!entry)
                    continue;
                ++checked;
                const auto why = CheckPackEntry(key, *entry, catalog);
                if (why.empty())
                    continue;
                // (the zone reader's mistakes, which its counts tell)
                const bool readable = why.starts_with(" ");
                (readable ? differ : unreadable) += 1;
                if (differ + unreadable <= 20)
                    Log(std::format("STORE {} {}:{}{}", readable ? "differs" : "unreadable", key, readable ? "" : " ",
                                    why));
            }
            const auto took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
            Log(std::format("{} store check: {} laid out in {:.0f} ms, {} differ from the catalog, {} don't read",
                            differ == 0 ? "PASS" : "FAIL", checked, took.count(), differ, unreadable));
        }
        else if (what == "expect")
        {
            // store expect loaded|absent <animation> | refs <n> <animation> | clips <actor> | within | ready | unready:
            // PASS when the store has it loaded or not, with n references; when every store animation of the actor's
            // tree is loaded with a reference; when what it has loaded nothing uses takes at most the budget; when the
            // pack can be loaded from or not
            std::string kind;
            args >> kind;
            const auto loaded = AnimationStore::GetLoaded();
            const auto find = [&](const std::string& key) {
                return std::ranges::find(loaded, key, &AnimationStore::LoadedInfo::key);
            };
            if (kind == "loaded" || kind == "absent")
            {
                const auto key = rest();
                const bool has = find(key) != loaded.end();
                Log(std::format("{} store expect {} {}: {}", has == (kind == "loaded") ? "PASS" : "FAIL", kind, key,
                                has ? "loaded" : "not loaded"));
            }
            else if (kind == "refs")
            {
                std::uint32_t expected = 0;
                args >> expected;
                const auto key = rest();
                const auto it = find(key);
                const auto references = it != loaded.end() ? it->references : 0;
                Log(std::format("{} store expect refs {} {}: {}{}",
                                it != loaded.end() && references == expected ? "PASS" : "FAIL", expected, key,
                                references, it != loaded.end() ? "" : " (not loaded)"));
            }
            else if (kind == "clips")
            {
                std::string which;
                args >> which;
                const auto id = ParseActorId(which);
                std::vector<std::string> keys;
                RunOnMainThread([&] { keys = Actors::GetStoredAnimations(id); });
                std::string missing, names;
                for (const auto& key : keys)
                {
                    names += key + ": ";
                    const auto it = find(key);
                    if (it == loaded.end() || it->references == 0)
                        missing += key + ": ";
                }
                Log(std::format("{} store expect clips {}: {} of the store ({}){}",
                                !keys.empty() && missing.empty() ? "PASS" : "FAIL", which, keys.size(), names,
                                missing.empty() ? "" : ", not loaded with a reference: " + missing));
            }
            else if (kind == "within")
            {
                const auto stats = AnimationStore::GetStats();
                std::uint64_t unused = 0;
                for (const auto& each : loaded)
                    unused += each.references == 0 && !each.wanted ? each.bytes : 0;
                Log(std::format("{} store expect within: {:.2f} MB loaded, {:.2f} MB of it unused, budget {:.2f} MB",
                                stats.loadedBytes <= stats.budget || unused == 0 ? "PASS" : "FAIL",
                                stats.loadedBytes / 1048576.0, unused / 1048576.0, stats.budget / 1048576.0));
            }
            else if (kind == "ready" || kind == "unready")
            {
                const bool ready = AnimationStore::IsReady();
                Log(std::format("{} store expect {}: {}", ready == (kind == "ready") ? "PASS" : "FAIL", kind,
                                ready ? "ready" : AnimationStore::GetError()));
            }
            else
            {
                Log("FAIL store expect: what?");
            }
        }
        else
        {
            Log("FAIL store: no such command " + what);
        }
        return true;
    }

    // bonecam steps <frames>|capture [label [expect <most uneven> <most still frames> [<most behind>] | dump]]: over that
    // many frames, or those of the last capture, how the camera moved: the frames' rate, the speed that played, the ms
    // each frame gave, and per frame the distance and turn of the camera and the distance of the bone as drawn: their
    // mean, most, how many frames didn't move, and how uneven they are (the mean change of a frame's step from the one
    // before, by the mean step: 0 is perfectly even, 2 is a step every other frame), and how far the camera was behind
    // the bone (without Steady); PASS when the camera's are at most that; dump logs each frame
    void RunBoneCameraSteps(std::istringstream& args)
    {
        std::string what, label, expect;
        double mostUneven = 0, mostBehind = 1e9;
        int mostStill = 0;
        args >> what >> label >> expect >> mostUneven >> mostStill >> mostBehind;
        auto frames = Hooks::Camera::GetBoneCameraFrames();
        if (what == "capture")
        {
            const auto end = std::find_if(frames.rbegin(), frames.rend(), [](const auto& frame) {
                                 return frame.capturing;
                             }).base();
            const auto first = std::find_if(std::make_reverse_iterator(end), frames.rend(), [](const auto& frame) {
                                   return !frame.capturing;
                               }).base();
            frames = {first, end};
            // (the first frames of a capture are drawn while the demo still stands)
            while (frames.size() > 1 && frames[1].timelineTick == frames[0].timelineTick)
                frames.erase(frames.begin());
        }
        else
        {
            const auto count = static_cast<std::size_t>(std::atoi(what.c_str()));
            const auto start = mainThreadFrames.load();
            while (mainThreadFrames.load() < start + static_cast<std::uint32_t>(count) + 1)
                Sleep(5);
            frames = Hooks::Camera::GetBoneCameraFrames();
            if (frames.size() > count)
                frames.erase(frames.begin(), frames.end() - count);
        }
        if (frames.size() < 3)
            return Log("FAIL bonecam steps: too few frames");
        std::map<std::int64_t, int> msCounts;
        std::vector<float> moved, turned, boneMoved;
        for (std::size_t i = 1; i < frames.size(); ++i)
        {
            const auto& from = frames[i - 1];
            const auto& to = frames[i];
            ++msCounts[static_cast<std::int64_t>(to.timelineTick) - from.timelineTick];
            moved.push_back(glm::distance(from.position, to.position));
            float turn = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
                turn = std::max(turn, std::abs(std::remainder(to.angles[axis] - from.angles[axis], 360.0f)));
            turned.push_back(turn);
            if (from.placedFrom && to.placedFrom)
                boneMoved.push_back(glm::distance(from.placedFrom->position, to.placedFrom->position));
        }
        struct Summary
        {
            double mean = 0, most = 0, uneven = 0;
            int still = 0;
        };
        const auto summarize = [](const std::vector<float>& steps) {
            double sum = 0, change = 0;
            Summary summary;
            for (std::size_t i = 0; i < steps.size(); ++i)
            {
                sum += steps[i];
                summary.most = std::max<double>(summary.most, steps[i]);
                summary.still += steps[i] < 1e-5f ? 1 : 0;
                if (i > 0)
                    change += std::abs(steps[i] - steps[i - 1]);
            }
            summary.mean = sum / std::max<std::size_t>(steps.size(), 1);
            summary.uneven = summary.mean > 0 ? change / std::max<std::size_t>(steps.size() - 1, 1) / summary.mean : 0.0;
            return summary;
        };
        const auto describe = [](const Summary& summary) {
            return std::format("mean {:.4f} most {:.4f} still {} uneven {:.2f}", summary.mean, summary.most, summary.still,
                               summary.uneven);
        };
        std::string ms;
        for (const auto& [step, times] : msCounts)
            ms += std::format(" {}:{}", step, times);
        const auto seconds = (frames.back().time - frames.front().time) / 1000.0;
        const auto ticks = static_cast<double>(frames.back().timelineTick) - frames.front().timelineTick;
        float behind = 0.0f;
        for (const auto& frame : frames)
        {
            if (!frame.placedFrom || frame.settings.steady > 0.0f)
                continue;
            const auto placed = Components::BoneCameraLogic::Place(
                {frame.placedFrom->position, frame.placedFrom->axes},
                Components::BoneCameraLogic::MakeFrame(frame.placedFrom->origin, frame.placedFrom->yaw), frame.settings);
            behind = std::max(behind, glm::distance(placed.position, frame.position));
            if (expect == "dump")
                Log(std::format("STEP tick {} time {:.1f} bone {:.3f} {:.3f} {:.3f} origin {:.3f} {:.3f} {:.3f} camera {:.3f} "
                                "{:.3f} {:.3f}",
                                frame.timelineTick, frame.time, frame.placedFrom->position.x,
                                frame.placedFrom->position.y, frame.placedFrom->position.z, frame.placedFrom->origin.x,
                                frame.placedFrom->origin.y, frame.placedFrom->origin.z, frame.position.x, frame.position.y,
                                frame.position.z));
        }
        const auto camera = summarize(moved);
        Log(std::format("STEPS {} fps {:.0f} played {:.4f} ms{} | moved {} | turned {} | bone {} | behind {:.4f}", label,
                        (frames.size() - 1) / seconds, ticks / 1000.0 / seconds, ms, describe(camera),
                        describe(summarize(turned)), describe(summarize(boneMoved)), behind));
        if (expect == "expect")
            Log(std::format(
                "{} bonecam steps {}: uneven {:.2f} (at most {}), {} frames still (at most {}), {:.4f} behind (at most {})",
                camera.uneven <= mostUneven && camera.still <= mostStill && behind <= mostBehind && camera.mean > 0.0
                    ? "PASS"
                    : "FAIL",
                label, camera.uneven, mostUneven, camera.still, mostStill, behind, mostBehind));
    }

    // bonecam keycheck <frames> [tolerance] | bonecam keycheck capture <fps> [tolerance]: over that many frames, or
    // those of the last capture, PASS when each frame's offset, rotation and FOV were the keys' at that frame's own
    // timeline tick and the camera was placed from them on the bone as drawn (without Steady); with capture also when
    // each frame was a frame (1000/fps ms) after the one before. Logs how far the UI's copy of the settings was.
    void RunBoneCameraKeyCheck(std::istringstream& args)
    {
        std::string what;
        args >> what;
        const bool capture = what == "capture";
        double fps = 50;
        if (capture)
            args >> fps;
        float tolerance = 0.01f;
        args >> tolerance;
        auto frames = Hooks::Camera::GetBoneCameraFrames();
        if (capture)
        {
            // the last capture's frames: the latest run of frames recorded while capturing
            const auto end = std::find_if(frames.rbegin(), frames.rend(), [](const auto& frame) {
                                 return frame.capturing;
                             }).base();
            const auto start = std::find_if(std::make_reverse_iterator(end), frames.rend(), [](const auto& frame) {
                                   return !frame.capturing;
                               }).base();
            frames = {start, end};
        }
        else
        {
            const auto count = std::atoi(what.c_str());
            const auto start = mainThreadFrames.load();
            while (mainThreadFrames.load() < start + static_cast<std::uint32_t>(count) + 1)
                Sleep(10);
            frames = Hooks::Camera::GetBoneCameraFrames();
            if (frames.size() > static_cast<std::size_t>(count))
                frames.erase(frames.begin(), frames.end() - count);
        }
        Components::BoneCameraLogic::Keys keys;
        RunOnRenderThread([&] { keys = Components::BoneCamera::CopyKeys(); });
        const auto keyed = Components::BoneCameraLogic::GetKeyed(keys);
        // how far two settings' keyed fields are apart
        const auto apart = [&](const Types::BoneCameraSettings& a, const Types::BoneCameraSettings& b) {
            float off = 0.0f;
            if (keyed.offset)
                off = std::max(off, glm::distance(a.positionOffset, b.positionOffset));
            if (keyed.rotation)
                off = std::max(off, glm::distance(a.rotationOffset, b.rotationOffset));
            if (keyed.fov)
                off = std::max(off, std::abs(a.fov - b.fov));
            return off;
        };
        float keyOff = 0.0f, cameraOff = 0.0f, uiOff = 0.0f;
        int checked = 0, uiFrames = 0, wrongSteps = 0, changed = 0;
        std::optional<Hooks::Camera::BoneCameraFrame> previous;
        for (const auto& frame : frames)
        {
            if (!frame.placedFrom)
                continue;
            ++checked;
            const auto expected =
                Components::BoneCameraLogic::ApplyKeys(frame.settings, keys, static_cast<double>(frame.timelineTick));
            keyOff = std::max(keyOff, apart(frame.settings, expected));
            if (const auto ui = apart(frame.fromUi, frame.settings); ui > tolerance)
            {
                ++uiFrames;
                uiOff = std::max(uiOff, ui);
            }
            if (frame.settings.steady <= 0.0f)
            {
                const auto placed = Components::BoneCameraLogic::Place(
                    {frame.placedFrom->position, frame.placedFrom->axes},
                    Components::BoneCameraLogic::MakeFrame(frame.placedFrom->origin, frame.placedFrom->yaw),
                    frame.settings);
                cameraOff = std::max(cameraOff, glm::distance(placed.position, frame.position));
            }
            if (previous)
            {
                changed += apart(previous->settings, frame.settings) > 0.0f ? 1 : 0;
                // (the first frames of a capture are drawn while the demo still stands)
                if (capture && frame.timelineTick != previous->timelineTick &&
                    std::abs((static_cast<double>(frame.timelineTick) - previous->timelineTick) - 1000.0 / fps) > 1.0)
                    ++wrongSteps;
            }
            previous = frame;
        }
        const bool pass =
            keyed.Any() && checked > 0 && keyOff <= tolerance && cameraOff <= tolerance && wrongSteps == 0;
        const auto range = capture && !frames.empty() ? std::format(" of the capture from {} to {}",
                                                                    frames.front().timelineTick,
                                                                    frames.back().timelineTick)
                                                      : std::string();
        Log(std::format("{} bonecam keycheck: {} frames{} (keyed:{}{}{}), their keyed values off the keys at their own "
                        "tick at most {:.4f}, the camera off its offsets from the bone at most {:.4f}, {} changed from "
                        "the frame before, {} not a frame after the one before; the UI's copy was off in {} frames, "
                        "at most {:.4f}",
                        pass ? "PASS" : "FAIL", checked, range, keyed.offset ? " offset" : "",
                        keyed.rotation ? " rotation" : "", keyed.fov ? " fov" : "", keyOff, cameraOff, changed,
                        wrongSteps, uiFrames, uiOff));
    }

    // The bone camera's settings and what it was placed from
    bool RunBoneCameraCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "bonecam")
            return false;

        // bonecam: logs its settings, the bone as the UI has it and the latest frame's placement
        // bonecam target actor <actor> | player <client>, bone <name>, offset <x y z>, rotation <pitch yaw roll>,
        // fov <degrees>, steady <seconds>, turn <bone|level|facing>, show <0|1>: sets one, as the Camera tab does
        // bonecam bones [expect <bone>...]: logs the target's bones (waiting up to 3 s for them); PASS when it has those
        // bonecam check <frames> [tolerance]: over that many frames, PASS when the camera was placed from the bone as it
        // was drawn in the same frame (an actor's) and is where its offsets put it from there (without Steady)
        // bonecam expect at <x y z> <distance>: PASS when the latest frame's camera is that close to the point
        // bonecam expect bone <name>, bonecam expect target actor <actor> | player <client>: PASS when it is on that
        // bone, or follows that target and is the camera selected; bonecam expect fov <degrees> [tolerance]: PASS when
        // the Camera tab's Field of View is that (its keys' at the playhead, with keys)
        // bonecam keycheck ...: see RunBoneCameraKeyCheck; bonecam reset: Reset offsets, as the Camera tab's button
        std::string action;
        args >> action;
        auto& camera = static_cast<Components::BoneCamera&>(
            *Components::CameraManager::Get().GetCamera(Components::Camera::Mode::Bone));
        const auto describe = [](const Types::BoneCameraSettings& settings) {
            return std::format("target {} {} bone {} offset ({:.2f} {:.2f} {:.2f}) rotation ({:.2f} {:.2f} {:.2f}) fov "
                               "{:.1f} steady {:.2f} turn {} show {}",
                               settings.target.kind == Types::BoneCameraTargetKind::Actor ? "actor" : "player",
                               settings.target.id, settings.bone, settings.positionOffset.x, settings.positionOffset.y,
                               settings.positionOffset.z, settings.rotationOffset.x, settings.rotationOffset.y,
                               settings.rotationOffset.z, settings.fov, settings.steady,
                               magic_enum::enum_name(settings.turn), settings.showBone ? 1 : 0);
        };
        const auto describeBone = [](const std::optional<Types::TargetBone>& bone) {
            return bone ? std::format("({:.2f} {:.2f} {:.2f}) of one at ({:.1f} {:.1f} {:.1f}) yaw {:.1f}",
                                      bone->position.x, bone->position.y, bone->position.z, bone->origin.x,
                                      bone->origin.y, bone->origin.z, bone->yaw)
                        : std::string("none");
        };

        if (action.empty())
        {
            Types::BoneCameraSettings settings;
            std::optional<Types::TargetBone> bone;
            RunOnRenderThread([&] {
                settings = camera.GetSettings();
                bone = Mod::GetGameInterface()->GetTargetBone(settings.target, settings.bone);
            });
            const auto frames = Hooks::Camera::GetBoneCameraFrames();
            Log(std::format("BONECAM {}; the UI's bone {}; latest frame {}", describe(settings), describeBone(bone),
                            frames.empty() ? std::string("none")
                                           : std::format("tick {} camera ({:.2f} {:.2f} {:.2f}) angles ({:.2f} {:.2f} "
                                                         "{:.2f}) from {}",
                                                         frames.back().tick, frames.back().position.x,
                                                         frames.back().position.y, frames.back().position.z,
                                                         frames.back().angles.x, frames.back().angles.y,
                                                         frames.back().angles.z, describeBone(frames.back().placedFrom))));
        }
        else if (action == "bones")
        {
            Types::BoneCameraTarget target;
            RunOnRenderThread([&] { target = camera.GetSettings().target; });
            std::vector<std::string> bones;
            for (int i = 0; i < 60 && bones.empty(); ++i)
            {
                RunOnRenderThread([&] { bones = Mod::GetGameInterface()->GetTargetBones(target); });
                if (bones.empty())
                    Sleep(50);
            }
            std::string text;
            for (const auto& bone : bones)
                text += " " + bone;
            Log(std::format("BONECAM bones ({}):{}", bones.size(), text));
            std::string expect;
            args >> expect;
            if (expect == "expect")
            {
                std::vector<std::string> missing;
                for (std::string bone; args >> bone;)
                    if (std::ranges::find(bones, bone) == bones.end())
                        missing.push_back(bone);
                Log(std::format("{} bonecam bones{}", missing.empty() ? "PASS" : "FAIL",
                                missing.empty() ? std::string() : " lack " + missing.front()));
            }
        }
        else if (action == "keycheck")
        {
            RunBoneCameraKeyCheck(args);
        }
        else if (action == "steps")
        {
            RunBoneCameraSteps(args);
        }
        else if (action == "glide")
        {
            // bonecam glide on|off: the camera through the frames the clock stands still, or exactly on the bone
            std::string state;
            args >> state;
            Hooks::Camera::SetBoneCameraGlide(state != "off");
        }
        else if (action == "reset")
        {
            RunOnRenderThread([&] { camera.ResetOffsets(); });
        }
        else if (action == "check")
        {
            int count = 0;
            float tolerance = 0.01f;
            args >> count >> tolerance;
            const auto start = mainThreadFrames.load();
            while (mainThreadFrames.load() < start + static_cast<std::uint32_t>(count) + 1)
                Sleep(10);
            auto frames = Hooks::Camera::GetBoneCameraFrames();
            if (frames.size() > static_cast<std::size_t>(count))
                frames.erase(frames.begin(), frames.end() - count);
            float renderedOff = 0.0f, cameraOff = 0.0f, moved = 0.0f;
            int checked = 0;
            const Types::TargetBone* previous = nullptr;
            for (const auto& frame : frames)
            {
                if (!frame.placedFrom || !frame.rendered)
                    continue;
                ++checked;
                renderedOff = std::max(renderedOff, glm::distance(*frame.rendered, frame.position));
                if (frame.settings.steady <= 0.0f)
                {
                    const auto placed = Components::BoneCameraLogic::Place(
                        {frame.placedFrom->position, frame.placedFrom->axes},
                        Components::BoneCameraLogic::MakeFrame(frame.placedFrom->origin, frame.placedFrom->yaw),
                        frame.settings);
                    cameraOff = std::max(cameraOff, glm::distance(placed.position, frame.position));
                }
                if (previous)
                    moved = std::max(moved, glm::distance(previous->position, frame.placedFrom->position));
                previous = &*frame.placedFrom;
            }
            const bool pass = checked > 0 && renderedOff <= tolerance && cameraOff <= tolerance;
            Log(std::format("{} bonecam check: {} frames of {}, the view rendered off where the camera was put at most "
                            "{:.4f}, the camera off its offsets from the bone as drawn at most {:.4f}, the bone moved "
                            "up to {:.3f} a frame",
                            pass ? "PASS" : "FAIL", checked, frames.size(), renderedOff, cameraOff, moved));
        }
        else if (action == "expect")
        {
            std::string what;
            args >> what;
            Types::BoneCameraSettings settings;
            bool active = false;
            RunOnRenderThread([&] {
                settings = camera.GetSettings();
                active = Components::CameraManager::Get().GetActiveCamera()->GetMode() ==
                         Components::Camera::Mode::Bone;
            });
            if (what == "at")
            {
                glm::vec3 point{};
                float distance = 0.0f;
                args >> point.x >> point.y >> point.z >> distance;
                const auto frames = Hooks::Camera::GetBoneCameraFrames();
                const auto off = frames.empty() ? -1.0f : glm::distance(frames.back().position, point);
                Log(std::format("{} bonecam expect at: {:.2f} away", off >= 0.0f && off <= distance ? "PASS" : "FAIL",
                                off));
            }
            else if (what == "bone")
            {
                std::string bone;
                args >> bone;
                Log(std::format("{} bonecam expect bone {} (it is on {})", settings.bone == bone ? "PASS" : "FAIL", bone,
                                settings.bone));
            }
            else if (what == "fov")
            {
                float fov = 0.0f, tolerance = 0.1f;
                args >> fov >> tolerance;
                Log(std::format("{} bonecam expect fov {:.2f} (the Camera tab has {:.2f})",
                                std::abs(settings.fov - fov) <= tolerance ? "PASS" : "FAIL", fov, settings.fov));
            }
            else if (what == "target")
            {
                std::string kind, which;
                args >> kind >> which;
                const Types::BoneCameraTarget target =
                    kind == "actor" ? Types::BoneCameraTarget{Types::BoneCameraTargetKind::Actor,
                                                              static_cast<std::int32_t>(ParseActorId(which))}
                                    : Types::BoneCameraTarget{Types::BoneCameraTargetKind::Player,
                                                              std::atoi(which.c_str())};
                Log(std::format("{} bonecam expect target {} {} (the camera follows {} {}, {})",
                                settings.target == target && active ? "PASS" : "FAIL", kind, target.id,
                                settings.target.kind == Types::BoneCameraTargetKind::Actor ? "actor" : "player",
                                settings.target.id, active ? "selected" : "not selected"));
            }
            else
                Log("FAIL bonecam expect: no such expectation " + what);
        }
        else
        {
            bool known = true;
            RunOnRenderThread([&] {
                auto& settings = camera.GetSettings();
                if (action == "target")
                {
                    std::string kind, which;
                    args >> kind >> which;
                    settings.target = kind == "actor" ? Types::BoneCameraTarget{Types::BoneCameraTargetKind::Actor,
                                                                                static_cast<std::int32_t>(
                                                                                    ParseActorId(which))}
                                                      : Types::BoneCameraTarget{Types::BoneCameraTargetKind::Player,
                                                                                std::atoi(which.c_str())};
                }
                else if (action == "bone")
                    args >> settings.bone;
                else if (action == "offset")
                    args >> settings.positionOffset.x >> settings.positionOffset.y >> settings.positionOffset.z;
                else if (action == "rotation")
                    args >> settings.rotationOffset.x >> settings.rotationOffset.y >> settings.rotationOffset.z;
                else if (action == "fov")
                    args >> settings.fov;
                else if (action == "steady")
                    args >> settings.steady;
                else if (action == "turn")
                {
                    std::string turn;
                    args >> turn;
                    settings.turn = turn == "level"    ? Types::BoneCameraTurn::Level
                                    : turn == "facing" ? Types::BoneCameraTurn::Facing
                                                       : Types::BoneCameraTurn::Bone;
                }
                else if (action == "show")
                {
                    int show = 1;
                    args >> show;
                    settings.showBone = show != 0;
                }
                else
                    known = false;
            });
            if (!known)
                Log("FAIL bonecam: no such setting " + action);
        }
        return true;
    }

    // commands for the actor workflow (see RunCharacterZoneCommand for why they are apart)
    bool RunWorkflowCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "pathtiming" && name != "stretches" && name != "hitdeath" && name != "look" && name != "anchors" &&
            name != "notes")
            return false;
        std::string actorRef, action;
        args >> actorRef >> action;
        const auto actor = Components::Actors::GetActor(ParseActorId(actorRef));
        if (!actor)
        {
            Log(std::format("FAIL {}: no such actor {}", name, actorRef));
            return true;
        }
        const auto ticks = [](const std::optional<std::uint32_t>& tick) {
            return tick ? std::to_string(*tick) : std::string("-");
        };
        if (name == "pathtiming")
        {
            // pathtiming <actor> [expect arrival [<tick> <tolerance>] | expect none]: logs when the actor sets off,
            // reaches each point of its path and arrives, as the panel and the path bar have it
            // (Components::Actors::GetPathTiming); with expect, PASS when it arrives (at the tick, within the
            // tolerance), or doesn't
            const auto timing = Components::Actors::GetPathTiming(*actor);
            std::string reached;
            for (const auto& each : timing.reached)
                reached += ticks(each) + " ";
            Log(std::format("PATHTIMING actor {} start {} reached {}arrival {}", actor->id, ticks(timing.start),
                            reached, ticks(timing.arrival)));
            if (action == "expect")
            {
                std::string what;
                std::int64_t tick = -1, tolerance = 0;
                args >> what;
                // (expect point <i> <tick> <tolerance>: when it reaches that point)
                if (what == "point")
                {
                    std::size_t point = 0;
                    args >> point >> tick >> tolerance;
                    const auto reached = point < timing.reached.size() ? timing.reached[point] : std::nullopt;
                    const bool pass = reached && std::abs(static_cast<std::int64_t>(*reached) - tick) <= tolerance;
                    Log(std::format("{} pathtiming actor {} point {} {} (expected {} +- {})", pass ? "PASS" : "FAIL",
                                    actor->id, point, ticks(reached), tick, tolerance));
                    return true;
                }
                args >> tick >> tolerance;
                const bool pass =
                    what == "none"
                        ? !timing.arrival
                        : timing.arrival &&
                              (tick < 0 || std::abs(static_cast<std::int64_t>(*timing.arrival) - tick) <= tolerance);
                Log(std::format("{} pathtiming actor {} arrival {} (expected {} {} +- {})", pass ? "PASS" : "FAIL",
                                actor->id, ticks(timing.arrival), what, tick, tolerance));
            }
            return true;
        }
        if (name == "stretches")
        {
            // stretches <actor> [expect <gait>... | expect look <animation>]: logs each point's stretch (the last
            // point's too, which goes as the last walked), its look and the actor's looks; with expect, PASS when the
            // gaits of its stretches are those (walk, run, sprint; crouched with a c after), or its first stretch's
            // look is that
            std::string seen;
            for (const auto& stretch : actor->stretches)
            {
                auto gait = std::string(magic_enum::enum_name(stretch.gait));
                std::ranges::transform(gait, gait.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
                seen += gait + (stretch.crouched ? "c " : " ");
            }
            std::string detail;
            for (const auto& stretch : actor->stretches)
                detail += std::format("[{} wait {}{}] ", stretch.animation, stretch.wait,
                                      stretch.look.empty() ? "" : " own look");
            std::string looks;
            for (const auto& look : actor->looks)
                looks +=
                    std::format("{}{} {} ", magic_enum::enum_name(look.gait), look.crouched ? "c" : "", look.animation);
            Log(std::format("STRETCHES actor {} {}{}pace {:.3f} looks {}", actor->id, seen, detail, actor->pace,
                            looks.empty() ? "none" : looks));
            if (action == "expect")
            {
                std::string wanted, each;
                while (args >> each)
                    wanted += each + " ";
                // (expect look <animation>: the first stretch's look)
                if (wanted.starts_with("look "))
                {
                    const auto look = wanted.substr(5, wanted.size() - 6);
                    const auto shown = actor->stretches.empty()
                                           ? std::string()
                                           : std::string(ActorLogic::GetLook(*actor, actor->stretches.front()));
                    Log(std::format("{} stretches actor {} look {} (expected {})", shown == look ? "PASS" : "FAIL",
                                    actor->id, shown.empty() ? "none" : shown, look));
                    return true;
                }
                const bool pass = seen == wanted;
                Log(std::format("{} stretches actor {}: {}(expected {})", pass ? "PASS" : "FAIL", actor->id, seen,
                                wanted));
            }
            return true;
        }
        if (name == "look")
        {
            // look <actor> <animation> [<stretch>]: sets the look of the gait it is a look of, for the whole path or
            // that stretch only (Components::Actors::SetLook), as the Look row and the clip editor do
            std::string stretch;
            args >> stretch;
            std::optional<std::size_t> only;
            if (!stretch.empty())
                only = static_cast<std::size_t>(std::stoul(stretch));
            RunOnRenderThread([&] {
                auto changed = *Components::Actors::GetActor(actor->id);
                Components::Actors::SetLook(changed, only, action);
                Components::Actors::Update(changed, false, "look " + action);
            });
            Log(std::format("LOOK actor {} {} on {}", actor->id, action, only ? std::to_string(*only) : "the path"));
            return true;
        }
        if (name == "anchors")
        {
            // anchors <actor> [expect <animation|*> <end|point|time> [<point>] [<offset> <tolerance>]]: logs what
            // each one-off of the user's hangs on (ActorClip::anchor) and where it starts; with expect, PASS when the
            // last clip of that animation (* for the last one-off) hangs on that (of the point, by about the offset; on
            // a time, starting at about the tick given as the offset) and starts where it says by the path's timing
            const auto timing = Components::Actors::GetPathTiming(*actor);
            std::string seen;
            for (const auto& clip : actor->clips)
                if (clip.chain == Types::ActorChain::None && !clip.loop)
                    seen += std::format(
                        "[{} {}{} {:+} at {}] ", clip.animation, magic_enum::enum_name(clip.anchor),
                        clip.anchor == Types::ActorAnchor::Point ? std::format(" {}", clip.anchorPoint) : "",
                        clip.anchorOffset, clip.start);
            Log(std::format("ANCHORS actor {} {}", actor->id, seen.empty() ? "none" : seen));
            if (action == "expect")
            {
                std::string animation, anchor;
                args >> animation >> anchor;
                std::uint32_t point = 0;
                if (anchor == "point")
                    args >> point;
                std::int64_t offset = 0, tolerance = -1;
                args >> offset >> tolerance;
                // (the last of that animation, or of the one-offs for *)
                const auto last =
                    std::ranges::find_if(actor->clips.rbegin(), actor->clips.rend(), [&](const Types::ActorClip& each) {
                        return animation == "*" ? each.chain == Types::ActorChain::None && !each.loop
                                                : each.animation == animation;
                    });
                const auto clip = last == actor->clips.rend() ? actor->clips.end() : std::prev(last.base());
                bool pass = false;
                if (clip != actor->clips.end())
                {
                    const auto wanted = anchor == "end"     ? Types::ActorAnchor::End
                                        : anchor == "point" ? Types::ActorAnchor::Point
                                                            : Types::ActorAnchor::Time;
                    const auto at = ActorLogic::GetAnchoredStart(*clip, timing);
                    // (on a time: the tick it starts at in place of the offset)
                    const auto off = wanted == Types::ActorAnchor::Time ? static_cast<std::int64_t>(clip->start)
                                                                        : static_cast<std::int64_t>(clip->anchorOffset);
                    pass = clip->anchor == wanted &&
                           (wanted != Types::ActorAnchor::Point || clip->anchorPoint == point) &&
                           (tolerance < 0 || std::abs(off - offset) <= tolerance) && (!at || *at == clip->start);
                }
                Log(std::format("{} anchors actor {} {}: {}(expected {} {} {} +- {})", pass ? "PASS" : "FAIL",
                                actor->id, animation, seen, anchor, point, offset, tolerance));
            }
            return true;
        }
        if (name == "notes")
        {
            // notes <actor> [expect <words>... | expect none | keep | letmove]: logs the notes the re-plans left
            // (Components::Actors::GetNotes); with expect, PASS when one says those words (or there is none); keep and
            // letmove answer the latest as its Keep time and Let it move do
            const auto notes = Components::Actors::GetNotes(actor->id);
            std::string seen;
            for (const auto& note : notes)
                seen += std::format("[{}: {}{}{}] ", note.kind, note.text, note.keepArrival ? " · Keep time" : "",
                                    note.letMovePace ? " · Let it move" : "");
            Log(std::format("NOTES actor {} {}", actor->id, seen.empty() ? "none" : seen));
            if (action == "expect")
            {
                std::string words, each;
                while (args >> each)
                    words += (words.empty() ? "" : " ") + each;
                const bool pass = words == "none"
                                      ? notes.empty()
                                      : std::ranges::any_of(notes, [&](const Components::Actors::Note& note) {
                                            return note.text.find(words) != std::string::npos;
                                        });
                Log(std::format("{} notes actor {}: {}(expected {})", pass ? "PASS" : "FAIL", actor->id, seen, words));
            }
            else if (action == "keep" || action == "letmove")
            {
                const auto note = std::ranges::find_if(notes, [&](const Components::Actors::Note& each) {
                    return action == "keep" ? each.keepArrival.has_value() : each.letMovePace.has_value();
                });
                if (note == notes.end())
                {
                    Log(std::format("FAIL notes actor {}: none to {}", actor->id, action));
                    return true;
                }
                RunOnRenderThread([&] {
                    auto changed = *Components::Actors::GetActor(actor->id);
                    if (note->keepArrival)
                        Components::Actors::ArriveAt(changed, *note->keepArrival);
                    else
                    {
                        Components::Actors::SetPace(changed, *note->letMovePace);
                        Components::Actors::FitOnlyWalk(changed);
                    }
                    Components::Actors::Update(changed, false, "answer a note");
                    Components::Actors::DismissNotes(actor->id);
                });
            }
            return true;
        }
        // hitdeath <actor> [expect <index> <animation|*|none> [<tick> <tolerance> [blood]] | expect count <n>]: logs
        // each Hit's tick, the death it dies with (its clip's animation and start) and its effect; with expect, PASS
        // when the Hit of that index (among its Hits) dies with that animation (any, or none), it and its death at the
        // tick (-1 for anywhere), with a fatal wound's blood; or when it has that many
        std::vector<const Types::ActorEvent*> hits;
        std::string seen;
        for (const auto& event : actor->events)
            if (event.type == Types::ActorEventType::Hit)
            {
                hits.push_back(&event);
                const auto* death = ActorLogic::FindDeathClip(*actor, event);
                seen += std::format("[hit {} dies {}{} {}] ", event.tick, death ? death->animation : "none",
                                    death ? std::format(" at {}", death->start) : "",
                                    event.effect.empty() ? "no effect" : event.effect);
            }
        Log(std::format("HITDEATH actor {} {}", actor->id, seen.empty() ? "none" : seen));
        if (action == "expect")
        {
            std::string first;
            args >> first;
            if (first == "count")
            {
                std::size_t count = 0;
                args >> count;
                Log(std::format("{} hitdeath actor {}: {} hits (expected {})", hits.size() == count ? "PASS" : "FAIL",
                                actor->id, hits.size(), count));
                return true;
            }
            const auto index = static_cast<std::size_t>(std::stoul(first));
            std::string wanted, blood;
            std::int64_t tick = -1, tolerance = 0;
            args >> wanted >> tick >> tolerance >> blood;
            bool pass = false;
            if (index < hits.size())
            {
                const auto* death = ActorLogic::FindDeathClip(*actor, *hits[index]);
                const auto at = static_cast<std::int64_t>(hits[index]->tick);
                pass = (wanted == "none" ? !death : death && (wanted == "*" || death->animation == wanted)) &&
                       (!death || death->start == hits[index]->tick) && (tick < 0 || std::abs(at - tick) <= tolerance) &&
                       (blood != "blood" || hits[index]->effect == ActorLogic::FATAL_BLOOD);
            }
            Log(std::format("{} hitdeath actor {} hit {}: {}(expected {} at {} +- {}{})", pass ? "PASS" : "FAIL",
                            actor->id, index, seen, wanted, tick, tolerance, blood == "blood" ? " with blood" : ""));
        }
        return true;
    }

    // Bullet time spans
    // clipkeep <actor> <label>: records the actor's clips (each animation, start, length, offset, speed, blend in, loop
    // and chain), its path's speed and the movement set it goes with (what Auto stands for); clipkeep <actor> same
    // <label>: PASS when they are as recorded (a person made another keeps them)
    std::map<std::string, std::string> keptClips;
    bool RunClipKeepCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "clipkeep")
            return false;
        std::string which, label, other;
        args >> which >> label >> other;
        const auto actor = Components::Actors::GetActor(ParseActorId(which));
        if (!actor)
        {
            Log(std::format("FAIL clipkeep {}: no such actor", which));
            return true;
        }
        std::string text = std::format("speed {:.1f} set {}:", actor->pathSpeed,
                                       static_cast<int>(Components::Actors::GetMovementSet(*actor)));
        for (const auto& clip : actor->clips)
            text += std::format(" {}{}@{}+{} o{:.3f} x{:.2f} b{}{}", clip.chain != Types::ActorChain::None ? "+" : "",
                                clip.animation, clip.start, clip.length, clip.offset, clip.speed, clip.blendIn,
                                clip.loop ? " loop" : "");
        if (label != "same")
        {
            keptClips[label] = text;
            Log(std::format("CLIPKEEP actor {} {}: {}", actor->id, label, text));
            return true;
        }
        const auto kept = keptClips.find(other);
        const bool same = kept != keptClips.end() && kept->second == text;
        Log(std::format("{} clipkeep actor {} as {}: {}{}", same ? "PASS" : "FAIL", actor->id, other, text,
                        same || kept == keptClips.end() ? std::string() : " (was " + kept->second + ")"));
        return true;
    }

    // commands for the actors' path bars on the timeline and the point selected
    bool RunPathBarCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "tl" && name != "actor")
            return false;
        // (the other tl and actor commands go on as before)
        const auto start = args.tellg();
        std::string action;
        args >> action;
        if ((name == "tl" && action != "path" && action != "pathclick") || (name == "actor" && action != "waypoint"))
        {
            args.clear();
            args.seekg(start);
            return false;
        }

        if (name == "actor")
        {
            // actor waypoint <actor> [expect <index|none>]: logs the point of its path selected (none while another
            // actor is), or PASS when it is that one
            std::string which, expect, wanted;
            args >> which >> expect >> wanted;
            const auto id = ParseActorId(which);
            const auto point =
                Components::Actors::GetSelected() == id ? Components::Actors::GetSelectedWaypoint() : std::nullopt;
            const auto text = point ? std::to_string(*point) : std::string("none");
            if (expect != "expect")
                Log(std::format("ACTOR {} waypoint {}", id, text));
            else
                Log(std::format("{} actor {} waypoint {} (expected {})", text == wanted ? "PASS" : "FAIL", id, text,
                                wanted));
            return true;
        }

        // tl path <actor>: logs its path bar as drawn last (TLPATH <actor> stretch <i> <x0> <x1>, point <i> <x>, wait
        // <i> <x0> <x1>, end <x>)
        // tl pathclick <actor> <stretch|point|wait|end|at> <index|tick> [click|dclick|rclick|hover|drag <tick>]
        // [modifiers]: clicks the middle of a piece of it (a point's tick; at: a tick of the timeline on the bar), moves
        // the mouse over it, or drags from there to a tick on the actor's clips row
        auto timeline = UI::UIManager::Get().GetUIComponent<UI::Timeline>(UI::Component::Timeline);
        std::string which;
        args >> which;
        const auto id = ParseActorId(which);
        std::optional<UI::Timeline::PathBar> bar;
        std::optional<float> clipsY;
        RunOnRenderThread([&] {
            for (const auto& each : timeline->GetPathBars())
                if (each.actor == id)
                    bar = each;
            for (const auto& row : timeline->GetRows())
                if (row.name == std::format("actor {} clips", id))
                    clipsY = (row.min.y + row.max.y) / 2;
        });
        if (!bar)
        {
            Log(std::format("FAIL tl {}: actor {} has no path bar drawn", action, which));
            return true;
        }
        const auto& shape = bar->shape;
        if (action == "path")
        {
            for (const auto& stretch : shape.stretches)
                Log(std::format("TLPATH {} stretch {} {:.0f} {:.0f}", id, stretch.index, stretch.x0, stretch.x1));
            for (const auto& tick : shape.ticks)
                Log(std::format("TLPATH {} point {} {:.0f}", id, tick.point, tick.x));
            for (const auto& wait : shape.waits)
                Log(std::format("TLPATH {} wait {} {:.0f} {:.0f}", id, wait.index, wait.x0, wait.x1));
            if (shape.end)
                Log(std::format("TLPATH {} end {:.0f}", id, *shape.end));
            return true;
        }

        std::string kind, how, modifiers;
        std::size_t index = 0;
        double dragTo = 0;
        args >> kind >> index >> how;
        if (how == "drag")
            args >> dragTo;
        args >> modifiers;
        if (how != "click" && how != "dclick" && how != "rclick" && how != "hover" && how != "drag")
        {
            modifiers = how;
            how = "click";
        }
        std::optional<float> x;
        if (kind == "stretch")
            for (const auto& stretch : shape.stretches)
                if (stretch.index == index)
                    x = (stretch.x0 + stretch.x1) / 2;
        if (kind == "point")
            for (const auto& tick : shape.ticks)
                if (tick.point == index)
                    x = tick.x;
        if (kind == "wait")
            for (const auto& wait : shape.waits)
                if (wait.index == index)
                    x = (wait.x0 + wait.x1) / 2;
        if (kind == "end")
            x = shape.end;
        if (kind == "at")
            RunOnRenderThread([&] { x = timeline->GetX(static_cast<double>(index)); });
        if (!x)
        {
            Log(std::format("FAIL tl pathclick: actor {} has no {} {} on its bar", id, kind, index));
            return true;
        }
        const POINT at{static_cast<LONG>(std::lround(*x)), static_cast<LONG>(std::lround((bar->minY + bar->maxY) / 2))};
        const auto hold = [&](bool down) {
            if (modifiers.empty())
                return;
            RunOnRenderThread([&] {
                auto& io = ImGui::GetIO();
                if (modifiers.find("ctrl") != std::string::npos)
                    io.AddKeyEvent(ImGuiMod_Ctrl, down);
                if (modifiers.find("shift") != std::string::npos)
                    io.AddKeyEvent(ImGuiMod_Shift, down);
                if (modifiers.find("alt") != std::string::npos)
                    io.AddKeyEvent(ImGuiMod_Alt, down);
            });
            Sleep(60);
        };
        PostMouse(WM_MOUSEMOVE, at.x, at.y);
        Sleep(60);
        hold(true);
        if (how == "drag")
        {
            // (in steps a frame or more apart, as a hand drags)
            float toX = 0.0f;
            RunOnRenderThread([&] { toX = timeline->GetX(dragTo); });
            const POINT to{static_cast<LONG>(std::lround(toX)), static_cast<LONG>(std::lround(clipsY.value_or(at.y)))};
            PostMouse(WM_LBUTTONDOWN, at.x, at.y, MK_LBUTTON);
            Sleep(100);
            for (int i = 1; i <= 10; i++)
            {
                PostMouse(WM_MOUSEMOVE, at.x + (to.x - at.x) * i / 10, at.y + (to.y - at.y) * i / 10, MK_LBUTTON);
                Sleep(30);
            }
            Sleep(100);
            PostMouse(WM_LBUTTONUP, to.x, to.y);
        }
        else if (how != "hover")
        {
            const bool right = how == "rclick";
            for (int i = 0; i < (how == "dclick" ? 2 : 1); i++)
            {
                PostMouse(right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, at.x, at.y, right ? MK_RBUTTON : MK_LBUTTON);
                PostMouse(right ? WM_RBUTTONUP : WM_LBUTTONUP, at.x, at.y);
            }
        }
        Sleep(60);
        hold(false);
        Log(std::format("TLPATH {} {} {} {} at {} {}", id, how, kind, index, at.x, at.y));
        return true;
    }

    // pools: whether the image and model pools grew and how, their entries, free slots
    // and the pointers from outside them the streaming code was handed (POOLS lines); poolgrow grows them now, in a
    // game started without (launch.ps1 -NoPoolGrowth)
    bool RunPoolsCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "poolgrow")
        {
            const auto before = ContentPacks::CountFreeSlots(8);
            const auto outcome = PoolGrowth::GrowNow();
            Log(std::format("POOLGROW {}; free images {} -> {}", outcome, before, ContentPacks::CountFreeSlots(8)));
            return true;
        }
        if (name == "poolhold")
        {
            std::string kind, how;
            args >> kind >> how;
            for (const auto type : kind == "image" ? std::vector{8} : kind == "model" ? std::vector{5} : std::vector{8, 5})
                Log(std::format("POOLHOLD {} {}: {} entries of the game's array", type == 8 ? "image" : "model", how,
                                PoolGrowth::HoldGameEntries(type, how == "on")));
            return true;
        }
        if (name != "pools")
            return false;
        // pools expect grown|<image|model> <least in new entries>: PASS when they grew, or that many of the pool's
        // registered assets have new entries
        std::string expect, what;
        std::uint32_t least = 0;
        args >> expect >> what >> least;
        const auto state = PoolGrowth::GetState();
        Log(std::format("POOLS grown {} ({}), {} zones at most, {} of their blocks unaligned, physical memory {} MB free",
                        state.grown ? 1 : 0, state.outcome, PoolGrowth::GetMaxZones(),
                        PoolGrowth::CountUnalignedZoneBlocks(), PoolGrowth::GetPhysicalMemoryFree() >> 20));
        if (expect == "expect" && what == "aligned")
            Log(std::format("{} pools aligned: {} blocks of character zones aren't",
                            PoolGrowth::CountUnalignedZoneBlocks() == 0 ? "PASS" : "FAIL",
                            PoolGrowth::CountUnalignedZoneBlocks()));
        if (expect == "expect" && what == "grown")
            Log(std::format("{} pools grown: {}", state.grown ? "PASS" : "FAIL", state.outcome));
        for (const auto& pool : state.pools)
        {
            const auto kind = pool.type == 8 ? "image" : "model";
            const auto inNew = ContentPacks::CountRegisteredIn(pool.type, pool.extension, pool.extensionEnd);
            Log(std::format("POOLS {} entries {} free {} in new entries {} strays {}", kind, pool.count,
                            ContentPacks::CountFreeSlots(pool.type), inNew, pool.strays));
            if (expect == "expect" && what == kind)
                Log(std::format("{} pools {}: {} registered in new entries (at least {})",
                                inNew >= least ? "PASS" : "FAIL", kind, inNew, least));
            for (const auto& stray : pool.strayEntries)
                Log(std::format("POOLS stray {} {:08X} '{}' at {:08X}, {} times", pool.type == 8 ? "image" : "model",
                                stray.entry, stray.name, stray.caller, stray.times));
        }
        std::string free;
        for (const auto type : {4, 5, 6, 7, 8, 9, 28})
            free += std::format(" {}:{}", type, ContentPacks::CountFreeSlots(type));
        Log("POOLS free" + free);
        return true;
    }

    // commands for the walk drawn as its looks, the joins between them and sliding the walk
    // campaignmap [entities <file> | expect <level> [<least kept images> <least spawns>]]: logs the campaign level
    // loaded as a multiplayer map (CampaignMaps): what of each type was registered,
    // shared, left out or had no room, its entities and spawn points, the scripts and kit, the time, notes and the
    // pools' free slots; with entities, writes the entity string multiplayer got to a file; with expect, PASS when that
    // level has finished loading with its kit, at least that many images registered and spawn points
    // mapoptions lights <steady|mission> | mapoptions doors <open|all|closed> | mapoptions clear: what the next load of a
    // campaign level or Zombies map does otherwise, for a test to compare: the game has the mission's lights and every door
    // open but those that open into nothing, with no choice; all opens those too, to
    // look at them
    Components::CampaignMapLogic::RewriteOptions mapOptions;

    bool RunMapOptionsCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "mapoptions")
            return false;
        std::string what, value;
        args >> what >> value;
        if (what == "lights")
            mapOptions.steadyLights = value == "steady";
        else if (what == "doors")
            mapOptions.doors = value == "closed" ? Components::CampaignMapLogic::Doors::Closed
                               : value == "all"  ? Components::CampaignMapLogic::Doors::AllOpen
                                                 : Components::CampaignMapLogic::Doors::Open;
        else
            mapOptions = {};
        CampaignMaps::SetRewriteOptions(mapOptions);
        using enum Components::CampaignMapLogic::Doors;
        Log(std::format("MAPOPTIONS lights {} doors {}", mapOptions.steadyLights ? "steady" : "mission",
                        mapOptions.doors == Closed ? "closed" : mapOptions.doors == AllOpen ? "all" : "open"));
        return true;
    }

    bool RunCampaignMapCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "campaignmap")
            return false;
        std::string sub;
        args >> sub;
        const auto state = CampaignMaps::GetState();
        if (!state)
        {
            Log("CAMPAIGNMAP none (" + CampaignMaps::GetError() + ")");
            if (sub == "expect")
                Log("FAIL campaignmap: no campaign map");
            return true;
        }
        if (sub == "doors")
        {
            // campaignmap doors [expect <doors> <pieces left out>]: the doors it found, and how many pieces were left out
            for (const auto& door : state->doors)
                Log(std::format("CAMPAIGNMAP door {} {} {} pieces {} at {:.0f} {:.0f} {:.0f}", door.name,
                                door.zombies ? "zombies" : "named", door.open ? "open" : "closed", door.pieces, door.at.x,
                                door.at.y, door.at.z));
            std::string word;
            std::uint32_t doors = 0, pieces = 0;
            args >> word >> doors >> pieces;
            if (word == "expect")
                Log(std::format("{} campaignmap doors: {} found, {} pieces left out",
                                state->doors.size() == doors && state->doorPiecesLeftOut == pieces ? "PASS" : "FAIL",
                                state->doors.size(), state->doorPiecesLeftOut));
            return true;
        }
        if (sub == "steady")
        {
            // campaignmap steady expect <lights>: PASS when the options made that many lights steady
            std::string word;
            std::uint32_t lights = 0;
            args >> word >> lights;
            Log(std::format("{} campaignmap steady: {} lights", state->lightsMadeSteady == lights ? "PASS" : "FAIL",
                            state->lightsMadeSteady));
            return true;
        }
        if (sub == "vehicles")
        {
            // campaignmap vehicles [expect <kept> <left out>]: what the level's vehicles came to (IsParkedVehicle)
            std::string word;
            std::uint32_t kept = 0, leftOut = 0;
            args >> word >> kept >> leftOut;
            const bool checked = word == "expect";
            Log(std::format("{}CAMPAIGNMAP vehicles kept {} left out {}, lights made steady {}",
                            !checked ? "" : state->vehiclesKept == kept && state->vehiclesLeftOut == leftOut ? "PASS " : "FAIL ",
                            state->vehiclesKept, state->vehiclesLeftOut, state->lightsMadeSteady));
            return true;
        }
        if (sub == "entities")
        {
            std::string path;
            args >> path;
            std::ofstream(path, std::ios::binary) << state->entities;
            Log(std::format("CAMPAIGNMAP entities {} chars to {}", state->entities.size(), path));
            return true;
        }
        if (sub == "spawned")
        {
            // the player stands at one of the spawn points (on the ground, within 128 units across)
            const auto cg = Structures::GetClientGlobals();
            if (!cg)
            {
                Log("FAIL campaignmap spawned: no client");
                return true;
            }
            const auto& ps = cg->predictedPlayerState;
            const glm::vec3 origin(ps.origin[0], ps.origin[1], ps.origin[2]);
            float nearest = 1e9f;
            for (const auto& point : state->spawnPoints)
                nearest = std::min(nearest, glm::length(glm::vec2(point) - glm::vec2(origin)));
            const bool grounded = ps.groundEntityNum != Structures::ENTITYNUM_NONE;
            Log(std::format("{} campaignmap spawned: at {:.0f} {:.0f} {:.0f}, {} the ground, {:.0f} from the nearest of {} "
                            "spawn points",
                            grounded && nearest < 128.0f ? "PASS" : "FAIL", origin.x, origin.y, origin.z,
                            grounded ? "on" : "off", nearest, state->spawnPoints.size()));
            return true;
        }
        const auto& r = state->result;
        std::string types;
        for (std::int32_t type = 0; type < 64; ++type)
            if (r.kept[type] || r.skipped[type] || r.shared[type] || r.poolFull[type])
                types += std::format(" {}:{}/{}/{}/{}", type, r.kept[type], r.shared[type], r.skipped[type],
                                     r.poolFull[type]);
        Log(std::format("CAMPAIGNMAP {} finished {} {} ms kit '{}', type:kept/shared/skipped/poolfull{}", state->level,
                        state->finished, r.milliseconds, state->kit, types));
        Log(std::format("CAMPAIGNMAP entities kept {} dropped {} spawns {}, scripts {}, released textures {} ({} MB) "
                        "shaders {} (kept {}), late {}",
                        state->entitiesKept, state->entitiesDropped, state->spawns, state->scripts, r.releasedTextures,
                        r.releasedTextureBytes >> 20, r.releasedShaders, r.keptShaders, r.registeredLate));
        for (const auto& note : r.notes)
            Log("CAMPAIGNMAP note " + note);
        for (const auto& note : state->kitNotes)
            Log("CAMPAIGNMAP kit " + note);
        std::string free;
        for (const auto type : {1, 4, 5, 6, 7, 8, 9, 28, 36})
            free += std::format(" {}:{}", type, ContentPacks::CountFreeSlots(type));
        Log("CAMPAIGNMAP free" + free);
        if (sub == "expect")
        {
            std::string level;
            std::uint32_t images = 0, spawns = 0;
            args >> level >> images >> spawns;
            const auto poolFull = std::accumulate(r.poolFull.begin(), r.poolFull.end(), 0u);
            const bool passed = state->level == level && state->finished && !state->kit.empty() &&
                                r.kept[8] >= images && state->spawns >= spawns && poolFull == 0;
            Log(std::format("{} campaignmap {}: finished {}, kit '{}', {} images registered (at least {}), {} spawn "
                            "points (at least {}), {} without room",
                            passed ? "PASS" : "FAIL", level, state->finished, state->kit, r.kept[8], images,
                            state->spawns, spawns, poolFull));
        }
        return true;
    }

    // The pixels of a video's picture that differ between its first and last frames by more than a limit in a channel,
    // through an FFmpeg filter that gives frames of that many pixels
    std::optional<std::size_t> CountFirstFrameChanges(const std::filesystem::path& video, std::string_view filter,
                                                      std::size_t pixels, int limit)
    {
        const std::size_t FRAME = pixels * 3;
        char shortPath[MAX_PATH]{};
        GetShortPathNameA(Components::GetFFmpegPath().string().c_str(), shortPath, MAX_PATH);
        // (the short path unquoted: cmd drops the first and last quotes of a line that starts with one)
        const auto command = std::format("{} -v error -i \"{}\" -vf {},format=rgb24 -f rawvideo - 2>NUL", shortPath,
                                         video.string(), filter);
        FILE* pipe = _popen(command.c_str(), "rb");
        if (!pipe)
            return std::nullopt;
        std::vector<std::uint8_t> first(FRAME), last(FRAME), frame(FRAME);
        std::size_t frames = 0;
        while (std::fread(frame.data(), 1, FRAME, pipe) == FRAME)
        {
            (frames == 0 ? first : last) = frame;
            ++frames;
        }
        _pclose(pipe);
        if (frames < 2)
            return std::nullopt;
        std::size_t changed = 0;
        for (std::size_t i = 0; i < FRAME; i += 3)
        {
            for (std::size_t c = 0; c < 3; ++c)
            {
                if (std::abs(first[i + c] - last[i + c]) > limit)
                {
                    ++changed;
                    break;
                }
            }
        }
        return changed;
    }

    // Those of the top left corner where ReShade draws its loading ring (96 x 96), by more than 48: none when ReShade
    // drew every frame of a still scene (a frozen demo), many when it loaded at first (the ring fills as it loads, the
    // effects come at its end)
    std::optional<std::size_t> CountFirstFrameCornerChanges(const std::filesystem::path& video)
    {
        constexpr std::size_t SIDE = 96;
        return CountFirstFrameChanges(video, std::format("crop={}:{}:0:0", SIDE, SIDE), SIDE * SIDE, 48);
    }

    // A video's first frame as a picture, that many pixels wide, as a grab is saved
    bool SaveFirstFrame(const std::filesystem::path& video, const std::filesystem::path& picture, int width)
    {
        char shortPath[MAX_PATH]{};
        GetShortPathNameA(Components::GetFFmpegPath().string().c_str(), shortPath, MAX_PATH);
        const auto command = std::format("{} -v error -y -i \"{}\" -frames:v 1 -vf scale={}:-2 \"{}\" 2>NUL", shortPath,
                                         video.string(), width, picture.string());
        return std::system(command.c_str()) == 0 && std::filesystem::exists(picture);
    }

    // capturecheck <directory> [reshade] [still] [pack] [surface] [frame <label>] [delete]: PASS when a capture wrote a
    // file there that isn't empty, with its size, waiting up to 15 s for FFmpeg to finish it (its size the same for a
    // second); with reshade, also when no frame of the last capture was drawn without ReShade's effects and each file's
    // first frame has the last one's top left corner, without ReShade's loading ring (a still scene: freeze on);
    // with still, also when each file's whole first frame has its last one's picture (at most 0.5% of 640 x 360 pixels
    // more than 24 apart: a first frame without Pack's textures or Surface depth); with pack and surface, also
    // when the first frame drew with them and had nothing left to make; with delete, the directory goes after (test
    // captures are 20+ MB a second)
    bool RunCaptureCheckCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "capturecheck")
            return false;
        std::string directory, how;
        args >> directory;
        bool reshade = false, still = false, pack = false, surface = false;
        std::string frameLabel;
        for (std::string word; args >> word;)
        {
            if (word == "frame")
                args >> frameLabel;
            else if (word == "reshade")
                reshade = true;
            else if (word == "still")
                still = true;
            else if (word == "pack")
                pack = true;
            else if (word == "surface")
                surface = true;
            else
                how = word;
        }
        std::error_code error;
        std::uintmax_t bytes = 0;
        std::size_t files = 0;
        const auto measure = [&] {
            bytes = 0;
            files = 0;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error))
            {
                if (entry.is_regular_file(error) && entry.file_size(error) > 0)
                {
                    ++files;
                    bytes += entry.file_size(error);
                }
            }
        };
        measure();
        for (int waited = 0; waited < 15; ++waited)
        {
            const auto before = bytes;
            Sleep(1000);
            measure();
            if (files > 0 && bytes == before)
                break;
        }
        Log(std::format("{} capturecheck {}: {} files, {} MB", files > 0 ? "PASS" : "FAIL", directory, files,
                        bytes >> 20));
        // (frame <label>: the default pass's first frame as shots\<label>.png, 960 wide, for !imgdiff)
        if (!frameLabel.empty())
        {
            bool saved = false;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error))
                if (!saved && entry.is_regular_file(error) && entry.path().extension() == ".mov")
                    saved = SaveFirstFrame(entry.path(), Harness::directory / "shots" / (frameLabel + ".png"), 960);
            Log(std::format("{} capturecheck frame {}", saved ? "PASS" : "FAIL", frameLabel));
        }
        if (reshade)
        {
            if (ReShade::GetStatus() == ReShade::Status::NotFound)
            {
                Log("capturecheck reshade: no ReShade, left out");
            }
            else
            {
                const auto without = Components::CaptureManager::Get().GetFramesWithoutReShade();
                Log(std::format("{} capturecheck reshade: {} frames drawn without ReShade's effects", without == 0 ? "PASS" : "FAIL",
                                without));
                for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error))
                {
                    if (!entry.is_regular_file(error) || entry.path().extension() != ".mov")
                        continue;
                    const auto changed = CountFirstFrameCornerChanges(entry.path());
                    Log(std::format("{} capturecheck reshade {}: {} pixels of the first frame's corner differ from the last's",
                                    changed == 0u ? "PASS" : "FAIL", entry.path().filename().string(),
                                    changed ? std::to_string(*changed) : "unread"));
                }
            }
        }
        if (still)
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error))
            {
                if (!entry.is_regular_file(error) || entry.path().extension() != ".mov")
                    continue;
                constexpr std::size_t PIXELS = 640 * 360;
                const auto changed = CountFirstFrameChanges(entry.path(), "scale=640:360", PIXELS, 24);
                Log(std::format("{} capturecheck still {}: {} of {} pixels of the first frame differ from the last's",
                                changed && *changed <= PIXELS / 200 ? "PASS" : "FAIL", entry.path().filename().string(),
                                changed ? std::to_string(*changed) : "unread", PIXELS));
            }
        }
        const auto first = Components::CaptureManager::Get().GetFirstFrame();
        if (pack)
            Log(std::format("{} capturecheck pack: the first frame swapped {} textures, {} left to make",
                            first.packSwaps > 0 && first.packLeft == 0 ? "PASS" : "FAIL", first.packSwaps, first.packLeft));
        if (surface)
            Log(std::format("{} capturecheck surface: the first frame drew {} surfaces with Surface depth, {} left to make",
                            first.surfaceDraws > 0 && first.surfaceLeft == 0 ? "PASS" : "FAIL", first.surfaceDraws,
                            first.surfaceLeft));
        if (how == "delete")
            std::filesystem::remove_all(directory, error);
        return true;
    }

    // restart trace <ms>: samples every 200 ms what a restart waits for on a thread of its own, while the next commands
    // run; restart trace wait logs it (a change of what is left only, and one line of the rest a second)
    std::mutex restartTraceMutex;
    std::vector<std::string> restartTraceLines;
    std::atomic<bool> restartTracing = false;

    void TraceRestart(int ms)
    {
        restartTracing = true;
        std::thread([ms] {
            const auto& capture = Components::CaptureManager::Get();
            for (int waited = 0; waited < ms; waited += 200)
            {
                const auto pack = Components::TexturePack::GetStats();
                const auto surface = Components::SurfaceDepth::GetStats();
                const auto reload = ReShade::GetReload();
                const auto screen = capture.GetRestartScreen();
                std::lock_guard lock(restartTraceMutex);
                restartTraceLines.push_back(std::format(
                    "RESTART {} ms {} [{}] [{}] [{}]: ReShade loading {} frames {}, Pack {} of {} made, {} drawn left, {} "
                    "ahead left, {} swaps of {} set, {} watched, Surface depth {} of {} heights, {} of {} shaders, {} "
                    "streaming, {} map draws, tick {}",
                    waited, capture.IsRestarting() ? "restarting" : "idle", screen.line, screen.time, screen.note,
                    reload.loading, reload.framesSince, pack.made, pack.images, pack.drawnLeft, pack.aheadLeft, pack.swaps,
                    pack.calls, pack.watched, surface.heights, surface.normalMaps, surface.patched, surface.shaders,
                    surface.streaming, surface.mapDraws, Components::Playback::GetTimelineTick()));
                Sleep(200);
            }
            restartTracing = false;
        }).detach();
    }

    void LogRestartTrace()
    {
        for (int i = 0; i < 12000 && restartTracing; ++i)
            Sleep(50);
        std::lock_guard lock(restartTraceMutex);
        for (const auto& line : restartTraceLines)
            Log(line);
        restartTraceLines.clear();
    }

    // restart [expect <capture|back|shadows> [reshade] [pack] [surface] [nopack] [nosurface] [held] [free] [timedout]]:
    // logs the restart of the renderer the editor waits for now (its loading screen's
    // words and line) and those that ended, each with how long it was held back and the game, ReShade, Pack and Surface
    // depth took and the frames the loading screen was drawn in; expect waits up to 700 s for none to run (ReShade's
    // compiles at a new size), then PASS when the last of that kind ended done, its loading screen drawn, none out of time
    // (timedout: one was), and with reshade, pack, surface each waited for, nopack and nosurface not waited for, held
    // held back for ReShade and free not (without ReShade, what is ReShade's is left out).
    // restart timeouts <reshade ms> <hold ms> | restart timeouts default: how long ReShade is waited for and a restart
    // held back for it; restart trace <ms> | restart trace wait: see TraceRestart
    bool RunRestartCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "restart")
            return false;
        using Components::RestartLogic::Kind;
        using Components::RestartLogic::Step;
        auto& capture = Components::CaptureManager::Get();
        std::string what, kindName;
        args >> what >> kindName;
        if (what == "timeouts")
        {
            std::uint64_t holdMs = 0;
            args >> holdMs;
            capture.SetRestartTimeouts(kindName == "default" ? std::nullopt : std::optional(std::stoull(kindName)),
                                       kindName == "default" ? std::nullopt : std::optional(holdMs));
            Log(std::format("RESTART timeouts {} {}", kindName, holdMs));
            return true;
        }
        if (what == "trace")
        {
            if (kindName == "wait")
                LogRestartTrace();
            else
                TraceRestart(std::stoi(kindName));
            return true;
        }
        std::set<std::string> wanted;
        for (std::string word; args >> word;)
            wanted.insert(word);
        if (what == "expect")
        {
            // a restart that a click just before asked for (Shadow quality's Apply) starts in the overlay frame that
            // takes the click in: those frames run first, or with slow frames none had started to wait for ("none
            // ended" in capture-pack-depth, full suite of 2026-11-08)
            WaitForOverlayFrames();
            WaitForOverlayFrames();
            for (int i = 0; i < 14000 && capture.IsRestarting(); ++i)
                Sleep(50);
        }
        if (capture.IsRestarting())
            Log(std::format("RESTART now: {}, {}", capture.GetRestartLabel(), capture.GetRestartScreen().line));
        const auto restarts = capture.GetRestarts();
        const auto describe = [](bool waited, bool timedOut, std::uint32_t ms) {
            return !waited ? std::string("not waited for")
                           : std::format("{} ms{}", ms, timedOut ? " out of time" : "");
        };
        for (const auto& each : restarts)
            Log(std::format("RESTART {}: {} {}, held back {} ms, the game {} ms, ReShade {}, Pack {}, Surface depth {}, the "
                            "screen in {} frames",
                            magic_enum::enum_name(each.kind), each.label, magic_enum::enum_name(each.end), each.heldMs,
                            each.gameMs, describe(each.waitedForReShade, each.reshadeTimedOut, each.reshadeMs),
                            describe(each.waitedForPack, each.packTimedOut, each.packMs),
                            describe(each.waitedForSurface, each.surfaceTimedOut, each.surfaceMs), each.screenFrames));
        if (what != "expect")
            return true;

        const auto kind = magic_enum::enum_cast<Kind>(kindName, magic_enum::case_insensitive);
        const auto last = std::find_if(restarts.rbegin(), restarts.rend(),
                                       [&](const auto& each) { return kind && each.kind == *kind; });
        if (!kind || last == restarts.rend())
        {
            Log(std::format("FAIL restart expect {}: none ended", kindName));
            return true;
        }
        const bool noReShade = ReShade::GetStatus() == ReShade::Status::NotFound;
        const bool withReShade = wanted.contains("reshade") && !noReShade;
        const bool timedOut = last->reshadeTimedOut || last->packTimedOut || last->surfaceTimedOut;
        const bool passed = last->end == Step::Done && last->screenFrames > 0 &&
                            (!withReShade || last->waitedForReShade) && (!wanted.contains("pack") || last->waitedForPack) &&
                            (!wanted.contains("surface") || last->waitedForSurface) &&
                            !(wanted.contains("nopack") && last->waitedForPack) &&
                            !(wanted.contains("nosurface") && last->waitedForSurface) &&
                            (!wanted.contains("held") || noReShade || last->heldMs > 0) &&
                            (!wanted.contains("free") || last->heldMs == 0) &&
                            (noReShade || timedOut == wanted.contains("timedout"));
        Log(std::format("{} restart expect {}: {}, held back {} ms, the screen in {} frames, ReShade {}, Pack {}, Surface "
                        "depth {}, {}",
                        passed ? "PASS" : "FAIL", kindName, magic_enum::enum_name(last->end), last->heldMs,
                        last->screenFrames,
                        !wanted.contains("reshade")  ? "not asked"
                        : !withReShade               ? "not in the game, left out"
                        : last->waitedForReShade     ? "waited for"
                                                     : "not waited for",
                        last->waitedForPack ? "waited for" : "not waited for",
                        last->waitedForSurface ? "waited for" : "not waited for",
                        timedOut ? "out of time" : "in time"));
        return true;
    }

    bool RunReloadGuessCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "reloadguess")
            return false;
        // reloadguess <body> <weapon variant> [<file>]: works out the reload profile of every one-off animation the game
        // has loaded that isn't named a reload, on the body holding the gun, as every clip got one once
        // (the hand's path near the magazine read as a reload) and as now, and logs
        // how many say reload each way, deaths apart, with each one that did into a file of the harness folder
        std::string body, weapon, file;
        args >> body >> weapon >> file;
        const auto catalog = Components::Actors::GetCatalog();
        const auto rig = Mod::GetGameInterface()->GetWeaponRig(weapon);
        if (!catalog || !rig)
        {
            Log("FAIL reloadguess: no catalog, or no such gun");
            return true;
        }
        std::size_t checked = 0, deaths = 0, before = 0, beforeDeaths = 0, after = 0, afterDeaths = 0;
        auto caught = nlohmann::json::array();
        for (const auto& animation : catalog->animations)
        {
            if (animation.loop || animation.stored || ReloadLogic::IsReloadName(animation.name))
                continue;
            const bool death = animation.entry ? animation.entry->death
                                               : ActorLogic::IsDeathAnimation(animation.name, animation.loop,
                                                                              animation.notetracks);
            ReloadLogic::ProfileInput input{.animation = "reload",
                                            .duration = animation.duration,
                                            .notetracks = animation.notetracks,
                                            .rig = rig,
                                            .bones = Components::ReloadProfiles::GetBones(animation.name, body)};
            const auto old = ReloadLogic::MakeProfile(input);
            input.animation = animation.name;
            input.death = death;
            const auto now = ReloadLogic::MakeProfile(input);
            ++checked;
            deaths += death ? 1 : 0;
            const bool was = ReloadLogic::GetMoments(old).has_value(), is = ReloadLogic::GetMoments(now).has_value();
            before += was ? 1 : 0;
            beforeDeaths += was && death ? 1 : 0;
            after += is ? 1 : 0;
            afterDeaths += is && death ? 1 : 0;
            if (was || is)
                caught.push_back({{"animation", animation.name},
                                  {"death", death},
                                  {"before", DescribeProfile(old)},
                                  {"after", DescribeProfile(now)}});
        }
        if (!file.empty())
            std::ofstream(directory / file) << caught.dump(1);
        Log(std::format("RELOADGUESS {} one-offs on {} with {} ({} deaths): {} moved the magazine ({} deaths) as every "
                        "clip was guessed, {} ({} deaths) now",
                        checked, body, weapon, deaths, before, beforeDeaths, after, afterDeaths));
        return true;
    }

    // the left button held at a world point (an axis of a gizmo held still), until release lets it go
    bool RunHeldPressCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "release")
        {
            const auto at = ImGui::GetIO().MousePos;
            PostMouse(WM_LBUTTONUP, static_cast<int>(at.x), static_cast<int>(at.y));
            return true;
        }
        if (name != "pressat")
            return false;
        glm::vec3 point{};
        args >> point.x >> point.y >> point.z;
        std::optional<glm::vec2> screen;
        RunOnRenderThread([&] { screen = GFX::WorldToScreen(point); });
        const auto at = screen ? glm::ivec2(glm::round(*screen)) : glm::ivec2(-1);
        PostMouse(WM_MOUSEMOVE, at.x, at.y);
        PostMouse(WM_LBUTTONDOWN, at.x, at.y, MK_LBUTTON);
        return true;
    }

    // tl scrollunder <row>: scrolls the tracks so that a row's middle is behind the ruler's, out of sight, as the
    // wheel can leave it; PASS when it got there; tl scrollunder end: as far down as they go. tl barselected <actor> <0|1>: PASS when an effect's or a light's bar
    // is selected or not
    bool RunTimelineScrollCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "tl")
            return false;
        const auto start = args.tellg();
        std::string action;
        args >> action;
        // (the rows as they were drawn last)
        const auto getRows = [] {
            std::vector<UI::Timeline::Row> rows;
            RunOnRenderThread([&] {
                rows = UI::UIManager::Get().GetUIComponent<UI::Timeline>(UI::Component::Timeline)->GetRows();
            });
            return rows;
        };
        const auto findRow = [](const std::vector<UI::Timeline::Row>& rows, std::string row) {
            if (row.starts_with("actor."))
            {
                if (const auto dot = row.find('.', 6); dot != std::string::npos)
                    row = std::format("actor {} {}", ParseActorId(row.substr(6, dot - 6)), row.substr(dot + 1));
            }
            return std::ranges::find(rows, row, &UI::Timeline::Row::name);
        };
        if (action == "roworder")
        {
            // tl roworder <row>...: PASS when they are in that order down the timeline
            const auto rows = getRows();
            std::string row, seen;
            std::optional<float> last;
            bool inOrder = true;
            while (args >> row)
            {
                const auto found = findRow(rows, row);
                inOrder = inOrder && found != rows.end() && (!last || found->min.y >= *last);
                last = found != rows.end() ? found->min.y : -1.0f;
                seen += std::format("{} {:.0f}, ", row, *last);
            }
            Log(std::format("{} tl roworder {}", inOrder ? "PASS" : "FAIL", seen));
            return true;
        }
        if (action == "rowshown")
        {
            // tl rowshown <row> <0|1>: PASS when it was drawn or not
            std::string row;
            bool wanted = false;
            args >> row >> wanted;
            const auto rows = getRows();
            const bool shown = findRow(rows, row) != rows.end();
            Log(std::format("{} tl row {} {}", shown == wanted ? "PASS" : "FAIL", row, shown ? "drawn" : "not drawn"));
            return true;
        }
        if (action == "rowhidden")
        {
            // tl rowhidden <actor> <0|1>: PASS when the actor is hidden or not
            std::string which;
            bool wanted = false;
            args >> which >> wanted;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            Log(std::format("{} tl actor {} hidden {}", actor && actor->hidden == wanted ? "PASS" : "FAIL", which,
                            actor && actor->hidden ? 1 : 0));
            return true;
        }
        if (action == "rowpinned")
        {
            // tl rowpinned <group>: PASS when its header is at the top of the tracks, what takes the mouse under it
            std::string group;
            args >> group;
            const auto rows = getRows();
            const auto header = findRow(rows, "group." + group + ".name");
            const auto lanes = findRow(rows, "lanes");
            const bool pinned = header != rows.end() && lanes != rows.end() && std::abs(header->max.y - lanes->min.y) <= 2.0f;
            Log(std::format("{} tl group {} {}", pinned ? "PASS" : "FAIL", group, pinned ? "at the top" : "not at the top"));
            return true;
        }
        if (action == "rowdrag")
        {
            // tl rowdrag <actor> <actor|end>: drags a row by its name to before another's, or below its group's last
            std::string which, target;
            args >> which >> target;
            const auto rows = getRows();
            const auto from = findRow(rows, "actor." + which + ".name");
            if (from == rows.end())
            {
                Log(std::format("FAIL tl rowdrag: no row {}", which));
                return true;
            }
            const auto to = target == "end" ? rows.end() : findRow(rows, "actor." + target + ".name");
            const auto x = static_cast<int>(from->min.x + 30.0f), y = static_cast<int>((from->min.y + from->max.y) / 2);
            const auto toY = static_cast<int>(to != rows.end() ? to->min.y + 2.0f : from->max.y + 200.0f);
            PostMouse(WM_MOUSEMOVE, x, y);
            Sleep(150);
            PostMouse(WM_LBUTTONDOWN, x, y, MK_LBUTTON);
            Sleep(100);
            for (int i = 1; i <= 6; i++)
            {
                PostMouse(WM_MOUSEMOVE, x, y + (toY - y) * i / 6, MK_LBUTTON);
                Sleep(40);
            }
            Sleep(100);
            PostMouse(WM_LBUTTONUP, x, toY);
            Sleep(200);
            return true;
        }
        if (action == "barselected")
        {
            std::string which;
            bool wanted = false;
            args >> which >> wanted;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            bool selected = false;
            RunOnRenderThread([&] { selected = actor && UI::TimelineSelection::IsSelected(UI::TimelineSelection::BarItem(*actor)); });
            Log(std::format("{} tl bar of {} {}", actor && selected == wanted ? "PASS" : "FAIL", which,
                            selected ? "selected" : "not selected"));
            return true;
        }
        if (action != "scrollunder")
        {
            args.clear();
            args.seekg(start);
            return false;
        }
        std::string row;
        args >> row;
        if (row.starts_with("actor."))
        {
            if (const auto dot = row.find('.', 6); dot != std::string::npos)
                row = std::format("actor {} {}", ParseActorId(row.substr(6, dot - 6)), row.substr(dot + 1));
        }
        if (row == "end")
        {
            RunOnRenderThread([&] {
                for (auto* window : ImGui::GetCurrentContext()->Windows)
                    if (std::string_view(window->Name).find("##tracks") != std::string_view::npos)
                        ImGui::SetScrollY(window, window->ScrollMax.y);
            });
            Sleep(200);
            return true;
        }
        const auto timeline = UI::UIManager::Get().GetUIComponent<UI::Timeline>(UI::Component::Timeline);
        // how far the row's middle is under the ruler's (actor.<id>.path: its path bar)
        const auto getOffset = [&]() -> std::optional<float> {
            std::optional<float> ruler, wanted;
            for (const auto& each : timeline->GetRows())
            {
                if (each.name == "ruler")
                    ruler = (each.min.y + each.max.y) / 2;
                if (each.name == row)
                    wanted = (each.min.y + each.max.y) / 2;
            }
            for (const auto& bar : timeline->GetPathBars())
            {
                if (row == std::format("actor {} path", bar.actor))
                    wanted = (bar.minY + bar.maxY) / 2;
            }
            return ruler && wanted ? std::optional(*wanted - *ruler) : std::nullopt;
        };
        RunOnRenderThread([&] {
            const auto offset = getOffset();
            if (!offset)
                return;
            for (auto* window : ImGui::GetCurrentContext()->Windows)
                if (std::string_view(window->Name).find("##tracks") != std::string_view::npos)
                    ImGui::SetScrollY(window, window->Scroll.y + *offset);
        });
        Sleep(200);
        std::optional<float> offset;
        RunOnRenderThread([&] { offset = getOffset(); });
        Log(std::format("{} tl scrollunder {}: {:.0f} px under the ruler",
                        offset && std::abs(*offset) <= 2.0f ? "PASS" : "FAIL", row, offset.value_or(-1.0f)));
        return true;
    }

    // How far the mod's lines are from the world they are drawn on: at each frame, a point straight ahead of the game's
    // view as drawn goes through the view the lines are drawn from and through the game's own matrix, and the two
    // places on the screen are compared. The free camera is turned by so many degrees a
    // second meanwhile, once a frame as the mouse turns it.
    struct ViewLag
    {
        float rate = 0.0f;
        // (shaken: this way and back every 100 ms, so what is looked at stays in the view)
        bool shake = false;
        double elapsed = 0.0;
        std::uint32_t frames = 0;
        std::uint32_t mostViews = 0;
        double sumPixels = 0.0, mostPixels = 0.0, sumDegrees = 0.0, mostDegrees = 0.0;
    };
    std::optional<ViewLag> viewLag;

    void RecordViewLag()
    {
        if (!viewLag)
            return;
        auto& lag = *viewLag;
        if (const auto drawn = Hooks::Camera::GetDrawnMatrices())
        {
            // (the third column of the game's view is where it looks)
            const glm::vec3 forward(drawn->view[0][2], drawn->view[1][2], drawn->view[2][2]);
            const auto point = drawn->origin + forward * 1000.0f;
            const auto gameView = UI::UIManager::Get().GetUIComponent<UI::GameView>(UI::Component::Component::GameView);
            const auto position = gameView->GetViewportPosition(), size = gameView->GetViewportSize();
            const auto clip = drawn->viewProjection * glm::vec4(point, 1.0f);
            const glm::vec2 game(position.x + (clip.x / clip.w + 1.0f) / 2.0f * size.x,
                                 position.y + (1.0f - clip.y / clip.w) / 2.0f * size.y);
            const auto screen = GFX::WorldToScreen(point);
            const auto& camera = Components::CameraManager::Get().GetActiveCamera();
            const auto pixels = screen ? static_cast<double>(glm::distance(*screen, game)) : 0.0;
            const auto degrees = glm::degrees(std::acos(static_cast<double>(
                std::clamp(glm::dot(glm::normalize(forward), camera->GetForwardVector()), -1.0f, 1.0f))));
            ++lag.frames;
            lag.mostViews = std::max(lag.mostViews, drawn->views);
            lag.sumPixels += pixels;
            lag.mostPixels = std::max(lag.mostPixels, pixels);
            lag.sumDegrees += degrees;
            lag.mostDegrees = std::max(lag.mostDegrees, degrees);
        }
        const auto deltaTime = ImGui::GetIO().DeltaTime;
        lag.elapsed += deltaTime;
        const auto direction = lag.shake && static_cast<int>(lag.elapsed / 0.1) % 2 ? -1.0f : 1.0f;
        Components::CameraManager::Get().GetActiveCamera()->GetRotation().y += direction * lag.rate * deltaTime;
    }

    // logs how far the lines were from the world since viewlag spin or shake began, and stops the view
    void ReportViewLag(std::istringstream& args)
    {
        ViewLag lag;
        RunOnRenderThread([&] {
            lag = *viewLag;
            viewLag.reset();
        });
        const auto frames = std::max<std::uint32_t>(lag.frames, 1);
        const auto line = std::format("viewlag {} deg/s: {} frames, {} views a frame at most, off by {:.1f} px on "
                                      "average and {:.1f} at most, the camera {:.2f} and {:.2f} degrees ahead",
                                      lag.rate, lag.frames, lag.mostViews, lag.sumPixels / frames, lag.mostPixels,
                                      lag.sumDegrees / frames, lag.mostDegrees);
        std::string expect;
        double most = 0.0;
        args >> expect >> most;
        if (expect == "expect")
            Log(std::format("{} {}", lag.frames > 0 && lag.mostPixels <= most ? "PASS" : "FAIL", line));
        else
            Log("VIEWLAG " + line);
    }

    // viewlag dump: logs the game's view as it was drawn; viewlag spin <degrees a second> <ms> [expect <pixels>]: turns
    // the free camera and logs how far the lines were from the world on the screen; with expect, PASS when never further;
    // viewlag shake <degrees a second>, then viewlag report [expect <pixels>]: the same, shaking the view until then, for
    // shots in the middle of it
    bool RunViewLagCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "viewlag")
            return false;
        std::string what;
        args >> what;
        if (what == "dump")
        {
            std::optional<Hooks::Camera::DrawnView> drawn;
            glm::vec3 camera{}, rotation{};
            float fov = 0.0f;
            RunOnRenderThread([&] {
                drawn = Hooks::Camera::GetDrawnMatrices();
                const auto& active = Components::CameraManager::Get().GetActiveCamera();
                camera = active->GetPosition();
                rotation = active->GetRotation();
                fov = active->GetFov();
            });
            if (!drawn)
            {
                Log("FAIL viewlag dump: no view drawn yet");
                return true;
            }
            const auto row = [](const glm::mat4& m, int i) {
                return std::format("{:.4f} {:.4f} {:.4f} {:.4f}", m[i][0], m[i][1], m[i][2], m[i][3]);
            };
            Log(std::format("VIEWLAG camera {} {} {} rotation {} {} {} fov {}", camera.x, camera.y, camera.z, rotation.x,
                            rotation.y, rotation.z, fov));
            Log(std::format("VIEWLAG origin {} {} {} views {}", drawn->origin.x, drawn->origin.y, drawn->origin.z,
                            drawn->views));
            for (int i = 0; i < 4; ++i)
                Log(std::format("VIEWLAG view {} | projection {}", row(drawn->view, i), row(drawn->projection, i)));
            return true;
        }
        if (what == "spin")
        {
            float rate = 0.0f;
            std::uint32_t ms = 0;
            args >> rate >> ms;
            RunOnRenderThread([&] { viewLag = ViewLag{rate}; });
            Sleep(ms);
            ReportViewLag(args);
            return true;
        }
        if (what == "shake")
        {
            float rate = 0.0f;
            args >> rate;
            RunOnRenderThread([&] { viewLag = ViewLag{rate, true}; });
            return true;
        }
        if (what == "report")
        {
            ReportViewLag(args);
            return true;
        }
        Log("FAIL viewlag: dump, spin, shake or report");
        return true;
    }

    // viewlines width <pixels> | strength <0 to 1> | dim: sets the view's lines, or logs how
    // strong they are now; viewlines expect width|strength|dim <x> <tolerance>: PASS when it is that
    bool RunViewLinesCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "viewlines")
            return false;
        std::string what, which;
        args >> what;
        auto& preferences = PreferencesConfiguration::Get();
        if (what == "expect")
        {
            float wanted = 0.0f, tolerance = 0.0f;
            args >> which >> wanted >> tolerance;
            float value = 0.0f;
            RunOnRenderThread([&] {
                value = which == "width"      ? preferences.viewLineWidth
                        : which == "strength" ? preferences.viewLineStrength
                                              : GFX::GetViewLineDim();
            });
            Log(std::format("{} viewlines {} {:.2f} (expected {:.2f} +- {:.2f})",
                            std::abs(value - wanted) <= tolerance ? "PASS" : "FAIL", which, value, wanted, tolerance));
            return true;
        }
        float value = 0.0f;
        if (args >> value)
            RunOnRenderThread([&] { (what == "width" ? preferences.viewLineWidth : preferences.viewLineStrength) = value; });
        Log(std::format("VIEWLINES width {:.2f} strength {:.2f} dim {:.2f}", preferences.viewLineWidth,
                        preferences.viewLineStrength, GFX::GetViewLineDim()));
        return true;
    }

    bool RunWalkPieceCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "tl" && name != "actor")
            return false;
        // (the other tl and actor commands go on as before)
        const auto start = args.tellg();
        std::string action;
        args >> action;
        if ((name == "tl" && action != "pieces" && action != "piececlick" && action != "joindrag" &&
             action != "pathslide" && action != "piecescope" && action != "walkend") ||
            (name == "actor" && action != "join" && action != "speedat"))
        {
            args.clear();
            args.seekg(start);
            return false;
        }
        std::string which;
        args >> which;
        const auto id = ParseActorId(which);
        const auto actor = Components::Actors::GetActor(id);
        if (!actor)
        {
            Log(std::format("FAIL {} {}: no such actor {}", name, action, which));
            return true;
        }
        auto timeline = UI::UIManager::Get().GetUIComponent<UI::Timeline>(UI::Component::Timeline);
        const auto hold = [&](bool alt, bool down) {
            if (!alt)
                return;
            RunOnRenderThread([&] { ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, down); });
            Sleep(60);
        };
        // a drag from one point to another, in steps a frame or more apart as a hand drags
        const auto dragMouse = [&](POINT from, POINT to, bool alt) {
            PostMouse(WM_MOUSEMOVE, from.x, from.y);
            Sleep(60);
            hold(alt, true);
            PostMouse(WM_LBUTTONDOWN, from.x, from.y, MK_LBUTTON);
            Sleep(100);
            for (int i = 1; i <= 10; i++)
            {
                PostMouse(WM_MOUSEMOVE, from.x + (to.x - from.x) * i / 10, from.y + (to.y - from.y) * i / 10,
                          MK_LBUTTON);
                Sleep(30);
            }
            Sleep(100);
            PostMouse(WM_LBUTTONUP, to.x, to.y);
            Sleep(60);
            hold(alt, false);
        };
        float pixelsPerTick = 0.0f;
        std::vector<UI::Timeline::WalkPiece> pieces;
        std::optional<UI::Timeline::PathBar> bar;
        RunOnRenderThread([&] {
            pixelsPerTick = (timeline->GetX(1000.0) - timeline->GetX(0.0)) / 1000.0f;
            for (const auto& each : timeline->GetWalkPieces())
                if (each.actor == id)
                    pieces.push_back(each);
            for (const auto& each : timeline->GetPathBars())
                if (each.actor == id)
                    bar = each;
        });

        if (name == "tl" && action == "pieces")
        {
            // tl pieces <actor> [expect <look>...]: logs its walk's pieces as drawn last (TLPIECE <actor> <i> <look>
            // <x0> <x1> ramp <x0> <x1>); with expect, PASS when their looks are those, in order
            std::string seen;
            for (const auto& piece : pieces)
            {
                Log(std::format("TLPIECE {} {} {} {:.0f} {:.0f} ramp {:.0f} {:.0f}", id, piece.index, piece.look,
                                piece.x0, piece.x1, piece.rampX0, piece.rampX1));
                seen += piece.look + " ";
            }
            std::string expect, wanted, each;
            args >> expect;
            while (args >> each)
                wanted += each + " ";
            if (expect == "expect")
                Log(std::format("{} tl pieces actor {}: {}(expected {})", seen == wanted ? "PASS" : "FAIL", id, seen,
                                wanted));
            return true;
        }
        if (name == "tl" && action == "piecescope")
        {
            // tl piecescope <actor> [expect stretch|path|none]: logs what a change on the piece the Actors tab shows
            // reaches (Apply to): its stretches, or the whole path; PASS when so
            std::optional<UI::ActorsMenu::PieceScope> scope;
            RunOnRenderThread([&] {
                scope = UI::UIManager::Get().GetUIComponent<UI::ActorsMenu>(UI::Component::ActorsMenu)->GetPieceScope();
            });
            const auto seen = !scope || scope->actor != id ? std::string("none")
                              : scope->wholePath           ? std::string("path")
                                                           : std::string("stretch");
            Log(std::format("TLPIECE {} scope {}{}", id, seen,
                            scope && scope->actor == id ? std::format(" {} {}", scope->first, scope->last) : ""));
            std::string expect, wanted;
            args >> expect >> wanted;
            if (expect == "expect")
                Log(std::format("{} tl piecescope actor {} {} (expected {})", seen == wanted ? "PASS" : "FAIL", id,
                                seen, wanted));
            return true;
        }
        if (name == "tl" && action == "walkend")
        {
            // tl walkend <actor> <ticks>: drags the right edge of its walk's last piece (just inside it) that many
            // ticks along, which changes when it arrives as the path bar's end does
            double ticks = 0;
            args >> ticks;
            if (pieces.empty())
            {
                Log(std::format("FAIL tl walkend: actor {} has no piece drawn", id));
                return true;
            }
            const auto& last = pieces.back();
            const POINT from{static_cast<LONG>(std::lround(last.x1 - 2)),
                             static_cast<LONG>(std::lround((last.minY + last.maxY) / 2))};
            const POINT to{static_cast<LONG>(std::lround(last.x1 - 2 + ticks * pixelsPerTick)), from.y};
            dragMouse(from, to, false);
            Log(std::format("TLPIECE {} walkend {} by {} at {} {}", id, last.index, ticks, from.x, from.y));
            return true;
        }
        if (name == "tl" && (action == "piececlick" || action == "joindrag"))
        {
            // tl piececlick <actor> <i> [click|dclick|rclick|hover]: clicks a piece of its walk (in its upper half,
            // away from the ramp), or moves the mouse over it; tl joindrag <actor> <i> <0|1> <ticks> [alt]: drags an
            // edge of the piece's ramp (0 its start) that many ticks along (with Alt held: Auto)
            std::size_t index = 0;
            args >> index;
            const auto piece = std::ranges::find(pieces, index, &UI::Timeline::WalkPiece::index);
            if (piece == pieces.end())
            {
                Log(std::format("FAIL tl {}: actor {} has no piece {} drawn", action, id, index));
                return true;
            }
            if (action == "joindrag")
            {
                int edge = 0;
                double ticks = 0;
                std::string alt;
                args >> edge >> ticks >> alt;
                const auto x = edge == 0 ? piece->rampX0 : piece->rampX1;
                const POINT from{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(piece->maxY - 3))};
                const POINT to{static_cast<LONG>(std::lround(x + ticks * pixelsPerTick)), from.y};
                dragMouse(from, to, alt == "alt");
                Log(std::format("TLPIECE {} joindrag {} edge {} by {} at {} {}", id, index, edge, ticks, from.x,
                                from.y));
                return true;
            }
            std::string how;
            args >> how;
            if (how.empty())
                how = "click";
            const POINT at{static_cast<LONG>(std::lround((piece->x0 + piece->x1) / 2)),
                           static_cast<LONG>(std::lround(piece->minY + (piece->maxY - piece->minY) * 0.3f))};
            PostMouse(WM_MOUSEMOVE, at.x, at.y);
            Sleep(60);
            if (how != "hover")
            {
                const bool right = how == "rclick";
                for (int i = 0; i < (how == "dclick" ? 2 : 1); i++)
                {
                    PostMouse(right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, at.x, at.y, right ? MK_RBUTTON : MK_LBUTTON);
                    PostMouse(right ? WM_RBUTTONUP : WM_LBUTTONUP, at.x, at.y);
                }
            }
            Sleep(60);
            Log(std::format("TLPIECE {} {} {} at {} {}", id, how, index, at.x, at.y));
            return true;
        }
        if (name == "tl" && action == "pathslide")
        {
            // tl pathslide <actor> <ticks>|to <tick> [grab start|point <i>|end] [alt]: drags its path bar that many
            // ticks along (or the tick grabbed to a tick of the timeline), pressed on the tick grabbed (where it sets
            // off by default; the end a little before its handle)
            double ticks = 0;
            std::optional<double> toTick;
            std::string grab, alt, first;
            args >> first;
            if (first == "to")
            {
                double tick = 0;
                args >> tick;
                toTick = tick;
            }
            else
                ticks = std::stod(first);
            args >> grab;
            if (grab == "grab")
                args >> grab;
            std::size_t point = 0;
            if (grab == "point")
                args >> point;
            args >> alt;
            if (grab == "alt")
                alt = grab;
            if (!bar || bar->shape.stretches.empty())
            {
                Log(std::format("FAIL tl pathslide: actor {} has no path bar drawn", id));
                return true;
            }
            const auto& shape = bar->shape;
            std::optional<float> x = shape.stretches.front().x0 + 3.0f;
            if (grab == "end")
                x = shape.end ? std::optional(*shape.end - 8.0f) : std::nullopt;
            if (grab == "point")
            {
                x.reset();
                for (const auto& tick : shape.ticks)
                    if (tick.point == point)
                        x = tick.x;
            }
            if (!x)
            {
                Log(std::format("FAIL tl pathslide: actor {} has no {} {} on its bar", id, grab, point));
                return true;
            }
            const auto y = static_cast<LONG>(std::lround((bar->minY + bar->maxY) / 2));
            const POINT from{static_cast<LONG>(std::lround(*x)), y};
            float toX = *x + static_cast<float>(ticks) * pixelsPerTick;
            if (toTick)
                RunOnRenderThread([&] { toX = timeline->GetX(*toTick); });
            const POINT to{static_cast<LONG>(std::lround(toX)), y};
            dragMouse(from, to, alt == "alt");
            Log(std::format("TLPATH {} slide {} grab {} {} at {} {}", id, ticks, grab.empty() ? "start" : grab, point,
                            from.x, from.y));
            return true;
        }
        if (action == "speedat")
        {
            // actor speedat <actor> <tick> [expect <least> <most>]: logs how fast it goes there (units a second);
            // PASS when within
            std::uint32_t tick = 0;
            std::string expect;
            float least = 0, most = 0;
            args >> tick >> expect >> least >> most;
            const auto speed = Components::Actors::GetSpeedAt(*actor, tick);
            Log(std::format("ACTOR {} speed {:.1f} at {}", id, speed, tick));
            if (expect == "expect")
                Log(std::format("{} actor {} speed {:.1f} at {} (expected {} to {})",
                                speed >= least && speed <= most ? "PASS" : "FAIL", id, speed, tick, least, most));
            return true;
        }

        // actor join <actor> <stretch> [blend <ms>|auto] [at before|centred|after] [feet on|off]: sets its join with
        // the stretch before (as the Animation section's piece does), else logs it (JOIN ...); actor join <actor>
        // <stretch> expect [auto <ms>] [blend <ms>] [near <ms> <tolerance>] [at ...] [feet ...] [speed <least>
        // <most>]: PASS when it is so (speed: how fast it goes at the point); actor join <actor> <stretch> seek
        // point|start|end: the playhead to its point, or its blend's start or end
        std::size_t index = 0;
        args >> index;
        if (index >= actor->stretches.size())
        {
            Log(std::format("FAIL actor join: actor {} has no stretch {}", id, index));
            return true;
        }
        std::vector<std::string> words;
        for (std::string word; args >> word;)
            words.push_back(word);
        const auto describe = [&](const Types::Actor& each) {
            const auto& stretch = each.stretches[index];
            const auto timing = Components::Actors::GetPathTiming(each);
            const auto at = stretch.joinAsBefore ? Types::ActorJoinAt::After : stretch.blendAt;
            auto place = std::string(magic_enum::enum_name(at));
            std::ranges::transform(place, place.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
            const auto& window = index < timing.joins.size() ? timing.joins[index] : std::nullopt;
            return std::format("join {} blend {} {} auto {} at {} feet {}{} window {}",
                               ActorLogic::HasJoin(each, index),
                               ActorLogic::GetJoinBlend(each, index, Components::Actors::GetAnimationLookup()),
                               stretch.joinAsBefore ? "before-joins"
                               : stretch.blend      ? "set"
                                                    : "auto",
                               ActorLogic::GetAutoJoinBlend(each, index, Components::Actors::GetAnimationLookup()),
                               place, stretch.matchFeet && !stretch.joinAsBefore ? "on" : "off",
                               index < timing.reached.size() && timing.reached[index]
                                   ? std::format(" point {}", *timing.reached[index])
                                   : "",
                               window ? std::format("{} {}", window->first, window->second) : std::string("none"));
        };
        if (!words.empty() && words.front() == "seek")
        {
            const auto timing = Components::Actors::GetPathTiming(*actor);
            const auto& window = index < timing.joins.size() ? timing.joins[index] : std::nullopt;
            const auto where = words.size() > 1 ? words[1] : std::string("point");
            const auto tick = where == "start" && window      ? std::optional(window->first)
                              : where == "end" && window      ? std::optional(window->second)
                              : index < timing.reached.size() ? timing.reached[index]
                                                              : std::nullopt;
            if (!tick)
            {
                Log(std::format("FAIL actor join: actor {} has no {} of stretch {}'s join", id, where, index));
                return true;
            }
            Components::Playback::MoveFrozenTick(Components::Playback::ActorToTimelineTick(*tick));
            Sleep(300);
            Log(std::format("JOIN actor {} stretch {} seek {} {}", id, index, where, *tick));
            return true;
        }
        if (!words.empty() && words.front() == "expect")
        {
            const auto seen = describe(*actor);
            const auto& stretch = actor->stretches[index];
            const auto blend = ActorLogic::GetJoinBlend(*actor, index, Components::Actors::GetAnimationLookup());
            const auto number = [&](std::size_t i) { return i < words.size() ? std::stod(words[i]) : 0.0; };
            bool pass = true;
            std::string more;
            for (std::size_t i = 1; i < words.size(); ++i)
            {
                const auto& key = words[i];
                if (key == "auto")
                    pass = pass && !stretch.blend && !stretch.joinAsBefore && blend == number(++i);
                else if (key == "blend")
                    pass = pass && blend == number(++i);
                else if (key == "near")
                {
                    const auto wanted = number(++i), tolerance = number(++i);
                    pass = pass && std::abs(static_cast<double>(blend) - wanted) <= tolerance;
                }
                else if (key == "speed")
                {
                    const auto least = number(++i), most = number(++i);
                    const auto timing = Components::Actors::GetPathTiming(*actor);
                    const auto speed = index < timing.reached.size() && timing.reached[index]
                                           ? Components::Actors::GetSpeedAt(*actor, *timing.reached[index])
                                           : -1.0f;
                    more += std::format(" speed {:.1f}", speed);
                    pass = pass && speed >= least && speed <= most;
                }
                else if (i + 1 < words.size())
                    pass = pass && seen.find(key + " " + words[++i]) != std::string::npos;
            }
            Log(std::format("{} actor {} stretch {} {}{}", pass ? "PASS" : "FAIL", id, index, seen, more));
            return true;
        }
        if (!words.empty())
        {
            RunOnRenderThread([&] {
                auto changed = *Components::Actors::GetActor(id);
                auto& stretch = changed.stretches[index];
                ActorLogic::UpdateJoin(stretch);
                for (std::size_t i = 0; i < words.size(); ++i)
                {
                    if (words[i] == "auto")
                        stretch.blend.reset();
                    else if (words[i] == "blend" && i + 1 < words.size())
                        stretch.blend = static_cast<std::uint32_t>(std::stoul(words[++i]));
                    else if (words[i] == "at" && i + 1 < words.size())
                    {
                        const auto& place = words[++i];
                        stretch.blendAt = place == "before"  ? Types::ActorJoinAt::Before
                                          : place == "after" ? Types::ActorJoinAt::After
                                                             : Types::ActorJoinAt::Centred;
                    }
                    else if (words[i] == "feet" && i + 1 < words.size())
                        stretch.matchFeet = words[++i] == "on";
                }
                Components::Actors::Update(changed, false, "change the join");
            });
        }
        Log(std::format("JOIN actor {} stretch {} {}", id, index, describe(*Components::Actors::GetActor(id))));
        return true;
    }

    // commands for which way actors' bodies face across their clips' hand-overs
    bool RunFacingCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "facecheck")
            return false;

        // facecheck <actor> <from> <to> <step> [<most> [<label>]]: seeks from one tick to another in steps (freeze and
        // pause first) and logs which way the visible body faces at each (the hips' and the shoulders': square to the
        // line from the right one to the left, flat) and the way the actor is drawn (its tag_origin); PASS when none of
        // the three turned more than that many degrees from one step to the next. With a label, remembers the hips' and
        // the drawn facing at the last step (see facecheck same)
        // facecheck same <label> <label> <degrees> [drawn]: PASS when the body (the drawn actor) faced the same way at
        // the two within the degrees
        // facecheck gun <actor> <yaw> <degrees>: PASS when the actor's gun points that way (tag_flash's forward, flat)
        // within the degrees now
        std::string which;
        args >> which;
        if (which == "gun")
        {
            std::string actor;
            float yaw = 0.0f, most = 5.0f;
            args >> actor >> yaw >> most;
            std::vector<std::optional<glm::mat4>> flash;
            RunOnMainThread([&] { flash = Actors::GetBoneMatrices(ParseActorId(actor), {"tag_flash"}); });
            if (flash.size() != 1 || !flash[0])
            {
                Log(std::format("FAIL facecheck gun {}: no gun drawn", actor));
                return true;
            }
            const auto points = glm::degrees(std::atan2((*flash[0])[0].y, (*flash[0])[0].x));
            const auto off = std::abs(std::remainder(points - yaw, 360.0f));
            Log(std::format("{} facecheck gun actor {}: points {:.1f}, {:.1f} off {} (at most {})",
                            off <= most ? "PASS" : "FAIL", actor, points, off, yaw, most));
            return true;
        }
        if (which == "same")
        {
            std::string a, b, what;
            float most = 2.0f;
            args >> a >> b >> most >> what;
            const auto first = facings.find(a), second = facings.find(b);
            if (first == facings.end() || second == facings.end())
            {
                Log(std::format("FAIL facecheck same {} {}: missing", a, b));
                return true;
            }
            const bool drawn = what == "drawn";
            const auto one = drawn ? first->second.second : first->second.first;
            const auto other = drawn ? second->second.second : second->second.first;
            const auto apart = std::abs(std::remainder(other - one, 360.0f));
            Log(std::format("{} facecheck same {} {}: {} {:.1f} and {:.1f}, {:.2f} apart (at most {})",
                            apart <= most ? "PASS" : "FAIL", a, b, drawn ? "drawn" : "body", one, other, apart, most));
            return true;
        }
        std::uint32_t from = 0, to = 0, step = 50;
        std::string mostText, label;
        args >> from >> to >> step >> mostText >> label;
        const auto most = mostText.empty() ? -1.0f : std::stof(mostText);
        const auto id = ParseActorId(which);
        if (!Components::Playback::IsGameFrozen() || step == 0 || to < from)
        {
            Log("FAIL facecheck: freeze first, and give a step and a stretch");
            return true;
        }
        std::string text;
        // the hips', the shoulders' and the drawn facing: the most each turned in a step, and where
        std::array<float, 3> largest{}, previous{};
        std::array<std::uint32_t, 3> where{};
        for (auto tick = from; tick <= to; tick += step)
        {
            Components::Playback::MoveFrozenTick(tick);
            // (120 ms drew a step late now and then)
            Sleep(200);
            std::vector<std::optional<glm::vec3>> bones;
            std::optional<glm::mat4> placement;
            RunOnMainThread([&] {
                bones = Actors::GetBonePositions(id, {"j_hip_le", "j_hip_ri", "j_shoulder_le", "j_shoulder_ri"});
                placement = Actors::GetDrawnPlacement(id);
            });
            if (!placement || bones.size() != 4 || !bones[0] || !bones[1])
            {
                Log("FAIL facecheck: the actor isn't drawn, or has no hips");
                return true;
            }
            // (square to the line from the right bone to the left)
            const auto facing = [](glm::vec3 left, glm::vec3 right) {
                const auto across = glm::vec2(left - right);
                return glm::degrees(std::atan2(-across.x, across.y));
            };
            const auto body = facing(*bones[0], *bones[1]);
            const std::array<float, 3> now = {body, bones[2] && bones[3] ? facing(*bones[2], *bones[3]) : body,
                                              glm::degrees(std::atan2((*placement)[0].y, (*placement)[0].x))};
            text += std::format(" {}:{:.1f}/{:.1f}/{:.1f}", tick, now[0], now[1], now[2]);
            for (std::size_t i = 0; i < now.size() && tick > from; ++i)
                if (const auto turned = std::abs(std::remainder(now[i] - previous[i], 360.0f)); turned > largest[i])
                {
                    largest[i] = turned;
                    where[i] = tick;
                }
            previous = now;
        }
        if (!label.empty())
            facings[label] = {previous[0], previous[2]};
        Log(std::format("FACING actor {} (tick:hips/chest/drawn){}", id, text));
        const auto verdict = most < 0.0f ? "INFO" : std::ranges::max(largest) <= most ? "PASS" : "FAIL";
        const auto turned = std::format("{:.2f} (hips, at {}), {:.2f} (chest, at {}), {:.2f} (drawn, at {})",
                                        largest[0], where[0], largest[1], where[1], largest[2], where[2]);
        Log(std::format("{} facecheck actor {} {}-{} every {}: turned at most {} in a step{}", verdict, id, from, to,
                        step, turned, most < 0.0f ? std::string() : std::format(" (at most {})", most)));
        return true;
    }

    bool RunBulletTimeCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "span")
            return false;

        // span list: logs the spans (where each starts on the timeline, the demo's tick it holds at, how long, whether
        // actors move) and the timeline's, the demo's and the actors' ticks now
        // span add <timeline tick> [ms], span length <index> <ms>, span remove <index>, span actors <index> <0|1>:
        // edits them as the timeline's menus do (one step each, the editor's ticks after a span moving along)
        // span expect count <n>, span expect <index> <demo tick> <ms> [actors 0|1]: PASS when so
        // span expect world <tick> | actor <tick> | timeline <tick> [tolerance]: PASS when the demo's (actors',
        // timeline's) tick is that
        // span check <ms>: plays that long (from the playhead, not paused) and PASS when in each frame the demo was at
        // the tick the spans put it at for the timeline's (held at a span's tick through it), going on exactly as
        // given, never back
        std::string action;
        args >> action;
        const auto describe = [] {
            const auto spans = Components::Playback::GetFrozenSpans();
            std::string text;
            for (std::size_t i = 0; i < spans->size(); ++i)
                text += std::format(" {}: from {} holding at {} for {}{}", i,
                                    Components::FrozenSpans::GetStart(*spans, i), (*spans)[i].worldTick,
                                    (*spans)[i].length, (*spans)[i].actorsMove ? " (actors move)" : "");
            return std::format("SPANS{}; timeline {} demo {} actors {} end {}", text.empty() ? " none" : text,
                               Components::Playback::GetTimelineTick(), Mod::GetGameInterface()->GetDemoInfo().gameTick,
                               Components::Playback::GetActorTick(), Components::Playback::GetTimelineEnd());
        };

        if (action == "list" || action.empty())
        {
            std::string text;
            RunOnMainThread([&] { text = describe(); });
            Log(text);
        }
        else if (action == "add" || action == "length" || action == "remove" || action == "actors")
        {
            std::uint32_t first = 0, second = Components::BulletTime::DEFAULT_LENGTH;
            args >> first >> second;
            RunOnRenderThread([&] {
                if (action == "add")
                    Components::BulletTime::Add(first, second);
                else if (action == "length")
                    Components::BulletTime::SetLength(first, second);
                else if (action == "remove")
                    Components::BulletTime::Remove(first);
                else
                    Components::BulletTime::SetActorsMove(first, second != 0);
            });
            std::string text;
            RunOnMainThread([&] { text = describe(); });
            Log(text);
        }
        else if (action == "expect")
        {
            std::string what;
            args >> what;
            const auto spans = Components::Playback::GetFrozenSpans();
            if (what == "count")
            {
                std::size_t count = 0;
                args >> count;
                Log(std::format("{} span expect count {} (there are {})", spans->size() == count ? "PASS" : "FAIL",
                                count, spans->size()));
            }
            else if (what == "world" || what == "actor" || what == "timeline")
            {
                std::int64_t tick = 0, tolerance = 0;
                args >> tick >> tolerance;
                std::int64_t now = 0;
                RunOnMainThread([&] {
                    now = what == "world"   ? Mod::GetGameInterface()->GetDemoInfo().gameTick
                          : what == "actor" ? Components::Playback::GetActorTick()
                                            : Components::Playback::GetTimelineTick();
                });
                Log(std::format("{} span expect {} {} (it is {})", std::abs(now - tick) <= tolerance ? "PASS" : "FAIL",
                                what, tick, now));
            }
            else
            {
                const auto index = static_cast<std::size_t>(std::atoi(what.c_str()));
                std::uint32_t world = 0, length = 0;
                std::string actorsWord;
                int actors = -1;
                args >> world >> length >> actorsWord >> actors;
                const bool pass = index < spans->size() && (*spans)[index].worldTick == world &&
                                  (*spans)[index].length == length &&
                                  (actors < 0 || (*spans)[index].actorsMove == (actors != 0));
                Log(std::format("{} span expect {} at {} for {}", pass ? "PASS" : "FAIL", index, world, length));
            }
        }
        else if (action == "check")
        {
            std::uint32_t length = 0;
            args >> length;
            struct Sample
            {
                std::uint32_t timeline = 0;
                std::uint32_t world = 0;
                bool rewinding = false;
            };
            std::vector<Sample> samples;
            std::uint32_t start = 0;
            RunOnMainThread([&] { start = Components::Playback::GetTimelineTick(); });
            for (int frame = 0; frame < 100000; ++frame)
            {
                Sample sample;
                RunOnMainThread([&] {
                    sample = {Components::Playback::GetTimelineTick(), Mod::GetGameInterface()->GetDemoInfo().gameTick,
                              Components::Rewinding::IsRewinding()};
                });
                samples.push_back(sample);
                if (sample.timeline >= start + length || sample.timeline < start)
                    break;
            }
            const auto spans = Components::Playback::GetFrozenSpans();
            std::size_t off = 0, held = 0, back = 0, inSpans = 0;
            std::string first;
            for (std::size_t i = 0; i < samples.size(); ++i)
            {
                const auto& sample = samples[i];
                if (sample.rewinding)
                    continue;
                const auto expected = Components::FrozenSpans::ToWorld(*spans, sample.timeline);
                if (expected != sample.world)
                {
                    ++off;
                    if (first.empty())
                        first = std::format(" (first at timeline {}: the demo at {}, not {})", sample.timeline,
                                            sample.world, expected);
                }
                if (const auto span = Components::FrozenSpans::FindAt(*spans, sample.timeline))
                {
                    ++inSpans;
                    if (sample.world != (*spans)[*span].worldTick)
                        ++held;
                }
                if (i > 0 && (sample.timeline < samples[i - 1].timeline || sample.world < samples[i - 1].world))
                {
                    if (back++ == 0)
                        first += std::format(" (back first from timeline {} demo {} to {} {})", samples[i - 1].timeline,
                                             samples[i - 1].world, sample.timeline, sample.world);
                }
            }
            const bool pass = samples.size() > 2 && off == 0 && held == 0 && back == 0;
            Log(
                std::format("{} span check: {} frames from {} to {}, {} in spans; the demo off its tick in {}{}, not "
                            "held in a span in {}, going back in {}",
                            pass ? "PASS" : "FAIL", samples.size(), start, samples.back().timeline, inSpans, off, first,
                            held, back));
        }
        else
        {
            Log("FAIL span: no such action " + action);
        }
        return true;
    }

    // the angle of the turn from one rotation to another, in degrees
    float TurnBetween(const glm::quat& a, const glm::quat& b)
    {
        const auto dot = std::abs(glm::dot(glm::normalize(a), glm::normalize(b)));
        return glm::degrees(2.0f * std::acos(std::min(dot, 1.0f)));
    }

    bool RunHeadTurnCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "headcheck")
        {
            // headcheck <actor> <from> <to> <step> <most> [bones...]: seeks from one tick to another in steps (freeze
            // and pause first) and logs how far the head (j_head and tag_eye unless named) turned from one step to
            // the next in the actor's own space, in degrees: the first bone's each step and each bone's most, and
            // where; PASS when none turned more than most (a look starts from where the
            // clip has the head, with no snap)
            std::string which;
            std::uint32_t from = 0, to = 0, step = 16;
            float most = 10.0f;
            args >> which >> from >> to >> step >> most;
            std::vector<std::string> bones;
            for (std::string bone; args >> bone;)
                bones.push_back(bone);
            if (bones.empty())
                bones = {"j_head", "tag_eye"};
            const auto id = ParseActorId(which);
            if (!Components::Playback::IsGameFrozen() || step == 0 || to <= from)
            {
                Log("FAIL headcheck: freeze first, and give a step and a stretch");
                return true;
            }
            std::vector<float> largest(bones.size(), 0.0f);
            std::vector<std::uint32_t> where(bones.size(), 0);
            std::vector<std::optional<glm::quat>> previous;
            std::string steps;
            for (auto tick = from; tick <= to; tick += step)
            {
                Components::Playback::MoveFrozenTick(tick);
                Sleep(120);
                std::vector<std::optional<glm::mat4>> matrices;
                std::optional<glm::mat4> placement;
                RunOnMainThread([&] {
                    matrices = Actors::GetBoneMatrices(id, bones);
                    placement = Actors::GetDrawnPlacement(id);
                });
                if (!placement || matrices.size() != bones.size())
                {
                    Log("FAIL headcheck: the actor isn't drawn");
                    return true;
                }
                const auto toActor = glm::transpose(glm::mat3(*placement));
                std::vector<std::optional<glm::quat>> rotations;
                for (const auto& matrix : matrices)
                    rotations.push_back(matrix ? std::optional(glm::quat_cast(toActor * glm::mat3(*matrix)))
                                               : std::nullopt);
                for (std::size_t i = 0; i < bones.size() && !previous.empty(); ++i)
                {
                    if (!rotations[i] || !previous[i])
                        continue;
                    const auto turned = TurnBetween(*rotations[i], *previous[i]);
                    if (i == 0)
                        steps += std::format("{}{} {:.1f}", steps.empty() ? "" : ", ", tick, turned);
                    if (turned > largest[i])
                    {
                        largest[i] = turned;
                        where[i] = tick;
                    }
                }
                previous = std::move(rotations);
            }
            Log(std::format("HEADCHECK {} by step: {}", bones[0], steps));
            std::string text;
            bool pass = true;
            for (std::size_t i = 0; i < bones.size(); ++i)
            {
                text += std::format("{}{} {:.1f} at {}", text.empty() ? "" : ", ", bones[i], largest[i], where[i]);
                pass = pass && largest[i] <= most;
            }
            Log(std::format("{} headcheck actor {} {}-{} every {}: {} (at most {})", pass ? "PASS" : "FAIL", id, from,
                            to, step, text, most));
            return true;
        }
        if (name == "headrest")
        {
            // headrest <model> <animation>... | headrest <model> all [degrees]: how far animations hold j_head from
            // its rest on the model, relative to the bone it hangs from (the most of 5 points of each), which is how
            // far a look snapped the head as it started once; all: the game's loaded
            // animations of the list, how many are over each of 10, 20, 40 and 60 degrees, and those over degrees
            // (20) by most
            std::string model, first;
            args >> model >> first;
            std::vector<std::string> animations;
            float over = 20.0f;
            if (first == "all")
            {
                args >> over;
                if (const auto list = Components::Actors::GetAnimationList())
                    for (const auto& animation : list->animations)
                        if (animation.loaded && !animation.loaded->stored)
                            animations.emplace_back(animation.name);
            }
            else
            {
                animations.push_back(first);
                for (std::string animation; args >> animation;)
                    animations.push_back(animation);
            }

            // the head's rotation relative to its parent in a pose
            auto local = [](const Types::AnimationPose& pose) -> std::optional<glm::quat> {
                const auto head = pose.FindBone("j_head");
                if (!head || pose.parents[*head] < 0)
                    return std::nullopt;
                return glm::inverse(pose.rotations[pose.parents[*head]]) * pose.rotations[*head];
            };
            std::optional<glm::quat> rest;
            RunOnMainThread([&] {
                if (const auto xmodel = static_cast<const Structures::XModel*>(
                        ContentPacks::FindAsset(Structures::ASSET_TYPE_XMODEL, model.c_str())))
                    rest = local(AnimationPose::GetRestPose(*xmodel));
            });
            if (!rest)
            {
                Log("FAIL headrest: the game hasn't loaded " + model + " or it has no j_head under another bone");
                return true;
            }

            std::vector<std::pair<float, std::string>> off;
            std::array<std::size_t, 4> counts{};
            std::size_t decoded = 0;
            for (const auto& animation : animations)
            {
                std::optional<float> most;
                for (const auto time : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
                    if (const auto pose = AnimationPose::GetPose(animation, model, time))
                        if (const auto head = local(*pose))
                            most = std::max(most.value_or(0.0f), TurnBetween(*head, *rest));
                if (!most)
                {
                    if (first != "all")
                        Log(std::format("HEADREST {}: not decoded", animation));
                    continue;
                }
                ++decoded;
                for (std::size_t i = 0; i < counts.size(); ++i)
                    counts[i] += *most > std::array{10.0f, 20.0f, 40.0f, 60.0f}[i] ? 1 : 0;
                if (first != "all" || *most > over)
                    off.emplace_back(*most, animation);
            }
            std::ranges::sort(off, std::greater{});
            for (const auto& [degrees, animation] : off)
                Log(std::format("HEADREST {} {:.1f}", animation, degrees));
            Log(std::format("HEADREST {}: {} animations decoded, over 10/20/40/60 degrees: {}/{}/{}/{}", model,
                            decoded, counts[0], counts[1], counts[2], counts[3]));
            return true;
        }
        return false;
    }

    bool RunSceneEffectCommand(const std::string& name, std::istringstream& args)
    {
        // Effects in the scene on their own:
        // effect add <effect> crosshair: as Add > Effect does (ActorActions::AddEffect), on the surface where the
        // camera
        //   looks, from the playhead; effect add <effect> <x> <y> <z> [<nx> <ny> <nz>]: at the point, facing out of a
        //   surface of that normal (up unless given); logs its id (last)
        // effect play <actor> <start> <end> [auto|once|loop] [fade|cut] [every ms]: its bar and how it plays
        // effect face <actor> <pitch> <yaw> <roll>: which way it fires
        // effect state <actor> [expect none | expect playing <n> | expect bar <start> <end> <ticks> | expect at <x> <y>
        // <z>
        //   <distance> [facing <pitch> <yaw> <degrees>]]: logs its bar, place and facing and what the game plays of it
        //   (its looping plays kept, alive, the plays spawned since the last jump); PASS when it plays nothing, n
        //   looping plays are alive, its bar is that within the ticks, or it is that close to the point (facing that
        //   way)
        // effect spawns [reset | expect <name part> <n>]: how many plays of each effect the scene's effects spawned
        //   since the last reset; PASS when those with the part in their name spawned at least n
        if (name != "effect")
            return false;
        std::string what;
        args >> what;
        if (what == "add")
        {
            std::string effect, where;
            args >> effect >> where;
            if (where == "crosshair")
            {
                RunOnRenderThread([&] {
                    const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                    UI::ActorActions::AddEffect(effect, camera->GetPosition(), camera->GetForwardVector());
                });
                const auto actors = Components::Actors::GetActors();
                const auto id = actors.empty() || !actors.back().effect ? 0 : actors.back().id;
                if (id)
                    previousActorId = std::exchange(lastActorId, id);
                Log(id ? std::format("EFFECT added {} as actor {} at the crosshair", effect, id)
                       : std::format("FAIL effect add: {} wasn't added", effect));
                return true;
            }
            glm::vec3 point{}, normal{0.0f, 0.0f, 1.0f};
            std::istringstream x(where);
            if (!(x >> point.x) || !(args >> point.y >> point.z))
            {
                Log("FAIL effect add: give an effect and crosshair, or an effect and a point");
                return true;
            }
            if (glm::vec3 given; args >> given.x >> given.y >> given.z)
                normal = given;
            const auto info = Components::Actors::GetEffectInfo(effect);
            if (!info)
            {
                Log(std::format("FAIL effect add: the map hasn't {}", effect));
                return true;
            }
            auto actor = ActorLogic::MakeEffect(effect, *info, point, normal, Components::Playback::GetActorTick());
            actor.name = Components::Actors::MakeName(ActorLogic::GetEffectBaseName(effect));
            const auto id = Components::Actors::Add(actor);
            previousActorId = std::exchange(lastActorId, id);
            Log(std::format("EFFECT added {} as actor {} ({}): bar {}-{}, loops {} for {} ms", effect, id, actor.name,
                            actor.effect->start, actor.effect->end, ActorLogic::EffectLoops(*info),
                            info->loopingLife));
            return true;
        }

        std::string which;
        args >> which;
        if (what == "spawns")
        {
            const bool reset = which == "reset";
            std::map<std::string, std::size_t> spawns;
            RunOnMainThread([&] { spawns = Actors::GetSceneEffectSpawns(reset); });
            std::string text;
            for (const auto& [effect, count] : spawns)
                text += std::format(" {} x{}", effect, count);
            if (which == "expect")
            {
                std::string part;
                std::size_t most = 0;
                args >> part >> most;
                std::size_t count = 0;
                for (const auto& [effect, n] : spawns)
                    count += effect.find(part) != std::string::npos ? n : 0;
                Log(std::format("{} effect spawns {}: {} (at least {}){}", count >= most ? "PASS" : "FAIL", part,
                                count, most, text));
                return true;
            }
            Log("EFFECT spawns" + text);
            return true;
        }
        auto actor = Components::Actors::GetActor(ParseActorId(which));
        if (!actor || !actor->effect)
        {
            Log(std::format("FAIL effect {}: {} isn't an effect in the scene", what, which));
            return true;
        }
        if (what == "play")
        {
            std::uint32_t start = 0, end = 0, every = 0;
            std::string plays, ends;
            args >> start >> end >> plays >> ends >> every;
            actor->effect->start = start;
            actor->effect->end = end;
            if (plays == "once")
                actor->effect->plays = Types::EffectPlays::Once;
            else if (plays == "loop")
                actor->effect->plays = Types::EffectPlays::Loop;
            else if (plays == "auto")
                actor->effect->plays = Types::EffectPlays::Auto;
            if (ends == "cut")
                actor->effect->ends = Types::EffectEnds::Cut;
            else if (ends == "fade")
                actor->effect->ends = Types::EffectEnds::Fade;
            if (every > 0)
                actor->effect->every = every;
            Components::Actors::Update(*actor);
            Log(std::format("EFFECT {} plays {}-{}", actor->id, start, end));
            return true;
        }
        if (what == "face")
        {
            float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
            args >> pitch >> yaw >> roll;
            actor->effect->pitch = pitch;
            actor->yaw = yaw;
            actor->effect->roll = roll;
            Components::Actors::Update(*actor);
            Log(std::format("EFFECT {} faces {} {} {}", actor->id, pitch, yaw, roll));
            return true;
        }
        if (what == "state")
        {
            std::optional<Actors::SceneEffectState> state;
            RunOnMainThread([&] { state = Actors::GetSceneEffectState(actor->id); });
            const auto& effect = *actor->effect;
            const auto text = std::format(
                "effect {} {} bar {}-{} at ({:.1f} {:.1f} {:.1f}) facing {:.1f} {:.1f} {:.1f}: kept {} alive {} begun "
                "{}",
                actor->id, effect.name, effect.start, effect.end, actor->position.x, actor->position.y,
                actor->position.z, effect.pitch, actor->yaw, effect.roll, state ? state->kept : 0,
                state ? state->alive : 0, state ? state->begun : 0);
            std::string expect, kind;
            args >> expect >> kind;
            if (expect != "expect")
            {
                Log("EFFECT " + text);
                return true;
            }
            bool pass = false;
            if (kind == "none")
                pass = !state || state->alive == 0;
            else if (kind == "playing")
            {
                std::size_t n = 0;
                args >> n;
                pass = state && state->alive == n;
            }
            else if (kind == "bar")
            {
                std::int64_t start = 0, end = 0, tolerance = 0;
                args >> start >> end >> tolerance;
                pass = std::abs(static_cast<std::int64_t>(effect.start) - start) <= tolerance &&
                       std::abs(static_cast<std::int64_t>(effect.end) - end) <= tolerance;
            }
            else if (kind == "at")
            {
                glm::vec3 point{};
                float distance = 0.0f;
                std::string facing;
                float pitch = 0.0f, yaw = 0.0f, degrees = 0.0f;
                args >> point.x >> point.y >> point.z >> distance >> facing >> pitch >> yaw >> degrees;
                pass = glm::distance(actor->position, point) <= distance;
                if (facing == "facing")
                    pass = pass && std::abs(effect.pitch - pitch) <= degrees &&
                           std::abs(std::remainder(actor->yaw - yaw, 360.0f)) <= degrees;
            }
            Log(std::format("{} {}", pass ? "PASS" : "FAIL", text));
            return true;
        }
        Log("FAIL effect: add, play, face, state or spawns");
        return true;
    }

    bool RunFxListCommand(const std::string& name, std::istringstream& args)
    {
        // fxlist [part]: logs the effects the game has loaded (the list the Effect event picks from), how many by
        // their first folder, and those with the part in their name, each with its looping life and how many elements
        // loop and play once (FxEffectDef +0xC, +0x10 and +0x14, as IW3 lays it out)
        if (name != "fxlist")
            return false;
        std::string part;
        args >> part;
        std::vector<std::string> lines;
        std::map<std::string, std::size_t> folders;
        std::size_t count = 0;
        RunOnMainThread([&] {
            const auto catalog = Components::Actors::GetCatalog();
            if (!catalog)
                return;
            count = catalog->effects.size();
            for (const auto& effect : catalog->effects)
            {
                ++folders[effect.substr(0, effect.find('/'))];
                if (!part.empty() && effect.find(part) == std::string::npos)
                    continue;
                const auto def = static_cast<const std::uint8_t*>(
                    ContentPacks::FindAsset(Structures::ASSET_TYPE_FX, effect.c_str()));
                if (!def)
                    continue;
                const auto field = [&](std::size_t offset) {
                    return *reinterpret_cast<const std::int32_t*>(def + offset);
                };
                lines.push_back(std::format("FXLIST {} life {} looping {} once {}", effect, field(0xC), field(0x10),
                                            field(0x14)));
            }
        });
        for (const auto& line : lines)
            Log(line);
        std::string text;
        for (const auto& [folder, n] : folders)
            text += std::format(" {} {}", folder, n);
        Log(std::format("FXLIST {} effects:{}", count, text));
        return true;
    }

    bool RunVisionCommand(const std::string& name, std::istringstream& args)
    {
        // vision [expect current | expect <vision>]: logs the naked and night visions the configstrings say and the
        // client applied last; PASS when the client's naked vision is what its
        // configstring says, or is that vision
        if (name != "vision")
            return false;
        std::string sub, expected;
        args >> sub >> expected;
        std::string nakedString, nightString, naked, night;
        bool client = false;
        RunOnMainThread([&] {
            const auto cg = reinterpret_cast<const char*>(Structures::GetClientGlobals());
            if (!cg)
                return;
            client = true;
            nakedString = Structures::GetConfigString(Structures::CS_VISION_NAKED);
            nightString = Structures::GetConfigString(Structures::CS_VISION_NIGHT);
            naked.assign(cg + Structures::CG_VISION_NAKED_NAME, strnlen(cg + Structures::CG_VISION_NAKED_NAME, 64));
            night.assign(cg + Structures::CG_VISION_NIGHT_NAME, strnlen(cg + Structures::CG_VISION_NIGHT_NAME, 64));
        });
        if (!client)
        {
            Log(std::format("{}VISION no client", sub == "expect" ? "FAIL " : ""));
            return true;
        }
        Log(std::format("VISION naked '{}' (configstring {}), night '{}' (configstring {})", naked, nakedString, night,
                        nightString));
        if (sub != "expect")
            return true;
        // the configstring's vision: its first quoted word
        const auto first = nakedString.find('"');
        const auto said = first == std::string::npos
                              ? std::string()
                              : nakedString.substr(first + 1, nakedString.find('"', first + 1) - first - 1);
        const auto want = expected == "current" ? said : expected;
        Log(std::format("{} vision {}: the client's naked vision is '{}'", _stricmp(naked.c_str(), want.c_str()) == 0
                                                                               ? "PASS"
                                                                               : "FAIL",
                        want, naked));
        return true;
    }

    bool RunMapMenuCommand(const std::string& name, std::istringstream& args)
    {
        // The Change Map menu's campaign and Zombies tabs (MapMenu):
        //   mapmenu: logs whether the table and map list have the levels' rows and select_map the tabs, the tab on, the
        //     maps the list shows (as its feeder gives them) and the pictures made
        //   mapmenu tab <game|campaign|zombies>: sets the tab as its button does (ui_showDLCMaps 0 and ui_irisMapTab)
        //   mapmenu expect <count> [<first map>]: PASS when the list shows that many maps, the first that one
        //   mapmenu shown <campaign|zombies> <yes|no>: PASS when the tab's label shows as selected, or not
        //   mapmenu text <key> <text...>: PASS when the language lookup gives that text for the key
        //   mapmenu picture <map> <yes|no>: PASS when the menu's material of its picture is the mod's, or not
        //   mapmenu pick <map>: picks it as the list's double-click does (ui_preview, ui_mapname, ui_currentNetMap;
        //     console commands, so a start right after it wants a moment's wait)
        //   mapmenu start: the LAN setup's START MATCH (UI_StartServer), which runs map <the map picked>
        if (name != "mapmenu")
            return false;
        namespace Logic = Components::MapMenuLogic;
        std::string sub;
        args >> sub;
        const auto tabOf = [](const std::string& word) {
            return word == "campaign"  ? Logic::Tab::Campaign
                   : word == "zombies" ? Logic::Tab::Zombies
                                       : Logic::Tab::Game;
        };
        if (sub.empty() || sub == "expect")
        {
            MapMenu::State state;
            RunOnMainThread([&] { state = MapMenu::GetState(); });
            std::string listed;
            for (const auto& map : state.listed)
                listed += (listed.empty() ? "" : ", ") + map;
            Log(std::format("MAPMENU rows {} table {} maplist {} tabs {} tab {} pictures {} missing {}{}: {}",
                            state.rows, state.tableRows, state.mapListCount, state.tabs,
                            static_cast<std::int32_t>(state.tab), state.pictures, state.picturesMissing,
                            state.error.empty() ? "" : " (" + state.error + ")", listed));
            if (sub != "expect")
                return true;
            std::size_t count = 0;
            std::string first;
            args >> count >> first;
            const bool pass = state.listed.size() == count && (first.empty() || (!state.listed.empty() &&
                                                                                 state.listed.front() == first));
            Log(std::format("{} mapmenu expect {} {}: {} listed", pass ? "PASS" : "FAIL", count, first,
                            state.listed.size()));
        }
        else if (sub == "tab")
        {
            std::string which;
            args >> which;
            Functions::Cbuf_AddText(std::format("set ui_showDLCMaps 0; set {} {}", Logic::TAB_DVAR,
                                                static_cast<std::int32_t>(tabOf(which))));
            Log(std::format("MAPMENU tab {}", which));
        }
        else if (sub == "shown")
        {
            std::string which, expected;
            args >> which >> expected;
            bool shown = false;
            RunOnMainThread([&] { shown = MapMenu::IsTabSelectedShown(tabOf(which)); });
            Log(std::format("{} mapmenu shown {}: {}", shown == (expected == "yes") ? "PASS" : "FAIL", which,
                            shown ? "selected" : "not selected"));
        }
        else if (sub == "text")
        {
            std::string key, expected;
            args >> key;
            std::getline(args >> std::ws, expected);
            std::string text;
            RunOnMainThread([&] { text = MapMenu::Localize(key); });
            Log(std::format("{} mapmenu text {}: '{}'", text == expected ? "PASS" : "FAIL", key, text));
        }
        else if (sub == "picture")
        {
            std::string map, expected;
            args >> map >> expected;
            bool has = false;
            RunOnMainThread([&] { has = MapMenu::HasPicture(map); });
            Log(std::format("{} mapmenu picture {}: {}", has == (expected == "yes") ? "PASS" : "FAIL", map,
                            has ? "the mod's" : "none"));
        }
        else if (sub == "pick")
        {
            std::string map;
            args >> map;
            std::int32_t index = -1;
            RunOnMainThread([&] { index = MapMenu::FindMapListIndex(map); });
            if (index < 0)
            {
                Log(std::format("FAIL mapmenu pick {}: not in the map list", map));
                return true;
            }
            Functions::Cbuf_AddText(
                std::format("set ui_preview {0}; set ui_mapname {0}; set ui_currentNetMap {1}", map, index));
            Log(std::format("MAPMENU pick {} (map list {})", map, index));
        }
        else if (sub == "start")
        {
            RunOnMainThread(
                [] { reinterpret_cast<void(__cdecl*)(std::int32_t)>(GetGameAddresses().UI_StartServer())(0); });
            Log("MAPMENU start");
        }
        else
            Log(std::format("FAIL mapmenu: no such action {}", sub));
        return true;
    }

    // the vision mode the frame's film settings came from (see screen)
    std::string GetFrameVision()
    {
        constexpr std::array<const char*, 6> MODES = {"naked", "night", "flare", "infrared", "tvguided", "spike"};
        std::string vision = "none";
        RunOnMainThread([&] {
            const auto cg = reinterpret_cast<const std::uint8_t*>(Structures::GetClientGlobals());
            if (!cg)
                return;
            // the refdef's film fields, and where in a vision each run of them is copied from
            struct Run
            {
                std::size_t refdef, vision, floats;
            };
            constexpr std::array<Run, 3> RUNS = {{{0x4318C, 0x14, 6}, {0x43180, 0x2C, 3}, {0x431A4, 0x50, 1}}};
            const auto isFilm = [&](const std::uint8_t* settings) {
                return std::ranges::all_of(RUNS, [&](const Run& run) {
                    for (std::size_t i = 0; i < run.floats; ++i)
                        if (std::abs(reinterpret_cast<const float*>(cg + run.refdef)[i] -
                                     reinterpret_cast<const float*>(settings + run.vision)[i]) > 1e-5f)
                            return false;
                    return true;
                });
            };
            for (std::size_t mode = 0; mode < MODES.size() && vision == "none"; ++mode)
                if (isFilm(cg + Structures::CG_VISION_CURRENT + mode * Structures::CG_VISION_SIZE))
                    vision = MODES[mode];
            // (the map's own kept from before a shellshock's vision took the naked one over)
            if (const auto map = Hooks::Camera::GetMapVision(); vision == "none" && !map.empty() && isFilm(map.data()))
                vision = "map";
        });
        return vision;
    }

    // screen [expect vision <mode> | expect shellshock <on|off> | wait <mode> [ms] | keep on|off|vision|filters]: the
    // vision mode the frame's film settings came from (CG_VisionSetFrame copies the mode's current settings into the
    // refdef: naked, night, flare, infrared, tvguided, spike; map, the map's kept while a shellshock's has the naked
    // mode; none), whether shellshocks draw their flash and blur (cg_drawShellshock) and whether the frame shows the
    // demo player's own screen (first person); PASS when it is that mode's, or shellshocks draw or not; wait plays until
    // the frame's film is that mode's (up to ms, 30000) and pauses, PASS when it got there; keep: the player's vision
    // and filters (or one of them) in every view, the game's own as before, for comparing
    bool RunScreenCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "screen")
            return false;
        std::string sub, what;
        args >> sub >> what;
        if (sub == "keep")
        {
            Hooks::Camera::KeepPlayerScreen(what == "on" || what == "vision", what == "on" || what == "filters");
            Log(std::format("SCREEN kept {}", what));
            return true;
        }

        if (sub == "wait")
        {
            // plays until the frame's film is that mode's, then pauses
            int ms = 30000;
            args >> ms;
            const auto until = GetTickCount64() + ms;
            if (Components::Playback::IsPaused())
                Components::Playback::TogglePaused();
            std::string vision;
            while ((vision = GetFrameVision()) != what && GetTickCount64() < until)
                Sleep(20);
            if (!Components::Playback::IsPaused())
                Components::Playback::TogglePaused();
            Log(std::format("{} screen wait {}: the frame's film is {}'s at {}", vision == what ? "PASS" : "FAIL", what,
                            vision, Components::Playback::GetTimelineTick()));
            return true;
        }
        const auto vision = GetFrameVision();

        const auto cg_drawShellshock = Functions::FindDvar("cg_drawShellshock");
        const bool shellshock = cg_drawShellshock && cg_drawShellshock->current.enabled;
        Log(std::format("SCREEN vision {}, shellshock {}, the player's screen {}", vision, shellshock ? "on" : "off",
                        Hooks::Camera::ShowsPlayerScreen() ? "shown" : "left out"));
        if (sub != "expect")
            return true;
        std::string want;
        args >> want;
        if (what == "vision")
            Log(std::format("{} screen vision {}: the frame's film is {}'s", vision == want ? "PASS" : "FAIL", want,
                            vision));
        else if (what == "shellshock")
            Log(std::format("{} screen shellshock {}", shellshock == (want == "on") ? "PASS" : "FAIL", want));
        return true;
    }

    // playerbodies: PASS when each player has a body model drawn (its DObj's first model), as a demo or
    // game shows them; on a campaign or Zombies map they had none while multiplayer precached another teamset's models
    bool RunPlayerBodiesCommand(const std::string& name, std::istringstream&)
    {
        if (name != "playerbodies")
            return false;
        RunOnMainThread([&] {
            const auto entities = Structures::GetEntities();
            std::uint32_t players = 0, bodies = 0;
            std::string seen;
            for (std::size_t i = 0; entities && i < Structures::MAX_GENTITIES; ++i)
            {
                if (entities[i].nextState.eType != Structures::ET_PLAYER)
                    continue;
                ++players;
                const auto obj = Functions::Com_GetClientDObj(static_cast<std::int32_t>(i), 0);
                const bool body = obj && obj->numModels > 0;
                bodies += body ? 1 : 0;
                seen += std::format(" {}:{}", i, body ? obj->models[0]->name : "none");
            }
            // (none: the player is still at the team or class menu, as while a level's shaders are first compiled)
            if (players == 0)
                Log("PLAYERBODIES none: no player has spawned");
            else
                Log(std::format("{} playerbodies: {} of {} players have a body{}", bodies == players ? "PASS" : "FAIL",
                                bodies, players, seen));
        });
        return true;
    }

    // zonehide <zone>... | none: the mod's lookup of the game's zone files says the game folder hasn't those zones (as
    // one without a DLC map), for what the mod checks itself, as the Demo tab's PLAY of a
    // film of such a map; none: has them all again. The game itself still finds them.
    bool RunZoneHideCommand(const std::string& name, std::istringstream& args)
    {
        if (name != "zonehide")
            return false;
        std::set<std::string> zones;
        for (std::string zone; args >> zone;)
            if (zone != "none")
                zones.insert(zone);
        Log(std::format("ZONEHIDE {} zones hidden", zones.size()));
        ContentPacks::HideZoneFiles(std::move(zones));
        return true;
    }

    bool RunMapEffectsCommand(const std::string& name, std::istringstream& args)
    {
        // mapeffects [expect <least alive> | expect kind <name> <least playing, 0 for none> | showall]: logs the
        // campaign level's ambient effects the mod plays (CampaignMaps): how many it places,
        // spawned since the map started, kept, alive and of definitions not registered, and the Look tab's rows (name,
        // placed, playing as last counted, shown); PASS when at least that many are alive and none is missing, or a row
        // plays at least that many (0: none); showall shows every level's again, in config.json too
        if (name != "mapeffects")
            return false;
        std::string sub;
        args >> sub;
        if (sub == "showall")
        {
            auto& hidden = PreferencesConfiguration::Get().hiddenMapEffects;
            for (const auto& [level, definitions] : hidden.items())
                Mod::GetGameInterface()->SetHiddenMapEffects(level, {});
            hidden = nlohmann::json::object();
            Configuration::Get().Write(true);
            Log("MAPEFFECTS all shown");
            return true;
        }
        const auto state = CampaignMaps::GetMapEffectsState();
        const auto rows = Mod::GetGameInterface()->GetMapEffects();
        if (!state || !rows)
        {
            Log(std::format("{}MAPEFFECTS none", sub == "expect" ? "FAIL " : ""));
            return true;
        }
        Log(std::format("MAPEFFECTS {}: {} placed, {} spawned, {} kept, {} alive, {} of definitions not registered, "
                        "started at {}",
                        state->level, state->placed, state->spawned, state->kept, state->alive, state->missing,
                        state->startedAt));
        for (const auto& kind : rows->kinds)
            Log(std::format("MAPEFFECTS row {} placed {} playing {} {}", kind.name, kind.placed, kind.playing,
                            kind.shown ? "shown" : "hidden"));
        if (sub != "expect")
            return true;
        std::string what;
        args >> what;
        if (what == "kind")
        {
            std::string kindName;
            std::uint32_t least = 0;
            args >> kindName >> least;
            const auto row = std::ranges::find(rows->kinds, kindName, &Types::MapEffectKind::name);
            const bool passed =
                row != rows->kinds.end() && (least == 0 ? row->playing == 0 : row->playing >= least);
            Log(std::format("{} mapeffects kind {}: {} playing ({} {})", passed ? "PASS" : "FAIL", kindName,
                            row != rows->kinds.end() ? row->playing : 0, least == 0 ? "none" : "at least",
                            least == 0 ? "" : std::to_string(least)));
            return true;
        }
        const auto least = static_cast<std::uint32_t>(std::stoul(what.empty() ? "1" : what));
        const bool passed = state->alive >= least && state->missing == 0;
        Log(std::format("{} mapeffects {}: {} alive (at least {}), {} missing", passed ? "PASS" : "FAIL", state->level,
                        state->alive, least, state->missing));
        return true;
    }

    // A ReShade id's value as a key holds it (one number, three for a colour or a vector)
    std::optional<ReShade::Value> ParseReShadeValue(std::istringstream& args, std::size_t count)
    {
        ReShade::Value value{};
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!(args >> value[i]))
                return i == 0 ? std::nullopt : std::optional(value);
        }
        return value;
    }

    // The value of a uniform in ReShade's preset file ([<file>] <variable>=x,y,z), or a technique's on (whether its
    // Techniques= line has it)
    std::optional<ReShade::Value> ReadReShadePresetFile(const std::string& path, const std::string& id)
    {
        std::ifstream stream(std::filesystem::path(std::u8string(path.begin(), path.end())));
        if (!stream)
            return std::nullopt;
        const auto file = Components::ReShadeLogic::GetFile(id);
        const auto rest = id.substr(std::min(id.size(), file.size() + 1));
        const bool technique = rest.starts_with("technique:");
        std::string section, line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.starts_with("["))
            {
                section = line.substr(1, line.find(']') - 1);
                continue;
            }
            const auto equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const auto key = line.substr(0, equals);
            const auto text = line.substr(equals + 1);
            if (technique && section.empty() && key == "Techniques")
            {
                const auto name = rest.substr(10) + "@" + file;
                std::istringstream list(text);
                bool on = false;
                for (std::string each; std::getline(list, each, ',');)
                    on = on || each == name;
                return ReShade::Value{on ? 1.0f : 0.0f};
            }
            if (!technique && section == file && key == rest)
            {
                ReShade::Value value{};
                std::istringstream numbers(text);
                std::string number;
                for (std::size_t i = 0; i < value.size() && std::getline(numbers, number, ','); ++i)
                    value[i] = std::stof(number);
                return value;
            }
        }
        return technique ? std::optional(ReShade::Value{}) : std::nullopt;
    }

    // set by "reshade require" in a game without ReShade: the scenario's other ReShade commands log and pass by
    bool reshadeSkipped = false;

    bool RunReShadeCommand(const std::string& name, std::istringstream& args)
    {
        // The Look tab's ReShade section (ReShade, Components::ReShadeKeys); ids are
        // "<file>/<variable>" for a value and "<file>/technique:<name>" for an effect's on/off, and their properties
        // "ReShade:<id>" for keys, diamond and graph:
        //   reshade [list] [values]: ReShade's state, its preset, each effect (on or off in the preset and as drawn,
        //     keyed) and with values each value (its kind, the preset's and as drawn)
        //   reshade expect state <notfound|waiting|performancemode|ready>
        //   reshade set <id> <x> [y z]: as the row's control does: keys it at the playhead when it has keys, else the
        //     preset's own value, saved as ReShade's Auto Save does (one undo step)
        //   reshade key <id> [x [y z]]: as the row's diamond does: a key at the playhead, of the value given or as it
        //     is (the key there changes)
        //   reshade require: PASS, and in a game without ReShade the other reshade commands are left out from then on
        //     (a scenario's)
        //   reshade expect <id> <x> [y z] [tolerance]: PASS when ReShade drew the last frame with it (0.001); for x
        //     "preset" the preset's own value, "keyed" its keys' value at the timeline's tick
        //   reshade expect preset <id> <x> [y z] [tolerance]: the preset's own value, without keys
        //   reshade expect file <id> <x> [y z] [tolerance]: the value in ReShade's preset file
        //   reshade menu <id> <x> [y z] [save]: as ReShade's menu sets it (D1), with save as its Auto Save saves after
        //     ("preset" for x: its preset's own value, which a save then writes as it is)
        //   reshade expect clock paused: PASS when ReShade's timer and frame count stood still for 300 ms (E1)
        //   reshade expect restores <n>: PASS when the mod gave the preset its own values back for n of ReShade's
        //     saves while keys held values (Q1)
        //   reshade clock: logs ReShade's timer, frame time and frame count of the frame drawn last (E1)
        //   reshade open|close <technique id>: opens or closes its fold in the Look tab
        //   reshade trace start <id> | reshade trace check <id> <fps>: records the value keys give each frame drawn,
        //     then PASS when the frames drawn while capturing had ticks one frame (1000/fps ms) apart and each the
        //     value of its own tick
        //   reshade clear: deletes every ReShade key (one undo step)
        if (name != "reshade")
            return false;
        if (reshadeSkipped)
        {
            Log("RESHADE skipped: no ReShade in this game");
            return true;
        }
        namespace ReShadeLogic = Components::ReShadeLogic;
        namespace ReShadeKeys = Components::ReShadeKeys;
        std::string action;
        args >> action;
        auto& keyframeManager = Components::KeyframeManager::Get();
        const auto effects = ReShade::GetEffects();
        const auto status = ReShade::GetStatus();
        const auto statusName = [](ReShade::Status each) {
            std::string text(magic_enum::enum_name(each));
            std::ranges::transform(text, text.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        const auto needEffects = [&] {
            if (effects)
                return true;
            Log(std::format("FAIL reshade {}: no effects ({})", action, statusName(status)));
            return false;
        };
        const auto describe = [](const ReShade::Value& value, std::size_t count) {
            std::string text;
            for (std::size_t i = 0; i < count; ++i)
                text += std::format("{}{:.4f}", i ? "," : "", value[i]);
            return text;
        };
        const auto countOf = [&](const std::string& id) -> std::size_t {
            if (!effects)
                return 1;
            if (const auto index = effects->FindUniform(id))
                return std::min<std::size_t>(effects->uniforms[*index].components, 3);
            return 1;
        };

        if (action == "require")
        {
            reshadeSkipped = status == ReShade::Status::NotFound;
            Log(reshadeSkipped ? "PASS reshade require: no ReShade in this game, its checks are left out"
                               : std::format("PASS reshade require: {}", statusName(status)));
            return true;
        }
        if (action.empty() || action == "list")
        {
            std::string values;
            args >> values;
            Log(std::format("RESHADE {}{}", statusName(status),
                            effects ? std::format(", {} effects, {} values, preset {}", effects->techniques.size(),
                                                  effects->uniforms.size(), effects->presetPath)
                                    : ""));
            if (!effects)
                return true;
            const auto now = ReShade::GetValues();
            if (now.generation != effects->generation)
                return true;
            for (std::size_t i = 0; i < effects->techniques.size(); ++i)
            {
                const auto& technique = effects->techniques[i];
                std::size_t keys = 0;
                RunOnRenderThread([&] {
                    if (const auto type = ReShadeKeys::FindType(technique.id))
                        keys = keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type)).size();
                });
                Log(std::format("RESHADE effect {} preset {} drawn {} keys {}", technique.id,
                                now.presetOn[i] ? "on" : "off", now.drawnOn[i] ? "on" : "off", keys));
            }
            if (values != "values")
                return true;
            for (std::size_t i = 0; i < effects->uniforms.size(); ++i)
            {
                const auto& uniform = effects->uniforms[i];
                Log(std::format("RESHADE value {} {} \"{}\" [{} {}] preset {} drawn {}", uniform.id,
                                magic_enum::enum_name(uniform.kind), uniform.label, uniform.min, uniform.max,
                                describe(now.preset[i], uniform.components),
                                describe(now.drawn[i], uniform.components)));
            }
            return true;
        }
        if (action == "effects")
        {
            // reshade effects on|off: as ReShade's effects toggle key does; it loads them after a reset all the same
            std::string state;
            args >> state;
            ReShade::SetEffectsOn(state == "on");
            Sleep(300);
            Log(std::format("RESHADE effects {}, drawing them: {}", state, ReShade::IsDrawingEffects()));
            return true;
        }
        if (action == "loading")
        {
            // reshade loading <ms>: taken as loading its effects for that long, so a restart asked for is held back
            std::uint32_t ms = 0;
            args >> ms;
            ReShade::SetLoadingFor(ms);
            Log(std::format("RESHADE loading for {} ms", ms));
            return true;
        }
        if (action == "clock")
        {
            const auto clock = ReShadeKeys::GetLastClock();
            Log(clock ? std::format("RESHADE clock timer {:.1f} frametime {:.2f} framecount {}", clock->timer,
                                    clock->frameTime, clock->frameCount)
                      : "RESHADE clock ReShade's own");
            return true;
        }
        if (action == "open" || action == "close")
        {
            std::string id;
            args >> id;
            auto visualsMenu = UI::UIManager::Get().GetUIComponent<UI::VisualsMenu>(UI::Component::VisualsMenu);
            RunOnRenderThread([&] { visualsMenu->SetReShadeFoldOpen(id, action == "open"); });
            Log(std::format("RESHADE {} {}", action == "open" ? "opened" : "closed", id));
            return true;
        }
        if (action == "clear")
        {
            RunOnRenderThread([&] {
                Components::UndoHistory::Get().BeginGroup("clear the ReShade keys");
                for (const auto type : Components::RuntimeProperties::GetAll())
                {
                    if (!keyframeManager.HasProperty(type))
                        continue;
                    const auto& property = keyframeManager.GetProperty(type);
                    if (keyframeManager.GetKeyframes(property).empty())
                        continue;
                    keyframeManager.ClearKeyframes(property);
                    keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
                }
                Components::UndoHistory::Get().EndGroup();
            });
            Log("RESHADE cleared");
            return true;
        }
        if (action == "trace")
        {
            std::string what, id;
            args >> what >> id;
            if (what == "start")
            {
                ReShadeKeys::StartTrace(id);
                Log("RESHADE tracing " + id);
                return true;
            }
            double fps = 50;
            args >> fps;
            const auto frames = ReShadeKeys::StopTrace();
            if (!needEffects())
                return true;
            const auto index = effects->FindUniform(id);
            const auto type = ReShadeKeys::FindType(id);
            std::size_t captured = 0, wrongValue = 0, wrongStep = 0;
            std::optional<std::uint32_t> previous;
            std::uint32_t first = 0, last = 0;
            const auto step = 1000.0 / fps;
            RunOnRenderThread([&] {
                if (!type)
                    return;
                const auto& property = keyframeManager.GetProperty(*type);
                for (const auto& frame : frames)
                {
                    if (!frame.capturing)
                    {
                        previous.reset();
                        continue;
                    }
                    if (captured++ == 0)
                        first = frame.tick;
                    last = frame.tick;
                    const auto key = keyframeManager.Interpolate(property, frame.tick);
                    const auto expected = index ? ReShadeLogic::FromKey(effects->uniforms[*index], key, frame.value)
                                                : ReShade::Value{key.floatingPoint >= 0.5f ? 1.0f : 0.0f};
                    if (!frame.held || std::abs(expected[0] - frame.value[0]) > 1e-4f)
                        ++wrongValue;
                    // (the first frames of a capture are drawn while the demo still stands)
                    if (previous && *previous != frame.tick && std::abs((frame.tick - *previous) - step) > 1.0)
                        ++wrongStep;
                    previous = frame.tick;
                }
            });
            // (the captured frames' ticks and values, to match a recording's frames against)
            std::string ticks;
            for (const auto& frame : frames)
                if (frame.capturing)
                    ticks += std::format("{}:{:.3f} ", frame.tick, frame.value[0]);
            Log("RESHADE trace frames " + ticks);
            const bool passed = type && captured > 0 && wrongValue == 0 && wrongStep == 0;
            Log(std::format("{} reshade trace {}: {} frames drawn, {} while capturing from {} to {}, {} with another "
                            "value than their tick's, {} not a frame after the one before",
                            passed ? "PASS" : "FAIL", id, frames.size(), captured, first, last, wrongValue, wrongStep));
            return true;
        }
        if (action == "set" || action == "key" || action == "menu")
        {
            std::string id;
            args >> id;
            if (!needEffects())
                return true;
            const bool technique = effects->FindTechnique(id).has_value();
            const auto uniform = effects->FindUniform(id);
            if (!technique && !uniform)
            {
                Log(std::format("FAIL reshade {}: no {}", action, id));
                return true;
            }
            const auto count = countOf(id);
            std::optional<ReShade::Value> value;
            {
                const auto start = args.tellg();
                std::string word;
                args >> word;
                if (word == "preset")
                {
                    const auto now = ReShade::GetValues();
                    if (now.generation == effects->generation)
                        value = technique ? ReShade::Value{now.presetOn[*effects->FindTechnique(id)] ? 1.0f : 0.0f}
                                          : now.preset[*uniform];
                }
                else
                {
                    args.clear();
                    args.seekg(start);
                    value = ParseReShadeValue(args, count);
                }
            }
            if (action == "menu")
            {
                std::string save;
                args >> save;
                if (!value)
                {
                    Log("FAIL reshade menu: no value");
                    return true;
                }
                ReShade::SetAsMenu(id, *value, save == "save");
                Log(std::format("RESHADE menu {} {}{}", id, describe(*value, count), save == "save" ? " saved" : ""));
                return true;
            }
            if (!value && action == "set")
            {
                Log("FAIL reshade set: no value");
                return true;
            }
            RunOnRenderThread([&] {
                const auto type = ReShadeKeys::FindType(id);
                if (!type)
                    return;
                const auto& property = keyframeManager.GetProperty(*type);
                auto& keys = keyframeManager.GetKeyframes(property);
                const auto now = ReShade::GetValues();
                const auto preset = technique ? ReShade::Value{now.presetOn[*effects->FindTechnique(id)] ? 1.0f : 0.0f}
                                              : now.preset[*uniform];
                const auto tick = Components::Playback::GetTimelineTick();
                if (action == "set" && keys.empty())
                {
                    ReShadeKeys::ChangePreset(id, technique, preset, *value, "change " + id);
                    ReShadeKeys::EndPresetEditWhenIdle(false);
                    return;
                }
                // (as it is now, without a value)
                const auto key = value ? (technique ? Types::KeyframeValue((*value)[0])
                                                    : ReShadeLogic::ToKey(effects->uniforms[*uniform], *value))
                                       : (keys.empty() ? *ReShadeKeys::GetPresetKey(*type)
                                                       : keyframeManager.Interpolate(property, tick));
                if (const auto here = std::ranges::find(keys, tick, &Types::Keyframe::tick); here != keys.end())
                {
                    auto changed = *here;
                    changed.value = key;
                    keyframeManager.ChangeKeyframes(property, {changed}, "change " + id);
                }
                else
                {
                    keyframeManager.AddKeyframe(property, Types::Keyframe(property, tick, key));
                    keyframeManager.SortAndSaveKeyframes(keys);
                }
            });
            Log(std::format("RESHADE {} {}{}", action == "set" ? "set" : "keyed", id,
                            value ? " " + describe(*value, count) : ""));
            return true;
        }
        if (action == "expect")
        {
            std::string what;
            args >> what;
            if (what == "state")
            {
                std::string expected;
                args >> expected;
                Log(std::format("{} reshade state {} (expected {})", statusName(status) == expected ? "PASS" : "FAIL",
                                statusName(status), expected));
                return true;
            }
            if (what == "clock")
            {
                const auto before = ReShadeKeys::GetLastClock();
                Sleep(300);
                const auto after = ReShadeKeys::GetLastClock();
                const bool passed = before && after && before->timer == after->timer &&
                                    before->frameCount == after->frameCount;
                Log(std::format("{} reshade clock paused: timer {} then {}, frame count {} then {}",
                                passed ? "PASS" : "FAIL", before ? before->timer : -1.0f, after ? after->timer : -1.0f,
                                before ? before->frameCount : 0u, after ? after->frameCount : 0u));
                return true;
            }
            if (what == "restores")
            {
                std::uint32_t expected = 0;
                args >> expected;
                const auto restores = ReShade::GetPresetRestores();
                Log(std::format("{} reshade restores {} (expected {})", restores == expected ? "PASS" : "FAIL",
                                restores, expected));
                return true;
            }
            const bool preset = what == "preset";
            const bool file = what == "file";
            std::string id = what;
            if (preset || file)
                args >> id;
            if (!needEffects())
                return true;
            const auto count = countOf(id);
            const auto now = ReShade::GetValues();
            std::optional<ReShade::Value> expected;
            {
                const auto start = args.tellg();
                std::string word;
                args >> word;
                if (word == "preset" || word == "keyed")
                {
                    const auto uniform = effects->FindUniform(id);
                    const auto technique = effects->FindTechnique(id);
                    if (now.generation == effects->generation && word == "preset")
                    {
                        if (uniform)
                            expected = now.preset[*uniform];
                        else if (technique)
                            expected = ReShade::Value{now.presetOn[*technique] ? 1.0f : 0.0f};
                    }
                    else if (now.generation == effects->generation)
                    {
                        RunOnRenderThread([&] {
                            const auto type = ReShadeKeys::FindType(id);
                            if (!type)
                                return;
                            const auto key = keyframeManager.Interpolate(keyframeManager.GetProperty(*type),
                                                                         Components::Playback::GetTimelineTick());
                            if (uniform)
                                expected = ReShadeLogic::FromKey(effects->uniforms[*uniform], key, now.preset[*uniform]);
                            else
                                expected = ReShade::Value{key.floatingPoint >= 0.5f ? 1.0f : 0.0f};
                        });
                    }
                }
                else
                {
                    args.clear();
                    args.seekg(start);
                    expected = ParseReShadeValue(args, count);
                }
            }
            float tolerance = 0.001f;
            if (!(args >> tolerance))
                tolerance = 0.001f;
            std::optional<ReShade::Value> actual;
            if (file)
                actual = ReadReShadePresetFile(effects->presetPath, id);
            else if (const auto index = effects->FindUniform(id); index && now.generation == effects->generation)
                actual = preset ? now.preset[*index] : now.drawn[*index];
            else if (const auto technique = effects->FindTechnique(id); technique && now.generation == effects->generation)
                actual = ReShade::Value{(preset ? now.presetOn[*technique] : now.drawnOn[*technique]) ? 1.0f : 0.0f};
            bool passed = expected && actual;
            for (std::size_t i = 0; passed && i < count; ++i)
                passed = std::abs((*actual)[i] - (*expected)[i]) <= tolerance;
            Log(std::format("{} reshade {}{} is {} (expected {})", passed ? "PASS" : "FAIL",
                            preset ? "preset " : file ? "file " : "", id, actual ? describe(*actual, count) : "none",
                            expected ? describe(*expected, count) : "none"));
            return true;
        }
        Log(std::format("FAIL reshade: no action {}", action));
        return true;
    }

    // viewgun bones: the models and bones of the gun on screen (the gun the film's player holds in first person)
    // viewgun light <actor> [expect viewgun|body <units>]: where a light on the viewed player's bone was put in the
    // last
    //   frame drawn, whether by the gun on screen, and how far that is from where its bone on that gun and on the body
    //   put it now; PASS when that one placed it and it's that close to where it puts it
    bool RunViewGunCommand(const std::string& name, std::istringstream& args)
    {
        namespace LightLogic = Components::LightLogic;
        if (name != "viewgun")
            return false;
        std::string action;
        args >> action;
        if (action == "bones")
        {
            RunOnMainThread([] {
                const auto obj = Functions::Com_GetClientDObj(Lights::VIEW_GUN_HANDLE, 0);
                if (!obj)
                {
                    Log("VIEWGUN no DObj");
                    return;
                }
                for (std::uint32_t i = 0; i < obj->numModels; ++i)
                {
                    const auto bones = AnimationPose::GetBoneNames(*obj->models[i]);
                    std::string list;
                    for (const auto& bone : bones)
                        list += bone + " ";
                    Log(std::format("VIEWGUN model {} {}: {} bones: {}", i, obj->models[i]->name, bones.size(), list));
                }
            });
            return true;
        }
        if (action == "light")
        {
            std::string which, expect, source;
            float units = 0.0f;
            args >> which >> expect >> source >> units;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || !actor->light || !actor->light->follow)
            {
                Log(std::format("FAIL viewgun light: {} isn't a light on a bone", which));
                return true;
            }
            // (a frame drawn after this was asked)
            const auto start = mainThreadFrames.load();
            while (mainThreadFrames.load() < start + 3)
                Sleep(5);
            auto placed = Lights::GetPlacedLights();
            std::erase_if(placed, [&](const auto& each) { return each.id != actor->light->id; });
            if (placed.empty())
            {
                Log("FAIL viewgun light: it wasn't placed");
                return true;
            }
            std::optional<glm::vec3> onGun, onBody;
            std::string gunBone;
            RunOnMainThread([&] {
                const auto keyed =
                    Components::Lights::WithKeyedPlacement(*actor, Components::Playback::GetTimelineTick());
                const auto& follow = *keyed.light->follow;
                if (const auto bone = Bones::GetTargetBone(follow.target, follow.bone))
                    onBody = LightLogic::Place(keyed, bone->position, bone->axes).position;
                const auto obj = Functions::Com_GetClientDObj(Lights::VIEW_GUN_HANDLE, 0);
                const auto pose = Functions::CG_GetPose(0, Lights::VIEW_GUN_HANDLE);
                if (!obj || !pose || obj->numModels == 0)
                    return;
                const auto arms = AnimationPose::GetBoneNames(*obj->models[0]);
                std::vector<std::string> gun;
                for (std::uint32_t i = 1; i < obj->numModels; ++i)
                    for (auto& bone : AnimationPose::GetBoneNames(*obj->models[i]))
                        gun.push_back(std::move(bone));
                const auto bone = LightLogic::GetViewGunBone(Types::LightFirstPerson::ViewGun, follow.bone, arms, gun);
                float axis[3][3] = {}, origin[3] = {};
                if (bone && Functions::CG_DObjGetWorldTagMatrix(pose, obj, Bones::GetScriptString(*bone), axis, origin))
                {
                    gunBone = *bone;
                    onGun = LightLogic::Place(keyed, glm::make_vec3(origin), glm::make_mat3(&axis[0][0])).position;
                }
            });
            const auto& last = placed.back();
            const auto away = [&](const std::optional<glm::vec3>& point) {
                return point ? std::format("{:.2f}", glm::distance(*point, last.position)) : std::string("none");
            };
            const auto [cached, made] = Lights::GetViewGunSkeletons();
            Log(std::format(
                "VIEWGUN light {} placed by {} at {:.1f} {:.1f} {:.1f}: from its bone on the gun on screen "
                "({}) {}, on the body {}; the gun's skeleton the game's {} times, made by the light {}",
                actor->id, last.viewGun ? "the gun on screen" : "the body", last.position.x, last.position.y,
                last.position.z, gunBone.empty() ? "none" : gunBone, away(onGun), away(onBody), cached, made));
            if (expect == "expect")
            {
                const bool byGun = source == "viewgun";
                const auto& point = byGun ? onGun : onBody;
                const bool passed = last.viewGun == byGun && point && glm::distance(*point, last.position) <= units;
                Log(std::format("{} viewgun light {} {}: {}", passed ? "PASS" : "FAIL", source, units, away(point)));
            }
            return true;
        }
        Log(std::format("FAIL viewgun: no {}", action));
        return true;
    }

    bool RunSpeedCommand(const std::string& name, std::istringstream& args)
    {
        // speed watch on|off: logs (SPEED) every change of the timescale dvar on the main thread
        // speed info: logs the demo's system info, which the game sets dvars from
        if (name != "speed")
            return false;
        std::string what;
        args >> what;
        if (what == "watch")
        {
            std::string state;
            args >> state;
            speedSeen = -1.0f;
            watchingSpeed = state != "off";
        }
        else if (what == "info")
        {
            RunOnMainThread([&] {
                const auto info = Structures::GetConfigString(Structures::CS_SYSTEMINFO);
                Log(std::format("SYSTEMINFO {}", info ? info : ""));
            });
        }
        else
            Log(std::format("FAIL speed: no {}", what));
        return true;
    }

    // the fields of a gun's weapon file its shots and its place on a back go by, for the checks of each gun
    void LogGunInfo(const std::string& name)
    {
        const auto variant = WeaponRig::FindVariant(name);
        const auto def = variant ? variant->weapDef : nullptr;
        if (!def)
        {
            Log("GUN " + name + " isn't there");
            return;
        }
        const auto effect = [](void* fx) { return fx ? *static_cast<const char* const*>(fx) : "none"; };
        const auto model = [](const Structures::XModel* xmodel) { return xmodel ? xmodel->name : "none"; };
        Log(std::format("GUN {} type {} class {} anim {} impact {} shots {} fire {} ms damage {} sound {} flash {} casing {} "
                        "model {} model2 {} stowed {} grip {:.1f} {:.1f} {:.1f} turn {:.1f} {:.1f} {:.1f}",
                        name, def->weapType, def->weapClass, def->playerAnimType, def->impactType, def->shotCount,
                        def->iFireTime, def->iDamage, WeaponRig::GetShotSound(*variant) ? WeaponRig::GetShotSound(*variant)
                                                                                        : "none",
                        effect(def->worldFlashEffect), effect(def->worldShellEjectEffect),
                        model(def->worldModel ? def->worldModel[0] : nullptr),
                        model(def->worldModel ? def->worldModel[1] : nullptr),
                        model(WeaponRig::GetStowedModel(*variant)), variant->ikLeftHandOffset[0],
                        variant->ikLeftHandOffset[1], variant->ikLeftHandOffset[2], variant->ikLeftHandRotation[0],
                        variant->ikLeftHandRotation[1], variant->ikLeftHandRotation[2]));
        // (a bomb's too, the Mortar Team's mortar_mp)
        if (def->weapType == Structures::WEAPTYPE_PROJECTILE || def->projectileModel)
            Log(std::format("GUN {} projectile {} speed {} trail {} blast {} blast sound {}", name,
                            model(def->projectileModel), def->iProjectileSpeed, effect(def->projTrailEffect),
                            effect(def->projExplosionEffect),
                            def->projExplosionSound ? def->projExplosionSound : "none"));
    }

    // guninfo <weapon variant>...: logs each as above
    // impacts <weapon variant> <surface>... [flags <n>]: logs the effect the game's impact table has for each surface
    // hits [reset] | hits expect <part of a line> <at least> [<at most>] | hits at <x y z> <distance>: logs what actors' bullets
    // left where they landed since the reset, or PASS when lines with the part add up to that many, or the last landed there
    // hits event <actor> <event> on|off, hits spread <actor> <event> <degrees>: a Fire event's Hits box and Spread
    bool RunGunCommand(const std::string& name, std::istringstream& args)
    {
        if (name == "guninfo")
        {
            RunOnMainThread([&] {
                for (std::string gun; args >> gun;)
                    LogGunInfo(gun);
            });
            return true;
        }
        if (name == "impacts")
        {
            std::string gun, word;
            args >> gun;
            std::vector<std::uint32_t> surfaces;
            std::uint32_t flags = 0;
            while (args >> word)
            {
                if (word == "flags")
                    args >> flags;
                else
                    surfaces.push_back(static_cast<std::uint32_t>(std::stoul(word)));
            }
            RunOnMainThread([&] {
                const auto variants = reinterpret_cast<Structures::WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
                std::uint32_t index = 0;
                for (std::size_t i = 1; i < Functions::GetWeaponVariantCount(); ++i)
                    if (variants[i] && variants[i]->szInternalName && gun == variants[i]->szInternalName)
                        index = static_cast<std::uint32_t>(i);
                for (const auto surface : surfaces)
                {
                    void* effect = nullptr;
                    std::uint32_t sound = 0;
                    Functions::CG_GetImpactEffect(index, surface, flags, effect, sound);
                    Log(std::format("IMPACT {} surface {} flags {}: {} sound {:x}", gun, surface, flags,
                                    effect ? *static_cast<const char* const*>(effect) : "none", sound));
                }
            });
            return true;
        }
        if (name == "rockets")
        {
            // rockets [expect <n>]: logs the launchers' rockets in the air, each by its model and where it is; PASS when there
            // are that many
            std::string mode;
            std::size_t wanted = 0;
            args >> mode >> wanted;
            std::vector<std::pair<std::string, glm::vec3>> flying;
            RunOnMainThread([&] { flying = Actors::GetRockets(); });
            for (const auto& [model, at] : flying)
                Log(std::format("ROCKET {} at {:.1f} {:.1f} {:.1f}", model, at.x, at.y, at.z));
            if (mode == "expect")
                Log(std::format("{} {} rockets in the air (expected {})", flying.size() == wanted ? "PASS" : "FAIL",
                                flying.size(), wanted));
            return true;
        }
        if (name == "firetiming")
        {
            // firetiming <actor> <event> expect <shots> <interval>: PASS when a Fire event is that long
            std::string which, word;
            std::size_t index = 0;
            std::uint32_t shots = 0, interval = 0;
            args >> which >> index >> word >> shots >> interval;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || index >= actor->events.size())
            {
                Log(std::format("FAIL firetiming: no event {} on {}", index, which));
                return true;
            }
            const auto& event = actor->events[index];
            Log(std::format("{} actor {} Fire of {} shots every {} ms (expected {} every {})",
                            event.shots == shots && event.interval == interval ? "PASS" : "FAIL", actor->id, event.shots,
                            event.interval, shots, interval));
            return true;
        }
        if (name == "fireloop")
        {
            // fireloop <actor> expect playing|none: PASS when its firing loop sounds, or doesn't
            std::string which, word, wanted;
            args >> which >> word >> wanted;
            bool playing = false;
            RunOnMainThread([&] { playing = Actors::IsFireLoopPlaying(ParseActorId(which)); });
            Log(std::format("{} actor {} firing loop {} (expected {})", playing == (wanted == "playing") ? "PASS" : "FAIL",
                            which, playing ? "playing" : "none", wanted));
            return true;
        }
        if (name == "weaponlisted")
        {
            // weaponlisted <weapon> expect actors|players|none: PASS when the Weapon menu lists it for actors only, for
            // players' rules too, or not at all
            std::string weapon, word, wanted;
            args >> weapon >> word >> wanted;
            const auto catalog = Components::Loadouts::GetCatalog();
            const auto found = catalog ? catalog->FindWeapon(weapon) : nullptr;
            const std::string seen = !found ? "none" : found->actorsOnly ? "actors" : "players";
            Log(std::format("{} {} listed for {}, in {} (expected {})", seen == wanted ? "PASS" : "FAIL", weapon, seen,
                            found ? found->category + " as " + found->displayName : "no group", wanted));
            return true;
        }
        if (name == "stowed")
        {
            // stowed <actor> <weapon|-> [camo [attachment...]] | stowed <actor> expect <weapon|-> | stowed rig on|off: gives an
            // actor a gun on its back; or PASS when it has that one; or puts every stowed gun on the rig of a body without the
            // tag
            std::string which, weapon;
            args >> which >> weapon;
            if (which == "rig")
            {
                Actors::SetStowedRigForced(weapon == "on");
                Log("STOWED rig " + weapon);
                return true;
            }
            auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor)
            {
                Log("STOWED no such actor " + which);
                return true;
            }
            if (weapon == "variant" || weapon == "camo")
            {
                // (the variant its gun and chips come to, or its camo)
                std::string wanted;
                args >> wanted;
                const auto has = weapon == "camo" ? std::to_string(actor->stowedCamo)
                                                  : Components::Actors::ResolveStowedWeapon(*actor);
                Log(std::format("{} actor {} stowed {} {} (expected {})", has == wanted ? "PASS" : "FAIL", actor->id, weapon,
                                has, wanted));
                return true;
            }
            if (weapon == "expect")
            {
                args >> weapon;
                const auto has = actor->stowedWeapon.empty() ? std::string("-") : actor->stowedWeapon;
                Log(std::format("{} actor {} stowed {} (expected {})", has == weapon ? "PASS" : "FAIL", actor->id, has, weapon));
                return true;
            }
            actor->stowedWeapon = weapon == "-" ? std::string() : weapon;
            actor->stowedCamo = 0;
            actor->stowedAttachments.clear();
            args >> actor->stowedCamo;
            for (std::string attachment; args >> attachment;)
                actor->stowedAttachments.push_back(attachment);
            std::ranges::sort(actor->stowedAttachments);
            Components::Actors::Update(*actor);
            Log(std::format("STOWED {} on actor {}", weapon, actor->id));
            return true;
        }
        if (name == "builtmodels")
        {
            // builtmodels <actor> [expect <part of a line>]: logs the models an actor is drawn from, each with its tag; with
            // expect, PASS when a line has the part (no: when none has it)
            std::string which, mode, part;
            args >> which >> mode >> part;
            std::vector<std::string> lines;
            RunOnMainThread([&] { lines = Actors::GetBuiltModelTags(ParseActorId(which)); });
            bool found = false;
            for (const auto& line : lines)
            {
                found |= !part.empty() && line.find(part) != std::string::npos;
                Log("BUILT " + line);
            }
            if (mode == "expect" || mode == "no")
                Log(std::format("{} actor {} {} {}", found == (mode == "expect") ? "PASS" : "FAIL", which,
                                mode == "expect" ? "has" : "hasn't", part));
            return true;
        }
        if (name != "hits")
            return false;
        std::string what;
        args >> what;
        if (what == "check")
        {
            // (PASS when a Fire event's Hits box and Spread are so)
            std::string which, wanted;
            std::size_t index = 0;
            float spread = 0.0f, tolerance = 0.0f;
            args >> which >> index >> wanted >> spread >> tolerance;
            const auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || index >= actor->events.size())
            {
                Log(std::format("FAIL hits check: no event {} on {}", index, which));
                return true;
            }
            const auto& event = actor->events[index];
            Log(std::format("{} actor {} Fire hits {} spread {:.2f} (expected {} {:.2f})",
                            event.hits == (wanted == "on") && std::abs(event.spread - spread) <= tolerance ? "PASS" : "FAIL",
                            actor->id, event.hits ? "on" : "off", event.spread, wanted, spread));
            return true;
        }
        if (what == "event" || what == "spread")
        {
            // (a Fire event's Hits box, or its Spread)
            std::string which, value;
            std::size_t index = 0;
            args >> which >> index >> value;
            auto actor = Components::Actors::GetActor(ParseActorId(which));
            if (!actor || index >= actor->events.size())
            {
                Log(std::format("HITS no event {} on {}", index, which));
                return true;
            }
            if (what == "event")
                actor->events[index].hits = value != "off";
            else
                actor->events[index].spread = std::stof(value);
            Components::Actors::Update(*actor);
            Log(std::format("HITS {} {} on {} event {}", what, value, actor->id, index));
            return true;
        }
        std::map<std::string, std::size_t> counts;
        std::optional<glm::vec3> last;
        RunOnMainThread([&] {
            counts = Actors::GetHitCounts(what == "reset");
            last = Actors::GetLastHit();
        });
        if (what == "expect")
        {
            std::string part;
            std::size_t least = 0, most = 0;
            args >> part >> least;
            const bool bounded = static_cast<bool>(args >> most);
            // (a ~ for a space)
            std::ranges::replace(part, '~', ' ');
            std::size_t total = 0;
            for (const auto& [line, count] : counts)
                if (line.find(part) != std::string::npos)
                    total += count;
            Log(std::format("{} hits {}: {} (expected at least {}{})",
                            total >= least && (!bounded || total <= most) ? "PASS" : "FAIL", part, total, least,
                            bounded ? std::format(", at most {}", most) : ""));
            return true;
        }
        if (what == "at")
        {
            glm::vec3 point(0.0f);
            float distance = 0.0f;
            args >> point.x >> point.y >> point.z >> distance;
            const auto off = last ? glm::distance(*last, point) : -1.0f;
            Log(std::format("{} last hit {:.1f} from {:.1f} {:.1f} {:.1f} (at most {:.1f})",
                            last && off <= distance ? "PASS" : "FAIL", off, point.x, point.y, point.z, distance));
            return true;
        }
        for (const auto& [line, count] : counts)
            Log(std::format("HITS {} x{}", line, count));
        Log(std::format("HITS {} kinds, last at {}", counts.size(),
                        last ? std::format("{:.1f} {:.1f} {:.1f}", last->x, last->y, last->z) : "none"));
        return true;
    }

    void Run(const std::string& command)
    {
        Log("COMMAND " + command);
        std::istringstream args(command);
        std::string name;
        args >> name;

        // some commands are in functions of their own, as the chain below is as long as the compiler takes
        if (RunCharacterZoneCommand(name, args) || RunDiagnosticCommand(name, args) || RunVisualsCommand(name, args) ||
            RunLightCommand(name, args) || RunAnimationCommand(name, args) || RunCatalogCommand(name, args) ||
            RunPlacementCommand(name, args) || RunStoreCommand(name, args) || RunKindCommand(name, args) ||
            RunTransitionCommand(name, args) || RunBoneCameraCommand(name, args) || RunBulletTimeCommand(name, args) ||
            RunKillstreakCommand(name, args) || RunPlacedPropCommand(name, args) || RunDemoLoadCommand(name, args) ||
            RunGameModCommand(name, args) ||
            RunAutoEaseCommand(name, args) || RunFxWatchCommand(name, args) || RunSelectEventCommand(name, args) ||
            RunLeftGunCommand(name, args) || RunWorkflowCommand(name, args) || RunFacingCommand(name, args) ||
            RunClipKeepCommand(name, args) || RunPathBarCommand(name, args) || RunWalkPieceCommand(name, args) ||
            RunTimelineScrollCommand(name, args) || RunViewLagCommand(name, args) || RunViewLinesCommand(name, args) ||
            RunPoolsCommand(name, args) || RunCampaignMapCommand(name, args) || RunMapOptionsCommand(name, args) ||
            RunCaptureCheckCommand(name, args) || RunSpeedCommand(name, args) || RunViewGunCommand(name, args) ||
            RunReloadGuessCommand(name, args) || RunFxSpawnsCommand(name, args) || RunDeathSuitsCommand(name, args) ||
            RunShadowMapCommand(name, args) || RunModelLightingCommand(name, args) || RunClientEntityCommand(name, args) ||
            RunEntScanCommand(name, args) ||
            RunZoneTexturesCommand(name, args) || RunAreaCommand(name, args) ||
            RunFxFillCommand(name, args) || RunHeadTurnCommand(name, args) || RunReadoutsCommand(name, args) ||
            RunCommandConflictsCommand(name, args) || RunMarkersCommand(name, args) ||
            RunHeldPressCommand(name, args) ||
            RunFxListCommand(name, args) || RunSceneEffectCommand(name, args) || RunVisionCommand(name, args) ||
            RunMapEffectsCommand(name, args) || RunZoneHideCommand(name, args) || RunPlayerBodiesCommand(name, args) ||
            RunMuzzlesCommand(name, args) || RunWeaponCatalogCommand(name, args) || RunLiveCommand(name, args) ||
            RunGripCommand(name, args) || RunGripSurveyCommand(name, args) || RunPlayerPoseCommand(name, args) ||
            RunMapMenuCommand(name, args) || RunReShadeCommand(name, args) || RunSurfaceCommand(name, args) ||
            RunTexturePackCommand(name, args) || RunScreenCommand(name, args) || RunRestartCommand(name, args) ||
            RunGunCommand(name, args) || RunYouCommand(name, args) || RunBodiesCommand(name, args))
            return;
        if (name == "sleep")
        {
            int ms = 0;
            args >> ms;
            Sleep(ms);
        }
        else if (name == "eject")
        {
            // eject [raw]: unloads the mod as File > Exit does; raw leaves out the game letting go of what it holds of
            // the mod first (T5Interface::PrepareUnload). The harness goes with it, logging EJECTED; injected again
            // (launch.ps1 -Reinject), it goes on with the next line of live.txt.
            std::string raw;
            args >> raw;
            skipPrepareUnload = raw == "raw";
            Log(std::format("EJECTING{}", skipPrepareUnload ? " without letting go first" : ""));
            ejecting = true;
        }
        else if (name == "gamestate")
        {
            // gamestate: logs whether the game is at the menu, in a demo or a game, whether a demo plays and cg.time,
            // with where the game keeps each (for a script to read them from outside, as after "eject")
            const auto clc = Structures::GetClientConnection();
            const auto cg = Structures::GetClientGlobals();
            const auto inGame = Functions::FindDvar("cl_ingame");
            Log(std::format("STATE {} demoplaying {} at {:X}, cl_ingame {} at {:X}, cg.time {} at {:X}",
                            magic_enum::enum_name(Mod::GetGameInterface()->GetGameState()), clc->demoplaying,
                            reinterpret_cast<std::uintptr_t>(&clc->demoplaying),
                            inGame && inGame->current.enabled ? 1 : 0,
                            inGame ? reinterpret_cast<std::uintptr_t>(&inGame->current.enabled) : 0, cg ? cg->time : 0,
                            cg ? reinterpret_cast<std::uintptr_t>(&cg->time) : 0));
        }
        else if (name == "config")
        {
            // config expect <preference> <json>: PASS when config.json's preferences have that value, as json
            // (config expect actorsAdvanced ["clip"])
            std::string action, preference, expected;
            args >> action >> preference;
            std::getline(args >> std::ws, expected);
            try
            {
                std::ifstream file(PathUtils::GetModPath() / "config.json");
                const auto saved = nlohmann::json::parse(file)["preferences"][preference];
                const auto wanted = nlohmann::json::parse(expected);
                Log(std::format("{} config {} is {} (expected {})", saved == wanted ? "PASS" : "FAIL", preference,
                                saved.dump(), wanted.dump()));
            }
            catch (const std::exception& e)
            {
                Log(std::format("FAIL config {}: {}", preference, e.what()));
            }
        }
        else if (name == "until")
        {
            // until <cg.time>
            int target = 0;
            args >> target;
            int pausedFor = 0;
            while (true)
            {
                const auto cg = Structures::GetClientGlobals();
                if (cg && cg->time >= target && !Components::Rewinding::IsRewinding())
                    break;
                if (IsTimeWaitLost(pausedFor))
                {
                    Log(std::format("FAIL until {}: the demo is paused", target));
                    break;
                }
                Sleep(5);
            }
        }
        else if (name == "untiltick")
        {
            // untiltick <tick of the demo's timeline>
            std::uint32_t target = 0;
            args >> target;
            int pausedFor = 0;
            while (Mod::GetGameInterface()->GetDemoInfo().gameTick < target || Components::Rewinding::IsRewinding())
            {
                if (IsTimeWaitLost(pausedFor))
                {
                    Log(std::format("FAIL untiltick {}: the demo is paused at {}", target,
                                    Mod::GetGameInterface()->GetDemoInfo().gameTick));
                    break;
                }
                Sleep(5);
            }
        }
        else if (name == "menus")
        {
            // menus [all]: the menus shown when last checked, or all that were checked
            std::string which;
            args >> which;
            for (std::size_t i = 0; i < MENU_SLOTS; i++)
            {
                const auto menu = menus[i].load();
                if (!menu || (!menuVisible[i] && which != "all"))
                    continue;
                Log(std::format("MENU '{}' visible {} show {:#018x} hide {:#018x}", menu->name ? menu->name : "?",
                                menuVisible[i].load(), menu->showBits, menu->hideBits()));
            }
        }
        else if (name == "ret" || name == "unret")
        {
            // ret <hex address>: makes a cdecl function return at once, to find what draws something; unret undoes it
            static std::map<std::uintptr_t, std::uint8_t> originals;
            std::uintptr_t address = 0;
            args >> std::hex >> address;
            auto code = reinterpret_cast<std::uint8_t*>(address);
            DWORD protection;
            VirtualProtect(code, 1, PAGE_EXECUTE_READWRITE, &protection);
            if (name == "ret" && !originals.contains(address))
            {
                originals[address] = *code;
                *code = 0xC3;
            }
            else if (name == "unret" && originals.contains(address))
            {
                *code = originals[address];
                originals.erase(address);
            }
            VirtualProtect(code, 1, protection, &protection);
        }
        else if (name == "rewind")
        {
            // rewind <ticks>, negative to go back
            int ticks = 0;
            args >> ticks;
            Components::Rewinding::RewindBy(ticks);
            WaitForRewind();
            Log("REWOUND");
        }
        else if (name == "jump")
        {
            // jump <tick>: goes to a tick of the timeline as the actors UI does (rewinding or skipping the demo) and
            // waits until the timeline is close to it
            std::uint32_t target = 0;
            args >> target;
            // (from where the demo is: right after "freeze off" it goes back to the frozen tick, rewinding or catching
            // up, and a skip worked out from before that overshot by seconds)
            WaitForRewind();
            for (int i = 0; i < 50; i++)
            {
                const auto before = Components::Playback::GetTimelineTick();
                Sleep(100);
                const auto after = Components::Playback::GetTimelineTick();
                if (!Components::Rewinding::IsRewinding() && after >= before && after - before <= 300)
                    break;
            }
            // (through the bullet time spans, which put the demo's tick elsewhere)
            const auto from = Components::Playback::GetTimelineTick();
            if (target != from)
                Components::Playback::SeekTimeline(target);
            WaitForRewind();
            // (a frame that stalls right after the skip, e.g. compiling shaders for what it shows, moves the demo on by
            // com_maxFrameTime, so it can be a little past the tick once seen; waiting for the exact tick while the demo
            // went on playing took 15 s, after which a scenario's events were long gone)
            for (int i = 0; i < 3000; i++)
            {
                const auto offset = static_cast<std::int64_t>(Components::Playback::GetTimelineTick()) -
                                    static_cast<std::int64_t>(target);
                if (offset >= -50 && offset <= 500 && !Components::Rewinding::IsRewinding())
                    break;
                // (the timeline starts seconds before a demo's first frame: a rewind stops there, and waiting
                // for the tick let the demo play on for 15 s)
                if (target < from && offset > 500 && i >= 60 && !Components::Rewinding::IsRewinding() &&
                    Components::Playback::GetTimelineTick() < from)
                    break;
                Sleep(5);
            }
            Log(std::format("JUMPED {} now {}", target, Components::Playback::GetTimelineTick()));
        }
        else if (name == "freeze")
        {
            // freeze [on|off]: toggles the frozen tick, or turns it on or off; while it is on the demo stops and the
            // timeline's tick is set freely (see seek)
            std::string state;
            args >> state;
            if (state.empty() || (state == "on") != Components::Playback::IsGameFrozen())
                Components::Playback::ToggleFrozenTick();
            Log(std::format("FROZEN {} tick {}", Components::Playback::IsGameFrozen(),
                            Components::Playback::GetTimelineTick()));
        }
        else if (name == "seek")
        {
            // seek <tick>: sets the timeline's tick exactly while it is frozen, and where it froze with it (actors are
            // posed at the tick while the demo stays: the timeline's own seeks move the playhead alone), and waits a
            // few frames for it to show; pause too, or the frozen timeline goes on from there
            std::uint32_t target = 0;
            args >> target;
            if (!Components::Playback::IsGameFrozen())
            {
                Log("SEEK needs freeze");
            }
            else
            {
                Components::Playback::MoveFrozenTick(target);
                Sleep(150);
                Log(std::format("SEEK {}", Components::Playback::GetTimelineTick()));
            }
        }
        else if (name == "snap")
        {
            // snap <label> <actor id> [bones...]: records where an actor's bones are now under a label (see same)
            std::string label, actorId;
            args >> label >> actorId;
            const auto id = ParseActorId(actorId);
            std::vector<std::string> bones;
            for (std::string bone; args >> bone;)
                bones.push_back(bone);
            if (bones.empty())
                bones = {"j_head",     "j_mainroot", "j_spine4", "j_wrist_le", "j_wrist_ri",
                         "j_ankle_le", "j_ankle_ri", "j_knee_le", "j_knee_ri", "tag_flash"};
            std::vector<std::optional<glm::vec3>> positions;
            RunOnMainThread([&] { positions = Actors::GetBonePositions(id, bones); });
            snaps[label] = {bones, positions};
            std::string text;
            for (std::size_t i = 0; i < positions.size(); ++i)
                if (positions[i])
                    text += std::format("{}:{:.1f},{:.1f},{:.1f} ", bones[i], positions[i]->x, positions[i]->y,
                                        positions[i]->z);
            Log(std::format("SNAP {} tick {} {}", label, Components::Playback::GetTimelineTick(),
                            positions.empty() ? "none" : text));
        }
        else if (name == "same")
        {
            // same <labelA> <labelB> <tolerance>: PASS when every bone of two snaps is within the tolerance
            std::string a, b;
            float tolerance = 0.5f;
            args >> a >> b >> tolerance;
            const auto first = snaps.find(a), second = snaps.find(b);
            if (first == snaps.end() || second == snaps.end() || first->second.second.empty() ||
                first->second.second.size() != second->second.second.size())
            {
                Log(std::format("FAIL same {} {}: missing snaps", a, b));
            }
            else
            {
                float largest = 0.0f;
                std::string worst;
                for (std::size_t i = 0; i < first->second.second.size(); ++i)
                {
                    const auto& p = first->second.second[i];
                    const auto& q = second->second.second[i];
                    if (!p || !q)
                        continue;
                    if (const auto d = glm::distance(*p, *q); d > largest)
                    {
                        largest = d;
                        worst = first->second.first[i];
                    }
                }
                Log(std::format("{} same {} {}: largest {:.2f} ({})", largest <= tolerance ? "PASS" : "FAIL", a, b,
                                largest, worst));
            }
        }
        else if (name == "differ")
        {
            // differ <labelA> <labelB> <distance>: PASS when some bone moved at least the distance between two snaps
            std::string a, b;
            float distance = 1.0f;
            args >> a >> b >> distance;
            const auto first = snaps.find(a), second = snaps.find(b);
            float largest = 0.0f;
            if (first != snaps.end() && second != snaps.end() &&
                first->second.second.size() == second->second.second.size())
                for (std::size_t i = 0; i < first->second.second.size(); ++i)
                    if (first->second.second[i] && second->second.second[i])
                        largest = std::max(largest,
                                           glm::distance(*first->second.second[i], *second->second.second[i]));
            Log(std::format("{} differ {} {}: largest {:.2f}", largest >= distance ? "PASS" : "FAIL", a, b, largest));
        }
        else if (command.starts_with("effects looping"))
        {
            // effects looping [expect current | expect forgotten <n>]: logs the looping effects kept track of (see
            // Hooks/Effects.cpp), how many still play and how many were forgotten once gone; expect current: PASS
            // once every one kept track of still plays (within 3 s), expect forgotten: PASS when at least n were
            std::string word, expect, what;
            std::size_t n = 0;
            args >> word >> expect >> what >> n;
            Hooks::Effects::LoopingCounts counts;
            RunOnMainThread([&] { counts = Hooks::Effects::CountLooping(); });
            for (int i = 0; expect == "expect" && what == "current" && counts.tracked != counts.alive && i < 30; ++i)
            {
                Sleep(100);
                RunOnMainThread([&] { counts = Hooks::Effects::CountLooping(); });
            }
            const auto text = std::format("looping effects {} kept track of, {} playing, {} forgotten", counts.tracked,
                                          counts.alive, counts.forgotten);
            if (expect != "expect")
                Log("EFFECTS " + text);
            else if (what == "current")
                Log(std::format("{} {} (all playing)", counts.tracked == counts.alive ? "PASS" : "FAIL", text));
            else
                Log(std::format("{} {} (at least {} forgotten)", counts.forgotten >= n ? "PASS" : "FAIL", text, n));
        }
        else if (name == "effects")
        {
            // effects [max [name part]]: logs how many of the client's 1024 effects are in use, the effects most in use
            // and how many have the part in their name; with max, PASS when at most that many are in use
            std::size_t max = 0;
            std::string part;
            const bool limited = static_cast<bool>(args >> max);
            args >> part;
            Hooks::Effects::EffectCounts counts;
            RunOnMainThread([&] { counts = Hooks::Effects::CountEffects(); });
            std::string most;
            std::size_t named = 0;
            for (std::size_t i = 0; i < counts.most.size(); ++i)
            {
                if (i < 5)
                    most += std::format(" {} x{}", counts.most[i].first, counts.most[i].second);
                if (!part.empty() && counts.most[i].first.find(part) != std::string::npos)
                    named += counts.most[i].second;
            }
            const auto text =
                std::format("effects {} in use{}, refused {} since the map loaded, most:{}", counts.used,
                            part.empty() ? std::string() : std::format(", {} x{}", part, named), counts.refused, most);
            if (limited)
                Log(std::format("{} {} (at most {})", counts.used <= max ? "PASS" : "FAIL", text, max));
            else
                Log("EFFECTS " + text);
        }
        else if (name == "fx")
        {
            // fx [max | min <n>]: logs how many effects the actors started are still playing; with max, PASS when
            // at most that many are, with min, when at least n are
            std::size_t live = 0;
            RunOnMainThread([&] { live = Actors::CountLiveEffects(); });
            std::string limit;
            args >> limit;
            std::size_t n = 0;
            if (limit == "min" && args >> n)
                Log(std::format("{} fx live {} (at least {})", live >= n ? "PASS" : "FAIL", live, n));
            else if (!limit.empty())
                Log(std::format("{} fx live {} (at most {})", live <= std::stoul(limit) ? "PASS" : "FAIL", live, limit));
            else
                Log(std::format("FX live {}", live));
        }
        else if (name == "animbones")
        {
            std::string animation;
            args >> animation;
            std::string text;
            RunOnMainThread([&] { text = Actors::DescribeAnimationBones(animation); });
            Log(std::format("ANIMBONES {}: {}", animation, text));
        }
        else if (name == "boneaxes")
        {
            std::string actorId;
            args >> actorId;
            std::vector<std::string> bones;
            for (std::string bone; args >> bone;)
                bones.push_back(bone);
            std::vector<std::optional<glm::mat4>> matrices;
            RunOnMainThread([&] { matrices = Actors::GetBoneMatrices(ParseActorId(actorId), bones); });
            std::string text;
            for (std::size_t i = 0; i < matrices.size(); ++i)
            {
                if (!matrices[i])
                    continue;
                const auto& m = *matrices[i];
                text += std::format(
                    "{} o({:.1f} {:.1f} {:.1f}) x({:.2f} {:.2f} {:.2f}) y({:.2f} {:.2f} {:.2f}) "
                    "z({:.2f} {:.2f} {:.2f}); ",
                    bones[i], m[3].x, m[3].y, m[3].z, m[0].x, m[0].y, m[0].z, m[1].x, m[1].y, m[1].z, m[2].x, m[2].y,
                    m[2].z);
            }
            Log("BONEAXES " + text);
        }
        else if (name == "posecheck")
        {
            // posecheck <actor|all> [tolerance] | posecheck <actor|all> <distance> later <fraction>: logs how far the
            // decoded animation data puts actors' spines, hands, guns and magazines from where the game draws them (see
            // Actors::CheckDecodedPose); PASS for an actor that shows one clip alone with each within the tolerance
            // (0.1 units). Decoded a fraction of the animation later, PASS when one is at least the distance off
            // instead, which shows the check sees the bones move.
            std::string which, mode;
            float tolerance = 0.1f, later = 0.0f;
            args >> which >> tolerance >> mode >> later;
            const bool moved = mode == "later";
            std::vector<std::uint32_t> ids;
            if (which == "all")
                for (const auto& actor : Components::Actors::GetActors())
                    ids.push_back(actor.id);
            else
                ids.push_back(ParseActorId(which));
            if (ids.empty())
                Log("FAIL posecheck: no actors");
            for (const auto id : ids)
            {
                std::optional<Actors::DecodedPoseCheck> check;
                RunOnMainThread([&] { check = Actors::CheckDecodedPose(id, moved ? later : 0.0f); });
                if (!check || !check->error.empty())
                {
                    Log(std::format("FAIL posecheck {}: {}", id, check ? check->error : "the actor isn't built"));
                    continue;
                }
                bool complete = check->weight > 0.999f && !check->bones.empty();
                float largest = 0.0f;
                std::string text;
                for (const auto& bone : check->bones)
                {
                    complete &= bone.distance.has_value();
                    largest = std::max(largest, bone.distance.value_or(FLT_MAX));
                    text += bone.distance ? std::format(" {} {:.3f} ({:.3f})", bone.name, *bone.distance, bone.origins)
                                          : std::format(" {} missing", bone.name);
                }
                if (check->rigToGame)
                {
                    largest = std::max(largest, *check->rigToGame);
                    text += std::format(", rig {:.3f} from the game's", *check->rigToGame);
                }
                const bool passed = complete && (moved ? largest >= tolerance : largest <= tolerance);
                Log(std::format("{} posecheck {} {} at {:.3f}{} (weight {:.2f}, spine {}):{} (largest {:.3f}, {} {})",
                                passed ? "PASS" : "FAIL", id, check->animation, check->time,
                                moved ? std::format(" decoded at +{}", later) : std::string(), check->weight,
                                check->spine, text, largest, moved ? "at least" : "at most", tolerance));
            }
        }
        else if (name == "profilecheck")
        {
            // profilecheck <actor|all> [tolerance] | profilecheck <actor|all> <distance> later <ticks>: logs the reload
            // profile (Components::ReloadProfiles) of each actor's first clip, waiting until it is worked out, seeks to
            // its first reload's out and in ticks (freeze and pause first) and compares where the game has the gun's
            // magazine there with where the profile puts it: in the hand with its grip, or where it starts to fall from
            // the gun; and the magazine drawn (in the hand or falling at out and a tick before in, on the gun at in)
            // with where the profile puts it. PASS for an actor with both within the tolerance (0.1 units) and the
            // drawn ones within 0.01. With later, that many ticks after out and before in, PASS when each grip is at
            // least the distance off instead, which shows the check sees the hand move.
            std::string which, mode;
            float tolerance = 0.1f;
            std::uint32_t later = 0;
            args >> which >> tolerance >> mode >> later;
            const bool moved = mode == "later";
            std::vector<std::uint32_t> ids;
            if (which == "all")
                for (const auto& actor : Components::Actors::GetActors())
                    ids.push_back(actor.id);
            else
                ids.push_back(ParseActorId(which));
            if (ids.empty())
                Log("FAIL profilecheck: no actors");
            for (const auto id : ids)
            {
                const auto check = CheckProfile(id, tolerance, moved ? std::optional(later) : std::nullopt);
                if (check.profile)
                    Log(std::format("PROFILE {} on {} with {}: {}", check.animation, check.body, check.weapon,
                                    DescribeProfile(*check.profile)));
                if (!check.error.empty())
                {
                    Log(std::format("FAIL profilecheck {}: {}", id, check.error));
                    continue;
                }
                Log(std::format("{} profilecheck {} {}:{} ({} {})", check.passed ? "PASS" : "FAIL", id, check.animation,
                                check.text, moved ? "each at least" : "each at most", tolerance));
            }
        }
        else if (name == "profiles")
        {
            // profiles [reset | worked <count>]: logs how many reload profiles were worked out (since the last reset)
            // and their generation; with worked, PASS when that many were
            static std::uint32_t baseline = 0;
            std::string mode;
            std::uint32_t expected = 0;
            args >> mode >> expected;
            if (mode == "reset")
                baseline = Components::ReloadProfiles::CountWorkedOut();
            const auto worked = Components::ReloadProfiles::CountWorkedOut() - baseline;
            const auto text = std::format("profiles: {} worked out, generation {}", worked,
                                          Components::ReloadProfiles::GetGeneration());
            if (mode == "worked")
                Log(std::format("{} {} (expected {})", worked == expected ? "PASS" : "FAIL", text, expected));
            else
                Log("PROFILES " + text);
        }
        else if (name == "reloadprofile")
        {
            // reloadprofile <actor> [expect kind <kind> | expect hand <hand> | expect <out|swap|in> <time|none>
            // [<source|-> [<tolerance>]]]: logs the reload profile of an actor's first clip as its override is now
            // (waiting up to 5 s for it); with an expectation, PASS when it says that (sources as profilecheck logs
            // them, with _ for spaces: clip_notetracks)
            std::string id, mode, what, value, source;
            float tolerance = 0.005f;
            args >> id >> mode >> what >> value >> source >> tolerance;
            const auto actor = Components::Actors::GetActor(ParseActorId(id));
            if (!actor || actor->clips.empty())
            {
                Log("FAIL reloadprofile: no such actor, or it has no clips");
                return;
            }
            const auto body = actor->models.empty() ? std::string() : actor->models.front();
            const auto weapon = Components::Actors::ResolveWeapon(*actor);
            const auto& animation = actor->clips.front().animation;
            const auto profile = WaitForProfile(animation, body, weapon);
            if (!profile)
            {
                Log(std::format("FAIL reloadprofile: the profile of {} wasn't worked out in 5 s", animation));
                return;
            }
            const auto text = std::format("reloadprofile {}: {}", animation, DescribeProfile(*profile));
            if (mode != "expect")
            {
                Log("RELOADPROFILE " + text);
                return;
            }
            bool passed = false;
            std::ranges::replace(source, '_', ' ');
            if (what == "kind")
                passed = ReloadLogic::GetKindName(profile->kind) == value;
            else if (what == "hand")
                passed = ReloadLogic::GetHandName(profile->hand) == value;
            else if (what == "out" || what == "swap" || what == "in")
            {
                const auto& moment = what == "out" ? profile->out : what == "swap" ? profile->swap : profile->in;
                char* end = nullptr;
                const auto time = std::strtof(value.c_str(), &end);
                passed =
                    value == "none"
                        ? !moment
                        : moment && end != value.c_str() && std::abs(moment->time - time) <= tolerance &&
                              (source.empty() || source == "-" || ReloadLogic::GetSourceName(moment->source) == source);
            }
            Log(std::format("{} {} (expected {} {}{})", passed ? "PASS" : "FAIL", text, what, value,
                            source.empty() ? std::string() : " " + source));
        }
        else if (name == "reloadoverride")
        {
            // reloadoverride <animation> [set <json> | clear | expect none | expect <json> [<tolerance>]]: logs an
            // animation's reload override as it is used and as config.json has it; set and clear change it as the
            // Animation page does (a step to undo); expect compares what config.json has, its moments within the
            // tolerance (0.005)
            std::string animation, mode, rest;
            args >> animation >> mode;
            std::getline(args >> std::ws, rest);
            std::optional<ReloadLogic::ProfileEntry> expected;
            float tolerance = 0.005f;
            try
            {
                const auto close = rest.rfind('}');
                if ((mode == "set" || mode == "expect") && close != std::string::npos)
                {
                    expected = ReloadLogic::EntryFromJson(nlohmann::json::parse(rest.substr(0, close + 1)));
                    std::istringstream(rest.substr(close + 1)) >> tolerance;
                }
            }
            catch (const std::exception& e)
            {
                Log(std::format("FAIL reloadoverride: {}", e.what()));
                return;
            }
            std::optional<ReloadLogic::ProfileEntry> used, saved;
            RunOnRenderThread([&] {
                if (mode == "set" || mode == "clear")
                    Components::ReloadProfiles::ChangeOverride(animation, mode == "set" ? expected : std::nullopt,
                                                               std::format("{} reload of {}", mode, animation));
                used = Components::ReloadProfiles::GetOverride(animation);
                const auto& json = PreferencesConfiguration::Get().reloadProfiles;
                if (const auto it = json.find(animation); it != json.end())
                    saved = ReloadLogic::EntryFromJson(*it);
            });
            const auto text = std::format("reloadoverride {}: used {}, saved {}", animation, DescribeOverride(used),
                                          DescribeOverride(saved));
            if (mode != "expect")
            {
                Log("RELOADOVERRIDE " + text);
                return;
            }
            const auto same = [&](std::optional<float> a, std::optional<float> b) {
                return a.has_value() == b.has_value() && (!a || std::abs(*a - *b) <= tolerance);
            };
            const bool passed = rest == "none" ? !saved
                                               : saved && expected && saved->kind == expected->kind &&
                                                     saved->hand == expected->hand && same(saved->out, expected->out) &&
                                                     same(saved->swap, expected->swap) && same(saved->in, expected->in);
            Log(std::format("{} {} (expected {})", passed ? "PASS" : "FAIL", text, rest));
        }
        else if (name == "reloadstrip")
        {
            // reloadstrip [scroll [control] | click <control> [right] | drag <out|swap|in> <time> | expect <control>
            // [absent]]: the reload controls of the Actors tab's Animation page (UI::ReloadStrip) as they were drawn
            // last: logs them; scrolls the Actors window so the strip (or a control) shows; clicks the middle of one
            // ("strip", a marker, "swap.playhead", "swap.jump", "reset", "magazine", "magazine.off", "hand",
            // "hand.left"...); drags a marker to a time of the play the strip shows (0 to 1); PASS when one is drawn
            // (or not). Clicks and drags scroll the window first where the control isn't in view.
            std::string mode, control, extra;
            args >> mode >> control >> extra;
            const auto controls = [] {
                std::vector<std::pair<std::string, UI::ReloadControl>> drawn;
                RunOnRenderThread([&] { drawn = UI::GetReloadControls(); });
                return drawn;
            };
            const auto find = [&](const std::string& wanted) -> std::optional<UI::ReloadControl> {
                for (const auto& [each, drawn] : controls())
                    if (each == wanted)
                        return drawn;
                return std::nullopt;
            };
            // scrolls the Actors window so a control is in it, and gives where it is then (the items of a menu or combo
            // are where they popped up)
            const auto show = [&](const std::string& wanted) -> std::optional<UI::ReloadControl> {
                auto found = find(wanted);
                if (!found || wanted.starts_with("menu.") || wanted.starts_with("magazine.") ||
                    wanted.starts_with("hand."))
                    return found;
                bool scrolled = false;
                RunOnRenderThread([&] {
                    auto window = ImGui::FindWindowByName("Actors");
                    if (!window)
                        return;
                    const auto& clip = window->InnerClipRect;
                    const auto margin = 8.0f;
                    float by = 0.0f;
                    if (found->max.y + margin > clip.Max.y)
                        by = found->max.y + margin - clip.Max.y;
                    else if (found->min.y - margin < clip.Min.y)
                        by = found->min.y - margin - clip.Min.y;
                    if (by != 0.0f)
                    {
                        ImGui::SetScrollY(window, window->Scroll.y + by);
                        scrolled = true;
                    }
                });
                if (scrolled)
                {
                    Sleep(300);
                    found = find(wanted);
                }
                return found;
            };
            const auto middle = [](const UI::ReloadControl& drawn) {
                return POINT{static_cast<LONG>(std::lround((drawn.min.x + drawn.max.x) / 2)),
                             static_cast<LONG>(std::lround((drawn.min.y + drawn.max.y) / 2))};
            };

            if (mode.empty() || mode == "scroll")
            {
                if (mode == "scroll" && !show(control.empty() ? "strip" : control))
                {
                    Log(std::format("FAIL reloadstrip: no {} drawn", control.empty() ? "strip" : control));
                    return;
                }
                for (const auto& [each, drawn] : controls())
                    Log(std::format("RELOADSTRIP {} {:.0f} {:.0f} {:.0f} {:.0f}", each, drawn.min.x, drawn.min.y,
                                    drawn.max.x, drawn.max.y));
            }
            else if (mode == "expect")
            {
                const bool drawn = find(control).has_value();
                const bool passed = extra == "absent" ? !drawn : drawn;
                Log(std::format("{} reloadstrip {} {}", passed ? "PASS" : "FAIL", control,
                                drawn ? "drawn" : "not drawn"));
            }
            else if (mode == "click")
            {
                const auto drawn = show(control);
                if (!drawn)
                {
                    Log(std::format("FAIL reloadstrip: no {} drawn", control));
                    return;
                }
                const auto at = middle(*drawn);
                const bool right = extra == "right";
                PostMouse(WM_MOUSEMOVE, at.x, at.y);
                PostMouse(right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, at.x, at.y, right ? MK_RBUTTON : MK_LBUTTON);
                PostMouse(right ? WM_RBUTTONUP : WM_LBUTTONUP, at.x, at.y);
                Log(std::format("RELOADSTRIP clicked {} at {} {}", control, at.x, at.y));
            }
            else if (mode == "drag")
            {
                show("strip");
                const auto marker = find(control);
                const auto strip = find("strip");
                if (!marker || !strip)
                {
                    Log(std::format("FAIL reloadstrip: no {} on a strip", control));
                    return;
                }
                const auto from = middle(*marker);
                const auto time = std::clamp(std::strtof(extra.c_str(), nullptr), 0.0f, 1.0f);
                const auto toX = static_cast<int>(std::lround(strip->min.x + time * (strip->max.x - strip->min.x)));
                PostMouse(WM_MOUSEMOVE, from.x, from.y);
                PostMouse(WM_LBUTTONDOWN, from.x, from.y, MK_LBUTTON);
                for (int i = 1; i <= 10; i++)
                    PostMouse(WM_MOUSEMOVE, from.x + (toX - from.x) * i / 10, from.y, MK_LBUTTON);
                PostMouse(WM_LBUTTONUP, toX, from.y);
                Log(std::format("RELOADSTRIP dragged {} from {} to {} ({:.3f})", control, from.x, toX, time));
            }
        }
        else if (name == "reloadsweep")
        {
            // reloadsweep <animation> <model>[+<model>...] <weapon[:attachment,...]|-> [frames 0|1] | reloadsweep spot
            // <x y z yaw>: puts an actor of the models with the gun at the spot (1700 300 -64 180 by default) playing
            // the animation once from the frozen tick (freeze and pause first; the free camera), and logs a SWEEP line
            // of json: its reload profile, profilecheck's distances, and four frames of it (out, the old one held or
            // the hand fetching, the new one held, a tick before in) grabbed into shots\sweep-<animation>-<n>.png with
            // the camera set to look at its hands; then PASS or FAIL as profilecheck when the reload is drawn. For the
            // reload sweep (reload-sweep.ps1); an animation that isn't loaded
            // logs "loaded": false.
            std::string animation, models, weapon;
            args >> animation;
            if (animation == "spot")
            {
                args >> sweepSpot.x >> sweepSpot.y >> sweepSpot.z >> sweepSpot.w;
                Log(std::format("SWEEP spot {:.1f} {:.1f} {:.1f} yaw {:.1f}", sweepSpot.x, sweepSpot.y, sweepSpot.z,
                                sweepSpot.w));
                return;
            }
            int frames = 1;
            args >> models >> weapon >> frames;
            SweepReload(animation, models, weapon, frames != 0);
        }
        else if (name == "grab")
        {
            // grab <label> [width]: saves the game's frame (the client area, without the overlay) scaled to the width
            // (640) as shots\<label>.png, taken by the game itself at once
            std::string label;
            UINT width = 640;
            args >> label >> width;
            const bool saved = GrabFrame(directory / "shots" / (label + ".png"), width);
            Log(std::format("GRAB {} {}", label, saved ? "saved" : "failed"));
        }
        else if (name == "notetracks")
        {
            // notetracks <animation> | notetracks find <notetrack> [max]: logs an animation's notetracks, from 0 to 1,
            // as the actors' catalog has them, or the animations that have a notetrack
            std::string animation;
            args >> animation;
            const auto catalog = Components::Actors::GetCatalog();
            if (animation == "find")
            {
                std::string notetrack;
                std::size_t max = 40;
                args >> notetrack >> max;
                std::string found;
                std::size_t count = 0;
                for (const auto& entry : catalog ? catalog->animations : decltype(catalog->animations){})
                {
                    if (std::ranges::find(entry.notetracks, notetrack, &Types::ActorNotetrack::name) ==
                        entry.notetracks.end())
                        continue;
                    if (count++ < max)
                        found += entry.name + " ";
                }
                Log(std::format("NOTETRACKS '{}' in {} animations: {}", notetrack, count, found));
                return;
            }
            const auto found = catalog ? catalog->FindAnimation(animation) : nullptr;
            std::string text;
            if (found)
                for (const auto& notetrack : found->notetracks)
                    text += std::format("'{}'@{:.3f} ", notetrack.name, notetrack.time);
            Log(std::format("NOTETRACKS {} {}: {}", animation, found ? std::format("{:.2f} s", found->duration)
                                                                    : std::string("not loaded"),
                            text));
        }
        else if (name == "reloadnotetracks")
        {
            // reloadnotetracks <file>: writes the duration and notetracks of each animation of the actors' catalog
            // with "reload" in its name into a file of the harness folder, for the unit tests of reload profiles
            std::string file;
            args >> file;
            const auto catalog = Components::Actors::GetCatalog();
            auto animations = nlohmann::json::object();
            for (const auto& entry : catalog ? catalog->animations : decltype(catalog->animations){})
            {
                if (entry.name.find("reload") == std::string::npos)
                    continue;
                auto notetracks = nlohmann::json::array();
                for (const auto& notetrack : entry.notetracks)
                    notetracks.push_back({notetrack.name, std::round(notetrack.time * 1e5f) / 1e5f});
                animations[entry.name] = {{"duration", std::round(entry.duration * 1e4f) / 1e4f},
                                          {"notetracks", notetracks}};
            }
            std::ofstream(directory / file) << animations.dump(1);
            Log(std::format("RELOADNOTETRACKS {} animations into {}", animations.size(), file));
        }
        else if (name == "reloaddata")
        {
            // reloaddata <animation> <body> <weapon variant> <file> [samples]: writes where the bones a reload profile
            // needs are (the wrists, tag_inhand and tag_weapon_right, in the model, decoded) at evenly spaced times and
            // at the notetracks, the gun's rig and the notetracks into a file of the harness folder, for the unit tests
            std::string animation, body, weapon, file;
            std::size_t samples = 121;
            args >> animation >> body >> weapon >> file >> samples;
            const auto& game = Mod::GetGameInterface();
            const auto catalog = Components::Actors::GetCatalog();
            const auto entry = catalog ? catalog->FindAnimation(animation) : nullptr;
            std::vector<float> times;
            for (std::size_t i = 0; i < samples; ++i)
                times.push_back(static_cast<float>(i) / static_cast<float>(samples - 1));
            auto notetracks = nlohmann::json::array();
            for (const auto& notetrack : entry ? entry->notetracks : std::vector<Types::ActorNotetrack>{})
            {
                notetracks.push_back({notetrack.name, notetrack.time});
                times.push_back(notetrack.time);
            }
            std::ranges::sort(times);
            times.erase(std::unique(times.begin(), times.end()), times.end());

            const auto round = [](float value) { return std::round(value * 1e4f) / 1e4f; };
            const auto transform = [&](const glm::mat4& m) {
                const auto q = glm::normalize(glm::quat_cast(glm::mat3(m)));
                return nlohmann::json::array(
                    {round(m[3].x), round(m[3].y), round(m[3].z), round(q.w), round(q.x), round(q.y), round(q.z)});
            };
            auto poses = nlohmann::json::array();
            std::size_t decoded = 0;
            for (const auto time : times)
            {
                const auto pose = game->GetAnimationPoseByName(animation, body, time);
                if (!pose)
                    continue;
                auto bones = nlohmann::json::object();
                for (const auto bone : {"j_wrist_le", "j_wrist_ri", "tag_inhand", "tag_weapon_right"})
                    if (const auto index = pose->FindBone(bone))
                        bones[bone] = transform(PoseLogic::GetTransform(*pose, *index));
                poses.push_back({{"time", time}, {"bones", bones}});
                ++decoded;
            }
            nlohmann::json root{{"animation", animation},   {"body", body},
                                {"weapon", weapon},         {"duration", entry ? entry->duration : 0.0f},
                                {"notetracks", notetracks}, {"poses", poses}};
            if (const auto rig = game->GetWeaponRig(weapon))
                root["rig"] = {
                    {"model", rig->model}, {"magazineBone", rig->magazineBone}, {"magazine", transform(rig->magazine)}};
            std::ofstream(directory / file) << root.dump();
            Log(std::format("RELOADDATA {} on {} with {}: {} of {} poses decoded{} into {}", animation, body, weapon,
                            decoded, times.size(), root.contains("rig") ? ", rig" : ", no rig", file));
        }
        else if (name == "modelbones")
        {
            // modelbones <xmodel>: logs a loaded model's bones, with their parents and rest positions
            std::string model;
            args >> model;
            std::string text;
            RunOnMainThread([&] {
                const auto xmodel =
                    static_cast<const Structures::XModel*>(ContentPacks::FindAsset(Structures::ASSET_TYPE_XMODEL, model.c_str()));
                if (!xmodel)
                    return;
                for (std::size_t i = 0; i < xmodel->numBones; ++i)
                {
                    const auto boneName = Functions::SL_ConvertToString(xmodel->boneNames[i]);
                    if (i < xmodel->numRootBones)
                        text += std::format("{} (root) ", boneName);
                    else
                    {
                        const auto other = i - xmodel->numRootBones;
                        const auto parent = i - xmodel->parentList[other];
                        text += std::format("{} <{} ({:.1f} {:.1f} {:.1f}) ", boneName,
                                            Functions::SL_ConvertToString(xmodel->boneNames[parent]),
                                            xmodel->trans[other][0], xmodel->trans[other][1], xmodel->trans[other][2]);
                    }
                }
            });
            Log(std::format("MODELBONES {}: {}", model, text.empty() ? "not loaded" : text));
        }
        else if (name == "magazine")
        {
            // magazine <actor> [gun | off | empty | old <distance> | new <distance> | dropped <count> [<height>
            // [<reach>]]]: logs where an actor's magazines are; with an expectation, PASS when the magazine is on the
            // gun, off it, off it with nothing in the hand, in the hand (the old one or a new one) within the distance
            // of the gun's magazine bone, or when that many are on the ground, each at most the height above it and
            // the reach from where the actor stands (along the ground)
            std::string id, expected;
            float limit = FLT_MAX;
            args >> id >> expected >> limit;
            std::optional<Actors::MagazineState> state;
            RunOnMainThread([&] { state = Actors::GetMagazineState(ParseActorId(id)); });
            if (!state)
            {
                Log("FAIL magazine: the actor isn't built");
                return;
            }
            std::string dropped;
            for (std::size_t i = 0; i < state->dropped.size(); ++i)
                dropped += std::format(" {:.2f} ({:.1f} away)", state->dropped[i], state->droppedFromActor[i]);
            const auto text =
                std::format("magazine {} {}, hand {}{}, {} dropped{}, reload {}", state->tag,
                            state->offGun ? "off the gun" : "on the gun", state->held.empty() ? "empty" : state->held,
                            state->heldToGun ? std::format(" {:.2f} from the gun", *state->heldToGun) : std::string(),
                            state->dropped.size(), dropped.empty() ? std::string() : " at" + dropped,
                            state->reload.empty() ? std::string("none") : state->reload);
            if (expected.empty())
            {
                Log("MAGAZINE " + text);
                return;
            }
            const bool held =
                state->offGun && state->held == expected && state->heldToGun && *state->heldToGun <= limit;
            const bool passed = expected == "gun"                        ? !state->offGun && state->held.empty()
                                : expected == "off"                      ? state->offGun
                                : expected == "empty"                    ? state->offGun && state->held.empty()
                                : expected == "old" || expected == "new" ? held
                                : expected == "dropped"
                                    ? std::abs(static_cast<float>(state->dropped.size()) - limit) < 0.5f
                                    : false;
            float height = FLT_MAX, reach = FLT_MAX;
            if (expected == "dropped")
                args >> height >> reach;
            const bool low = std::ranges::all_of(state->dropped, [&](float h) { return h <= height; }) &&
                             std::ranges::all_of(state->droppedFromActor, [&](float d) { return d <= reach; });
            Log(std::format("{} {} (expected {}{}{})", passed && low ? "PASS" : "FAIL", text, expected,
                            limit == FLT_MAX ? std::string() : std::format(" {}", limit),
                            height == FLT_MAX  ? std::string()
                            : reach == FLT_MAX ? std::format(" {}", height)
                                               : std::format(" {} {}", height, reach)));
        }
        else if (name == "played")
        {
            // played [reset] | played <shots|sounds> <min> <max>: logs the shots and sounds actors played from
            // notetracks since the last reset; with a range, PASS when the count is in it
            std::string what;
            args >> what;
            Actors::NotetrackCounts counts;
            RunOnMainThread([&] { counts = Actors::GetNotetrackCounts(what == "reset"); });
            std::size_t low = 0, high = 0;
            if ((what == "shots" || what == "sounds") && args >> low >> high)
            {
                const auto count = what == "shots" ? counts.shots : counts.sounds;
                Log(std::format("{} played {} {} (from {} to {})", count >= low && count <= high ? "PASS" : "FAIL",
                                count, what, low, high));
            }
            else
                Log(std::format("PLAYED shots {} sounds {}", counts.shots, counts.sounds));
        }
        else if (name == "marks")
        {
            // marks [max | min <n>]: logs how many impact marks (decals) there are, about; with max, PASS when there
            // are at most that many, with min, when there are at least n
            std::size_t used = 0;
            RunOnMainThread([&] { used = Functions::FX_CountMarkTriGroups(0); });
            std::string limit;
            args >> limit;
            std::size_t n = 0;
            if (limit == "min" && args >> n)
                Log(std::format("{} marks {} (at least {})", used >= n ? "PASS" : "FAIL", used, n));
            else if (!limit.empty())
                Log(std::format("{} marks {} (at most {})", used <= std::stoul(limit) ? "PASS" : "FAIL", used, limit));
            else
                Log(std::format("MARKS {}", used));
        }
        else if (name == "scriptmemory")
        {
            // logs how many damaged links of the script memory tree the guard cut (Hooks/ScriptMemory.cpp), and how
            // many entities' local client the entity guard put right (Hooks/EntityClients.cpp)
            Log(std::format("SCRIPTMEMORY repairs {} entity clients {}", Hooks::ScriptMemory::GetRepairCount(),
                            Hooks::EntityClients::GetFixCount()));
        }
        else if (name == "mark")
        {
            std::string label;
            args >> label;
            Log("MARK " + label);
        }
        else if (name == "shot")
        {
            // waits until the scripts took the screenshot, so the game doesn't change before it is taken
            std::string label;
            args >> label;
            const auto done = directory / "shots" / (label + ".png");
            std::filesystem::remove(done);
            // the render thread draws a frame behind, so a change made just before is given time to show
            Sleep(50);
            Log("SHOT " + label);
            for (int i = 0; i < 500 && !std::filesystem::exists(done); i++)
                Sleep(10);
        }
        else if (name == "overlay")
        {
            // overlay: shows or hides the overlay; overlay expect <shown|hidden>: PASS when it is
            std::string action, expected;
            args >> action >> expected;
            if (action == "expect")
            {
                const auto shown = UI::UIManager::Get().IsOverlayHidden() ? "hidden" : "shown";
                Log(std::format("{} overlay {} (expected {})", shown == expected ? "PASS" : "FAIL", shown, expected));
            }
            else
                UI::UIManager::Get().ToggleOverlay();
        }
        else if (name == "client")
        {
            // logs where the game window's client area is on the screen, for turning screenshots into coordinates
            RECT rect{};
            GetClientRect(GetGameWindow(), &rect);
            POINT origin{};
            ClientToScreen(GetGameWindow(), &origin);
            Log(std::format("CLIENT {} {} {} {}", origin.x, origin.y, rect.right, rect.bottom));
        }
        else if (name == "click" || name == "dclick" || name == "rclick")
        {
            // click|dclick|rclick <x> <y>, in the game window's client coordinates, or <window>, in its middle
            const auto point = ParseMousePoint(args);
            if (!point)
            {
                Log(std::format("FAIL {}: no such window", name));
                return;
            }
            const int x = point->x, y = point->y;
            const bool right = name == "rclick";
            PostMouse(WM_MOUSEMOVE, x, y);
            for (int i = 0; i < (name == "dclick" ? 2 : 1); i++)
            {
                PostMouse(right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, x, y, right ? MK_RBUTTON : MK_LBUTTON);
                PostMouse(right ? WM_RBUTTONUP : WM_LBUTTONUP, x, y);
            }
        }
        else if (name == "moveat" || name == "clickat" || name == "dragat")
        {
            // moveat|clickat <x y z>, dragat <x y z> <x y z>: the mouse commands at where world points are in the view
            auto project = [&] {
                glm::vec3 point{};
                args >> point.x >> point.y >> point.z;
                std::optional<glm::vec2> screen;
                RunOnRenderThread([&] { screen = GFX::WorldToScreen(point); });
                return screen ? glm::ivec2(glm::round(*screen)) : glm::ivec2(-1);
            };
            const auto from = project();
            if (name == "moveat")
                PostMouse(WM_MOUSEMOVE, from.x, from.y);
            else if (name == "clickat")
            {
                PostMouse(WM_MOUSEMOVE, from.x, from.y);
                PostMouse(WM_LBUTTONDOWN, from.x, from.y, MK_LBUTTON);
                PostMouse(WM_LBUTTONUP, from.x, from.y);
            }
            else
            {
                const auto to = project();
                PostMouse(WM_MOUSEMOVE, from.x, from.y);
                PostMouse(WM_LBUTTONDOWN, from.x, from.y, MK_LBUTTON);
                for (int i = 1; i <= 10; i++)
                    PostMouse(WM_MOUSEMOVE, from.x + (to.x - from.x) * i / 10, from.y + (to.y - from.y) * i / 10,
                              MK_LBUTTON);
                PostMouse(WM_LBUTTONUP, to.x, to.y);
            }
        }
        else if (name == "move")
        {
            // move <x> <y>: moves the mouse there without a button, in client coordinates
            int x = 0, y = 0;
            args >> x >> y;
            PostMouse(WM_MOUSEMOVE, x, y);
        }
        else if (name == "drag")
        {
            // drag <x0> <y0> <x1> <y1>, with the left button, in steps
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            args >> x0 >> y0 >> x1 >> y1;
            PostMouse(WM_MOUSEMOVE, x0, y0);
            PostMouse(WM_LBUTTONDOWN, x0, y0, MK_LBUTTON);
            for (int i = 1; i <= 10; i++)
                PostMouse(WM_MOUSEMOVE, x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10, MK_LBUTTON);
            PostMouse(WM_LBUTTONUP, x1, y1);
        }
        else if (name == "wheel")
        {
            // wheel <x> <y> <notches> or wheel <window> <notches> (in its middle), negative scrolls down
            const auto point = ParseMousePoint(args);
            int notches = 0;
            args >> notches;
            if (!point)
            {
                Log("FAIL wheel: no such window");
                return;
            }
            const int x = point->x, y = point->y;
            PostMouse(WM_MOUSEMOVE, x, y);
            PostInput(WM_MOUSEWHEEL, MAKEWPARAM(0, notches * WHEEL_DELTA), MAKELPARAM(x, y));
        }
        else if (name == "post")
        {
            // post key <name> [down|up|repeat]: the window messages of a real key press (and release, without down or
            // up; repeat is a press of a key already down), with its scan code, which reach the overlay and the game
            // as typed keys do (modifiers can't be held this way: Windows doesn't count posted keys as down). Names:
            // a-z, 0-9, f1-f12, escape, enter, tab, space, console (the key below Escape, by its scan code).
            // post char <c>: a typed character
            std::string what, key, how;
            args >> what >> key >> how;
            if (what == "char" && !key.empty())
                PostInput(WM_CHAR, static_cast<unsigned char>(key[0]), 1);
            else if (what == "key")
            {
                static const std::map<std::string, UINT> named = {{"escape", VK_ESCAPE},
                                                                  {"enter", VK_RETURN},
                                                                  {"tab", VK_TAB},
                                                                  {"space", VK_SPACE},
                                                                  {"console", VK_OEM_3}};
                UINT vk = 0;
                if (const auto it = named.find(key); it != named.end())
                    vk = it->second;
                else if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0])))
                    vk = std::toupper(static_cast<unsigned char>(key[0]));
                else if (key.size() >= 2 && key[0] == 'f' && std::isdigit(static_cast<unsigned char>(key[1])))
                    vk = VK_F1 + std::stoi(key.substr(1)) - 1;
                // (the game tells its console key by the scan code)
                const UINT scan = key == "console" ? 0x29 : MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                const LPARAM press = 1 | (scan << 16) | (how == "repeat" ? 1 << 30 : 0);
                const LPARAM release = 1 | (scan << 16) | (1u << 30) | (1u << 31);
                if (!vk)
                    Log(std::format("no such key: {}", key));
                else
                {
                    if (how != "up")
                        PostInput(WM_KEYDOWN, vk, press);
                    if (how.empty() || how == "up")
                        PostInput(WM_KEYUP, vk, release);
                }
            }
            else
                Log("post key <name> [down|up|repeat] | post char <c>");
        }
        else if (name == "gamekeys")
        {
            // gamekeys: logs the presses and releases of keys CL_KeyEvent was given since the reset (the game runs
            // binds there); gamekeys reset; gamekeys expect <key> <presses> [releases]: PASS when it was given that
            // many
            // (* for any); gamekeys expect none: PASS when it was given no press. Keys are a character, tab, enter,
            // escape, space, console, mouse1-5, wheelup, wheeldown or #<number>
            std::string action, key, presses, releases;
            args >> action >> key >> presses >> releases;
            if (action == "reset")
            {
                for (auto& count : gameKeyPresses)
                    count = 0;
                for (auto& count : gameKeyReleases)
                    count = 0;
                Log("GAMEKEYS reset");
            }
            else if (action == "expect" && key == "none")
            {
                std::string pressed;
                for (std::size_t i = 0; i < gameKeyPresses.size(); i++)
                {
                    if (gameKeyPresses[i])
                        pressed += std::format(" #{}:{}", i, gameKeyPresses[i].load());
                }
                Log(std::format("{} gamekeys pressed:{} (expected none)", pressed.empty() ? "PASS" : "FAIL",
                                pressed.empty() ? " none" : pressed));
            }
            else if (action == "expect")
            {
                const auto code = ParseGameKey(key);
                if (!code || *code < 0 || *code >= static_cast<int>(gameKeyPresses.size()))
                    Log(std::format("FAIL gamekeys no such key: {}", key));
                else
                {
                    const auto pressed = gameKeyPresses[*code].load();
                    const auto released = gameKeyReleases[*code].load();
                    const auto matches = [](const std::string& expected, std::uint32_t count) {
                        return expected.empty() || expected == "*" || std::to_string(count) == expected;
                    };
                    Log(std::format("{} gamekeys {} pressed {} released {} (expected {} {})",
                                    matches(presses, pressed) && matches(releases, released) ? "PASS" : "FAIL", key,
                                    pressed, released, presses, releases.empty() ? "*" : releases));
                }
            }
            else
            {
                std::string text;
                for (std::size_t i = 0; i < gameKeyPresses.size(); i++)
                {
                    if (gameKeyPresses[i] || gameKeyReleases[i])
                        text += std::format(" #{}:{}/{}", i, gameKeyPresses[i].load(), gameKeyReleases[i].load());
                }
                Log(std::format("GAMEKEYS{}", text.empty() ? " none" : text));
            }
        }
        else if (name == "catchers")
        {
            // catchers: logs the game's key catchers, what takes keys instead of the binds (1 the console, 0x10 a
            // menu, 0x20 the chat field...); catchers expect <hex mask> <set|clear>: PASS when one of those bits is
            // set, or none
            std::string action, mask, expected;
            args >> action >> mask >> expected;
            const auto catchers = *Structures::GetKeyCatchers();
            if (action == "expect")
            {
                const auto bits = static_cast<std::int32_t>(std::stoul(mask, nullptr, 16));
                const bool set = (catchers & bits) != 0;
                Log(std::format("{} catchers {:X} ({} {} expected)", set == (expected == "set") ? "PASS" : "FAIL",
                                catchers, mask, expected));
            }
            else
                Log(std::format("CATCHERS {:X}", catchers));
        }
        else if (name == "zones")
        {
            // zones: logs the loaded zones and the flags they were loaded with (ContentPacks loads single player
            // "common" with 0x10000000 and a pack with 0x200000); zones expect <zone> <loaded|absent>: PASS when it is
            std::string action, zone, expected;
            args >> action >> zone >> expected;
            constexpr std::size_t FLAGS = 0x40;
            std::string text;
            bool loaded = false;
            for (std::size_t i = 0; i < PoolGrowth::GetZoneInfoCount(); ++i)
            {
                const auto info = PoolGrowth::GetZoneInfo() + i * Structures::ZONE_INFO_SIZE;
                const std::string_view loadedName(reinterpret_cast<const char*>(info));
                if (loadedName.empty() || loadedName.size() >= FLAGS)
                    continue;
                text += std::format(" {}:{:X}", loadedName, *reinterpret_cast<const std::int32_t*>(info + FLAGS));
                loaded = loaded || _stricmp(loadedName.data(), zone.c_str()) == 0;
            }
            if (action == "expect")
                Log(std::format("{} zones {} {} (expected {})", loaded == (expected == "loaded") ? "PASS" : "FAIL",
                                zone, loaded ? "loaded" : "absent", expected));
            else
                Log("ZONES" + text);
        }
        else if (name == "playdemo")
        {
            // playdemo <file>: plays a demo of the game's demos folder as its PLAY button on the Demo tab does, so the
            // Demo tab shows it with STOP (the control demo.stop); demo <name> in the console doesn't
            // (or the demo at a path of its own)
            std::string file;
            std::getline(args >> std::ws, file);
            const auto homePath = Functions::FindDvar("fs_homepath");
            const auto path = std::filesystem::path(file).is_absolute()
                                  ? std::filesystem::path(file)
                                  : std::filesystem::path(homePath ? homePath->current.string : "") / "demos" / file;
            // (refused while another loads, as its PLAY is)
            bool played = false;
            RunOnRenderThread([&] {
                played = UI::UIManager::Get().GetUIComponent<UI::DemoLoader>(UI::Component::DemoLoader)->Play(path);
            });
            Log(std::format("PLAYDEMO {}{}", path.string(), played ? "" : " refused: a demo is loading"));
        }
        else if (name == "waitstate")
        {
            // waitstate <menu|demo|game> [ms]: waits up to ms (60000) until the game is at the menu, or plays a demo
            // or a game (a map loaded with devmap) whose time goes on; PASS or FAIL
            std::string wanted;
            DWORD timeout = 60000;
            args >> wanted;
            if (!(args >> timeout))
                timeout = 60000;
            const auto start = GetTickCount64();
            bool reached = false;
            for (int lastTime = -1; !reached && GetTickCount64() - start < timeout; Sleep(100))
            {
                const auto state = Mod::GetGameInterface()->GetGameState();
                const auto cg = Structures::GetClientGlobals();
                const int time = cg ? cg->time : 0;
                if (wanted == "menu")
                    reached = state == Types::GameState::MainMenu;
                else
                    reached = state == (wanted == "game" ? Types::GameState::InGame : Types::GameState::InDemo) &&
                              time > 0 && lastTime > 0 && time > lastTime;
                lastTime = time;
            }
            Log(std::format("{} waitstate {} after {} ms (game {})", reached ? "PASS" : "FAIL", wanted,
                            GetTickCount64() - start, magic_enum::enum_name(Mod::GetGameInterface()->GetGameState())));
        }
        else if (name == "type")
        {
            // type <text>: the rest of the line, as typed characters
            std::string text;
            std::getline(args >> std::ws, text);
            for (const char c : text)
                PostInput(WM_CHAR, static_cast<unsigned char>(c), 1);
        }
        else if (name == "key")
        {
            // key <enter|escape|tab|backspace|[ctrl+][shift+][alt+]<delete|a-z> [ms]>
            static const std::map<std::string, WPARAM> keys = {
                {"enter", VK_RETURN}, {"escape", VK_ESCAPE}, {"tab", VK_TAB}, {"backspace", VK_BACK}};
            std::string key;
            args >> key;
            if (const auto it = keys.find(key); it != keys.end())
            {
                PostInput(WM_KEYDOWN, it->second, 1);
                PostInput(WM_KEYUP, it->second, 0xC0000001);
            }
            else
            {
                // key [ctrl+][shift+][alt+]<name> [ms]: straight to ImGui, as posted messages don't hold the
                // modifiers down for the backend, which asks Windows whether they are; held for ms (100 by default).
                // Names: a-z, 0-9, f1-f12, delete, space, slash, comma, period, left, right, up, down, backslash,
                // lbracket, rbracket, home, end, return (Enter for the overlay only: a posted enter reaches the
                // game too)
                const bool ctrl = key.find("ctrl+") != std::string::npos;
                const bool shift = key.find("shift+") != std::string::npos;
                const bool alt = key.find("alt+") != std::string::npos;
                DWORD hold = 100;
                if (!(args >> hold))
                    hold = 100;
                const auto name = key.substr(key.find_last_of('+') + 1);
                static const std::map<std::string, ImGuiKey> named = {
                    {"delete", ImGuiKey_Delete},
                    {"space", ImGuiKey_Space},
                    {"slash", ImGuiKey_Slash},
                    {"comma", ImGuiKey_Comma},
                    {"period", ImGuiKey_Period},
                    {"left", ImGuiKey_LeftArrow},
                    {"right", ImGuiKey_RightArrow},
                    {"up", ImGuiKey_UpArrow},
                    {"down", ImGuiKey_DownArrow},
                    {"backslash", ImGuiKey_Backslash},
                    {"lbracket", ImGuiKey_LeftBracket},
                    {"rbracket", ImGuiKey_RightBracket},
                    {"home", ImGuiKey_Home},
                    {"end", ImGuiKey_End},
                    {"return", ImGuiKey_Enter},
                };
                auto pressed = ImGuiKey_None;
                if (const auto it = named.find(name); it != named.end())
                    pressed = it->second;
                else if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z')
                    pressed = static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'a'));
                else if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
                    pressed = static_cast<ImGuiKey>(ImGuiKey_0 + (name[0] - '0'));
                else if (name.size() >= 2 && name[0] == 'f' && std::isdigit(static_cast<unsigned char>(name[1])))
                    pressed = static_cast<ImGuiKey>(ImGuiKey_F1 + (std::stoi(name.substr(1)) - 1));
                auto send = [&](bool down) {
                    RunOnRenderThread([&] {
                        auto& io = ImGui::GetIO();
                        if (ctrl)
                        {
                            io.AddKeyEvent(ImGuiMod_Ctrl, down);
                            io.AddKeyEvent(ImGuiKey_LeftCtrl, down);
                        }
                        if (shift)
                        {
                            io.AddKeyEvent(ImGuiMod_Shift, down);
                            io.AddKeyEvent(ImGuiKey_LeftShift, down);
                        }
                        if (alt)
                        {
                            io.AddKeyEvent(ImGuiMod_Alt, down);
                            io.AddKeyEvent(ImGuiKey_LeftAlt, down);
                        }
                        io.AddKeyEvent(pressed, down);
                    });
                };
                if (pressed != ImGuiKey_None)
                {
                    send(true);
                    Sleep(hold);
                    send(false);
                }
                else
                    Log(std::format("no such key: {}", key));
            }
        }
        else if (name == "fly")
        {
            // fly [on|off|expect <on|off>]: gives the free camera the keyboard and mouse (as F does) or takes them
            // back, or logs whether it has them; with expect, PASS or FAIL
            std::string what, expected;
            args >> what >> expected;
            bool flying = false;
            RunOnRenderThread([&] {
                auto& gameView = UI::UIManager::Get().GetUIComponent(UI::Component::GameView);
                if (what == "on" || what == "off")
                    gameView->SetHasFocus(what == "on");
                flying = gameView->HasFocus() && UI::UIManager::Get().IsControllableCameraModeSelected();
            });
            if (what == "expect")
                Log(std::format("{} flying {} (expected {})", flying == (expected == "on") ? "PASS" : "FAIL",
                                flying ? "on" : "off", expected));
            else
                Log(std::format("FLY {}", flying ? "on" : "off"));
        }
        else if (name == "rdown" || name == "rup")
        {
            // rdown <x> <y>, rup <x> <y>: presses or lets go of the right mouse button there, in client coordinates
            int x = 0, y = 0;
            args >> x >> y;
            PostMouse(WM_MOUSEMOVE, x, y, name == "rup" ? MK_RBUTTON : 0);
            PostMouse(name == "rdown" ? WM_RBUTTONDOWN : WM_RBUTTONUP, x, y, name == "rdown" ? MK_RBUTTON : 0);
        }
        else if (name == "ldown" || name == "lup" || name == "lmove")
        {
            // ldown <x> <y>, lmove <x> <y>, lup <x> <y>: presses the left mouse button there, moves it there held (in
            // steps from where it was), or lets go of it there, in client coordinates
            int x = 0, y = 0;
            args >> x >> y;
            if (name == "lmove")
            {
                POINT from{};
                GetCursorPos(&from);
                ScreenToClient(GetGameWindow(), &from);
                for (int i = 1; i <= 10; i++)
                    PostMouse(WM_MOUSEMOVE, from.x + (x - from.x) * i / 10, from.y + (y - from.y) * i / 10, MK_LBUTTON);
            }
            else
            {
                PostMouse(WM_MOUSEMOVE, x, y, name == "lup" ? MK_LBUTTON : 0);
                PostMouse(name == "ldown" ? WM_LBUTTONDOWN : WM_LBUTTONUP, x, y, name == "ldown" ? MK_LBUTTON : 0);
            }
        }
        else if (name == "viewmenu")
        {
            // viewmenu <x> <y> | viewmenu at <x y z>: right-clicks the view there (client coordinates, or where a
            // world point is) and lets go at once, which opens the view's menu (UI::ViewMenu); viewmenu: logs what it
            // is open for; viewmenu expect <closed|ground|actor|point|line> [id]: PASS when it is (a point or line of
            // the selected actor's path, by its index)
            std::string first;
            args >> first;
            const auto describe = [] {
                UI::ViewMenu::State state;
                RunOnRenderThread([&] { state = UI::ViewMenu::GetState(); });
                return !state.open   ? std::string("closed")
                       : state.point ? std::format("point {}", *state.point)
                       : state.line  ? std::format("line {}", *state.line)
                       : state.actor ? std::format("actor {}", *state.actor)
                                     : std::string("ground");
            };
            if (first.empty() || first == "expect")
            {
                std::string what, id;
                args >> what >> id;
                const auto shown = describe();
                if (first.empty())
                    Log("VIEWMENU " + shown);
                else
                {
                    const auto expected = what == "actor" && !id.empty() ? std::format("actor {}", ParseActorId(id))
                                          : (what == "point" || what == "line") && !id.empty() ? what + " " + id
                                                                                               : what;
                    const bool pass = id.empty() && what != "ground" && what != "closed" ? shown.starts_with(what)
                                                                                         : shown == expected;
                    Log(std::format("{} viewmenu {} (expected {})", pass ? "PASS" : "FAIL", shown, expected));
                }
                return;
            }
            int x = 0, y = 0;
            if (first == "at")
            {
                glm::vec3 point{};
                args >> point.x >> point.y >> point.z;
                std::optional<glm::vec2> screen;
                RunOnRenderThread([&] { screen = GFX::WorldToScreen(point); });
                if (!screen)
                {
                    Log("FAIL viewmenu: the point is behind the camera");
                    return;
                }
                x = static_cast<int>(std::lround(screen->x));
                y = static_cast<int>(std::lround(screen->y));
            }
            else
            {
                x = std::stoi(first);
                args >> y;
            }
            PostMouse(WM_MOUSEMOVE, x, y);
            Sleep(60);
            PostMouse(WM_RBUTTONDOWN, x, y, MK_RBUTTON);
            Sleep(60);
            PostMouse(WM_RBUTTONUP, x, y);
            Log(std::format("VIEWMENU right-clicked {} {}", x, y));
        }
        else if (name == "clickstrip")
        {
            // clickstrip [above]: clicks the mode strip along the bottom of the view, left of its middle (where no
            // key or link is), which takes the click itself; with above, the view 50 px above it instead
            std::string where;
            args >> where;
            std::optional<POINT> point;
            RunOnRenderThread([&] {
                const auto gameView = UI::UIManager::Get().GetUIComponent<UI::GameView>(UI::Component::GameView);
                const auto position = gameView->GetViewportPosition(), size = gameView->GetViewportSize();
                point = POINT{static_cast<LONG>(position.x + size.x * 0.45f),
                              static_cast<LONG>(position.y + size.y - (where == "above" ? 56 : 6))};
            });
            PostMouse(WM_MOUSEMOVE, point->x, point->y);
            Sleep(60);
            PostMouse(WM_LBUTTONDOWN, point->x, point->y, MK_LBUTTON);
            PostMouse(WM_LBUTTONUP, point->x, point->y);
        }
        else if (name == "hover")
        {
            // hover <window>: moves the mouse to the middle of an overlay window ("Timeline", "GameView")
            std::string window;
            std::getline(args >> std::ws, window);
            std::optional<ImVec2> middle;
            RunOnRenderThread([&] {
                if (const auto found = ImGui::FindWindowByName(window.c_str()))
                    middle = found->Pos + found->Size * 0.5f;
            });
            if (middle)
                PostMouse(WM_MOUSEMOVE, static_cast<int>(middle->x), static_cast<int>(middle->y));
            else
                Log(std::format("FAIL hover: no window {}", window));
        }
        else if (name == "contexts")
        {
            // contexts: logs the input contexts of the last frame (UI::Commands)
            std::string text;
            RunOnRenderThread([&] {
                const auto active = UI::Commands::GetActiveContexts();
                for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(UI::Commands::Context::Count); ++i)
                {
                    const auto context = static_cast<UI::Commands::Context>(i);
                    if (active & UI::Commands::Bit(context))
                        text += std::string(magic_enum::enum_name(context)) + " ";
                }
            });
            Log("CONTEXTS " + text);
        }
        else if (name == "command")
        {
            // command run <id> | command expect <id> available|unavailable: runs a command as a menu does, or logs
            // PASS when it can (or can't) run now
            std::string action, id, expected;
            args >> action >> id >> expected;
            std::optional<std::string> reason;
            bool ran = false;
            RunOnRenderThread([&] {
                reason = UI::Commands::GetUnavailableReason(id);
                if (action == "run")
                    ran = UI::Commands::Run(id);
            });
            if (action == "expect")
                Log(std::format("{} command {} {} (expected {})",
                                reason.has_value() == (expected == "unavailable") ? "PASS" : "FAIL", id,
                                reason ? "unavailable: " + *reason : std::string("available"), expected));
            else
                Log(std::format("COMMAND {} {}", id, ran ? "ran" : "unavailable: " + reason.value_or("")));
        }
        else if (name == "history")
        {
            // history [undo|redo <label>]: logs what undo and redo would take back; with a label, PASS when theirs
            // has it in it ("none" for nothing)
            std::string which, expected;
            args >> which;
            std::getline(args >> std::ws, expected);
            const auto& history = Components::UndoHistory::Get();
            const auto undo = history.GetUndoLabel();
            const auto redo = history.GetRedoLabel();
            if (which == "undo" || which == "redo")
            {
                const auto label = (which == "undo" ? undo : redo).value_or("none");
                const bool passed = expected == "none" ? label == "none" : label.find(expected) != std::string::npos;
                Log(std::format("{} history {} '{}' (expected '{}')", passed ? "PASS" : "FAIL", which, label,
                                expected));
            }
            else
                Log(std::format("HISTORY undo '{}' redo '{}'", undo.value_or("none"), redo.value_or("none")));
        }
        else if (name == "toast")
        {
            // toast [expect <text>]: logs the messages shown at the bottom of the view; with expect, PASS when one
            // has the text in it; toast clear: takes them away (each scenario starts without)
            std::string action, expected;
            args >> action;
            std::getline(args >> std::ws, expected);
            std::vector<std::string> shown;
            RunOnRenderThread([&] {
                if (action == "clear")
                    UI::Toasts::Clear();
                shown = UI::Toasts::GetShown();
            });
            std::string text;
            bool found = false;
            for (const auto& toast : shown)
            {
                text += "'" + toast + "' ";
                found = found || toast.find(expected) != std::string::npos;
            }
            if (action == "expect")
                Log(std::format("{} toast '{}' in {}", found ? "PASS" : "FAIL", expected, text));
            else
                Log("TOASTS " + text);
        }
        else if (name == "keys")
        {
            // keys select <property> <index...|all|none> | keys count <property> <n> | keys selected <n>: selects
            // keys of a property (campath, or a Types::KeyframeablePropertyType name) on the timeline by their index,
            // or logs PASS when it has n keys, or n items are selected on the timeline. keys interpolation <property>
            // <index...|all> <Smooth|Linear|Hold|Auto>, keys ease <property> <index...|all> <in|out> <amount>: selects
            // them and changes them as the selection's menu does. keys value, keys set, keys expect: see below
            std::string action, propertyName;
            args >> action;
            if (action == "selected")
            {
                std::size_t expected = 0;
                args >> expected;
                std::size_t count = 0;
                RunOnRenderThread([&] { count = UI::TimelineSelection::Count(); });
                Log(std::format("{} keys selected {} (expected {})", count == expected ? "PASS" : "FAIL", count,
                                expected));
                return;
            }
            args >> propertyName;
            auto type = Types::KeyframeablePropertyType::CampathCamera;
            if (propertyName != "campath")
            {
                const auto parsed = ParsePropertyType(propertyName);
                if (!parsed)
                {
                    Log(std::format("FAIL keys: no property {}", propertyName));
                    return;
                }
                type = *parsed;
            }
            auto& keyframeManager = Components::KeyframeManager::Get();
            const auto& property = keyframeManager.GetProperty(type);
            if (action == "clear")
            {
                // keys clear <property>: deletes its keys, as its track's Clear does
                RunOnRenderThread([&] {
                    keyframeManager.ClearKeyframes(property);
                    keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
                });
                return;
            }
            if (action == "count")
            {
                std::size_t expected = 0;
                args >> expected;
                std::size_t count = 0;
                RunOnRenderThread([&] { count = keyframeManager.GetKeyframes(property).size(); });
                Log(std::format("{} keys {} count {} (expected {})", count == expected ? "PASS" : "FAIL", propertyName,
                                count, expected));
                return;
            }
            if (action == "value")
            {
                // keys value <property> <tick> [<value index> <expected> <tolerance>]: the value at a tick, as the
                // game gets it (the campath retimed at constant speed)
                double tick = 0;
                args >> tick;
                Types::KeyframeValue value;
                RunOnRenderThread([&] { value = keyframeManager.Interpolate(property, static_cast<float>(tick)); });
                std::string text;
                for (std::int32_t i = 0; i < property.GetValueCount(); ++i)
                    text += std::format("{:.3f} ", value.GetByIndex(i));
                std::uint32_t index = 0;
                double expected = 0, tolerance = 0;
                if (args >> index >> expected >> tolerance)
                    Log(std::format("{} keys {} value at {} is {:.3f} (expected {:.3f} +- {}): {}",
                                    std::abs(value.GetByIndex(index) - expected) <= tolerance ? "PASS" : "FAIL",
                                    propertyName, tick, value.GetByIndex(index), expected, tolerance, text));
                else
                    Log(std::format("KEYVALUE {} {} {}", propertyName, tick, text));
                return;
            }
            if (action == "set")
            {
                // keys set <property> <tick> <value>...: adds a key with these values, as a double-click on its lane
                // with the values typed would
                std::uint32_t tick = 0;
                args >> tick;
                auto value = Types::KeyframeValue::GetDefaultValue(property.valueType);
                for (std::int32_t i = 0; i < property.GetValueCount(); ++i)
                {
                    float each = 0;
                    if (args >> each)
                        value.SetByIndex(i, each);
                }
                RunOnRenderThread([&] {
                    keyframeManager.AddKeyframe(property, Types::Keyframe(property, tick, value));
                    keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
                });
                return;
            }
            if (action == "expect")
            {
                // keys expect <property> <index> <interpolation> [<ease in> <ease out> [tolerance]] (an ease of -1
                // is left unchecked)
                std::size_t index = 0;
                std::string interpolation;
                float easeIn = -1, easeOut = -1, tolerance = 1e-4f;
                args >> index >> interpolation >> easeIn >> easeOut >> tolerance;
                std::optional<Types::Keyframe> key;
                RunOnRenderThread([&] {
                    const auto& keys = keyframeManager.GetKeyframes(property);
                    if (index < keys.size())
                        key = keys[index];
                });
                if (!key)
                {
                    Log(std::format("FAIL keys {}: no key {}", propertyName, index));
                    return;
                }
                const auto name = std::string(magic_enum::enum_name(key->interpolation));
                const bool passed = _stricmp(name.c_str(), interpolation.c_str()) == 0 &&
                                    (easeIn < 0 || std::abs(key->easeIn - easeIn) < tolerance) &&
                                    (easeOut < 0 || std::abs(key->easeOut - easeOut) < tolerance);
                Log(std::format("{} keys {} {} is {}, eases {:.2f} {:.2f} (expected {} {:.2f} {:.2f})",
                                passed ? "PASS" : "FAIL", propertyName, index, name, key->easeIn, key->easeOut,
                                interpolation, easeIn, easeOut));
                return;
            }
            std::vector<std::string> which;
            for (std::string index; args >> index;)
                which.push_back(index);
            // keys interpolation|ease <property> <index...|all> <interpolation> | <in|out> <amount>: the last words
            // say what, as the menu of the selection does it
            std::string setting, amount;
            if (action == "interpolation" && !which.empty())
            {
                setting = which.back();
                which.pop_back();
            }
            if (action == "ease" && which.size() >= 2)
            {
                amount = which.back();
                which.pop_back();
                setting = which.back();
                which.pop_back();
            }
            RunOnRenderThread([&] {
                const auto& keyframes = keyframeManager.GetKeyframes(property);
                std::vector<UI::TimelineSelection::Item> items;
                for (std::size_t i = 0; i < keyframes.size(); ++i)
                {
                    if (std::ranges::find(which, "all") != which.end() ||
                        std::ranges::find(which, std::to_string(i)) != which.end())
                        items.push_back(UI::TimelineSelection::KeyItem(property.type, keyframes[i].id));
                }
                UI::TimelineSelection::Set(items);
                if (action == "interpolation")
                {
                    if (const auto interpolation =
                            magic_enum::enum_cast<Types::KeyInterpolation>(setting, magic_enum::case_insensitive))
                        UI::TimelineSelection::SetInterpolation(*interpolation);
                    else
                        Log(std::format("FAIL keys: no interpolation {}", setting));
                }
                else if (action == "ease")
                {
                    UI::TimelineSelection::SetEase(setting == "in", std::strtof(amount.c_str(), nullptr));
                }
            });
        }
        else if (name == "constantspeed")
        {
            // constantspeed <on|off> | constantspeed expect <on|off> | constantspeed passing <node index> [<tick>
            // <tolerance>]: turns the campath's constant speed on or off as the Camera tab does, logs PASS when it is
            // on or off, or logs when the camera passes a node (PASS when near the tick)
            std::string action;
            args >> action;
            auto& keyframeManager = Components::KeyframeManager::Get();
            if (action == "passing")
            {
                std::size_t index = 0;
                double expected = 0, tolerance = -1;
                args >> index >> expected >> tolerance;
                std::optional<double> passing;
                RunOnRenderThread([&] {
                    const auto& nodes = keyframeManager.GetKeyframes(
                        keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera));
                    if (index < nodes.size())
                        passing = keyframeManager.GetCampathPassingTick(nodes[index].tick);
                });
                if (!passing)
                    Log(std::format("FAIL constantspeed: no node {}", index));
                else if (tolerance >= 0)
                    Log(std::format("{} constantspeed node {} passed at {:.1f} (expected {} +- {})",
                                    std::abs(*passing - expected) <= tolerance ? "PASS" : "FAIL", index, *passing,
                                    expected, tolerance));
                else
                    Log(std::format("PASSING {} {:.1f}", index, *passing));
                return;
            }
            if (action == "expect")
            {
                std::string expected;
                args >> expected;
                bool on = false;
                RunOnRenderThread([&] { on = keyframeManager.IsConstantSpeed(); });
                Log(std::format("{} constantspeed {} (expected {})", on == (expected == "on") ? "PASS" : "FAIL",
                                on ? "on" : "off", expected));
                return;
            }
            RunOnRenderThread([&] {
                const bool on = keyframeManager.IsConstantSpeed();
                if ((action == "on") != on)
                    UI::Commands::Run("campath.constantSpeed");
            });
            bool on = false;
            RunOnRenderThread([&] { on = keyframeManager.IsConstantSpeed(); });
            Log(std::format("CONSTANTSPEED {}", on ? "on" : "off"));
        }
        else if (name == "diamond" || name == "control")
        {
            // diamond list | diamond <name> [click [right | <fraction across>] | dclick | scroll | expect
            // <none|animated|here>]: the
            // diamonds and keyframeable controls (ImGuiEx::Keyframeable) as they were drawn last: logs them, clicks
            // the middle of one (a diamond is "diamond.<property type>", a control "control.<property type>" with
            // ".0" to ".2" for each value of Direction, or "control.CampathCamera.position|angles|fov",
            // "control.constantSpeed", "control.interpolation"), scrolling its window first so it shows, or PASS when
            // a property's diamond says that
            // control <name> [click [right] | dclick | scroll | expect <drawn|absent>]: the same for the side panel's
            // controls that record where they were drawn (tab.<name>, page.<name>, camera.mode.<mode>, campath.add,
            // node.<i>, node.<i>.goto|update|delete, players.<section>, firstrun.close, firstrun.step.<i>[.done]...)
            std::string control, action, extra;
            args >> control >> action >> extra;
            const auto drawnNow = [] {
                std::vector<std::pair<std::string, ImGuiEx::Keyframeable::Drawn>> drawn;
                RunOnRenderThread([&] { drawn = ImGuiEx::Keyframeable::GetDrawn(); });
                return drawn;
            };
            const auto find = [&](const std::string& wanted) -> std::optional<ImGuiEx::Keyframeable::Drawn> {
                for (const auto& [each, drawn] : drawnNow())
                    if (each == wanted)
                        return drawn;
                return std::nullopt;
            };
            if (name == "control" && action == "expect")
            {
                const bool drawn = find(control).has_value();
                Log(std::format("{} control {} {} (expected {})", drawn == (extra != "absent") ? "PASS" : "FAIL",
                                control, drawn ? "drawn" : "absent", extra));
                return;
            }
            if (control == "list" || control.empty())
            {
                for (const auto& [each, drawn] : drawnNow())
                    Log(std::format("DIAMOND {} {:.0f} {:.0f} {:.0f} {:.0f}", each, drawn.min.x, drawn.min.y,
                                    drawn.max.x, drawn.max.y));
                return;
            }
            if (action == "expect")
            {
                const auto type = ParsePropertyType(control.starts_with("diamond.") ? control.substr(8) : control);
                if (!type)
                {
                    Log(std::format("FAIL diamond: no property {}", control));
                    return;
                }
                auto state = ImGuiEx::Keyframeable::KeyState::None;
                RunOnRenderThread([&] {
                    auto& keyframeManager = Components::KeyframeManager::Get();
                    state = ImGuiEx::Keyframeable::GetKeyState(keyframeManager.GetProperty(*type));
                });
                const auto name = std::string(magic_enum::enum_name(state));
                Log(std::format("{} diamond {} is {} (expected {})",
                                _stricmp(name.c_str(), extra.c_str()) == 0 ? "PASS" : "FAIL", control, name, extra));
                return;
            }
            auto found = find(control);
            if (!found)
            {
                Log(std::format("FAIL diamond: no {} drawn", control));
                return;
            }
            // the window it is in scrolled so it shows
            bool scrolled = false;
            RunOnRenderThread([&] {
                for (const auto windowName : {"Visuals", "Camera", "Players", "Demos", "Kills", "Actors"})
                {
                    auto window = ImGui::FindWindowByName(windowName);
                    if (!window || !window->WasActive ||
                        !window->Rect().Contains(ImVec2(found->min.x + 1, window->Pos.y + 1)))
                        continue;
                    const auto& clip = window->InnerClipRect;
                    const auto margin = 8.0f;
                    float by = 0.0f;
                    if (found->max.y + margin > clip.Max.y)
                        by = found->max.y + margin - clip.Max.y;
                    else if (found->min.y - margin < clip.Min.y)
                        by = found->min.y - margin - clip.Min.y;
                    if (by != 0.0f)
                    {
                        ImGui::SetScrollY(window, window->Scroll.y + by);
                        scrolled = true;
                    }
                }
            });
            if (scrolled)
            {
                Sleep(300);
                found = find(control);
            }
            if (!found || action == "scroll")
                return;
            if (action == "drag")
            {
                // diamond <name> drag <pixels>: presses the middle of it, moves the mouse that far across with the
                // button held, in ten steps a frame or more apart, and lets go there: a slider's drag
                const auto pixels = std::atoi(extra.c_str());
                const POINT from{static_cast<LONG>(std::lround((found->min.x + found->max.x) / 2)),
                                 static_cast<LONG>(std::lround((found->min.y + found->max.y) / 2))};
                PostMouse(WM_MOUSEMOVE, from.x, from.y);
                Sleep(60);
                PostMouse(WM_LBUTTONDOWN, from.x, from.y, MK_LBUTTON);
                Sleep(100);
                for (int i = 1; i <= 10; i++)
                {
                    PostMouse(WM_MOUSEMOVE, from.x + pixels * i / 10, from.y, MK_LBUTTON);
                    Sleep(30);
                }
                Sleep(100);
                PostMouse(WM_LBUTTONUP, from.x + pixels, from.y);
                Log(std::format("DIAMOND drag {} from {} {} by {}", control, from.x, from.y, pixels));
                return;
            }
            if (action == "hover")
            {
                // control <name> hover: moves the mouse over its middle, without a click
                const POINT at{static_cast<LONG>(std::lround((found->min.x + found->max.x) / 2)),
                               static_cast<LONG>(std::lround((found->min.y + found->max.y) / 2))};
                PostMouse(WM_MOUSEMOVE, at.x, at.y);
                Log(std::format("DIAMOND hover {} at {} {}", control, at.x, at.y));
                return;
            }
            // (click <fraction>: that far across it, for a slider; click alt: with Alt held)
            char* parsed = nullptr;
            const auto fraction = std::strtod(extra.c_str(), &parsed);
            const auto across = parsed != extra.c_str() ? fraction : 0.5;
            const POINT at{static_cast<LONG>(std::lround(found->min.x + (found->max.x - found->min.x) * across)),
                           static_cast<LONG>(std::lround((found->min.y + found->max.y) / 2))};
            const bool right = extra == "right";
            const bool alt = extra == "alt";
            PostMouse(WM_MOUSEMOVE, at.x, at.y);
            Sleep(60);
            if (alt)
            {
                RunOnRenderThread([] { ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true); });
                Sleep(60);
            }
            for (int i = 0; i < (action == "dclick" ? 2 : 1); i++)
            {
                PostMouse(right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, at.x, at.y, right ? MK_RBUTTON : MK_LBUTTON);
                // (a slider takes the value where it is pressed while the button is down)
                if (parsed != extra.c_str())
                    Sleep(150);
                PostMouse(right ? WM_RBUTTONUP : WM_LBUTTONUP, at.x, at.y);
            }
            if (alt)
            {
                Sleep(60);
                RunOnRenderThread([] { ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false); });
            }
            Log(std::format("DIAMOND {} {} at {} {}", action, control, at.x, at.y));
        }
        else if (name == "graph")
        {
            // The graph area under the timeline's tracks (UI::TimelineGraph); open it with "tl press graph.<row>"
            //   graph: logs its plot, the values it shows and the values in it
            //   graph point <property> <value index> <key index>: logs where a key is drawn
            //   graph click|dclick|rclick <property> <value index> <key index> [modifiers]: at that key
            //   graph drag <property> <value index> <key index> <tick> <value> [modifiers]: drags a key there
            //   graph at click|dclick|rclick|wheel <tick> <value> [notches] [modifiers]: anywhere on the plot
            //   graph expect shown <n> | inview <property> <value index> (all its keys show) | open | closed
            std::string action;
            args >> action;
            auto timeline = UI::UIManager::Get().GetUIComponent<UI::Timeline>(UI::Component::Timeline);
            const auto check = [&](bool passed, const std::string& what) {
                Log(std::format("{} graph {}", passed ? "PASS" : "FAIL", what));
            };
            const auto parseType = [&](const std::string& text) -> std::optional<Types::KeyframeablePropertyType> {
                if (text == "campath")
                    return Types::KeyframeablePropertyType::CampathCamera;
                return ParsePropertyType(text);
            };
            const auto hold = [&](const std::string& modifiers, bool down) {
                if (modifiers.empty())
                    return;
                RunOnRenderThread([&] {
                    auto& io = ImGui::GetIO();
                    if (modifiers.find("ctrl") != std::string::npos)
                        io.AddKeyEvent(ImGuiMod_Ctrl, down);
                    if (modifiers.find("shift") != std::string::npos)
                        io.AddKeyEvent(ImGuiMod_Shift, down);
                    if (modifiers.find("alt") != std::string::npos)
                        io.AddKeyEvent(ImGuiMod_Alt, down);
                });
                Sleep(60);
            };
            // where a tick and a value are on the plot
            const auto pointAt = [&](double tick, double value) {
                POINT point{};
                RunOnRenderThread([&] {
                    point = POINT{static_cast<LONG>(std::lround(timeline->GetX(tick))),
                                  static_cast<LONG>(std::lround(UI::TimelineGraph::GetValueAxis().ToY(value)))};
                });
                return point;
            };
            const auto keyPoint = [&](const std::string& property, std::uint32_t index,
                                      std::size_t key) -> std::optional<POINT> {
                const auto type = parseType(property);
                std::optional<ImVec2> position;
                if (type)
                    RunOnRenderThread([&] { position = UI::TimelineGraph::GetKeyPoint(*type, index, key); });
                if (!position)
                {
                    Log(std::format("FAIL graph: no key {} of {} {} drawn", key, property, index));
                    return std::nullopt;
                }
                return POINT{static_cast<LONG>(std::lround(position->x)), static_cast<LONG>(std::lround(position->y))};
            };
            const auto clickAt = [&](POINT at, const std::string& how, const std::string& modifiers) {
                PostMouse(WM_MOUSEMOVE, at.x, at.y);
                Sleep(60);
                hold(modifiers, true);
                const bool right = how == "rclick";
                for (int i = 0; i < (how == "dclick" ? 2 : 1); i++)
                {
                    PostMouse(right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, at.x, at.y, right ? MK_RBUTTON : MK_LBUTTON);
                    PostMouse(right ? WM_RBUTTONUP : WM_LBUTTONUP, at.x, at.y);
                }
                Sleep(60);
                hold(modifiers, false);
            };

            if (action.empty())
            {
                ImRect plot;
                UI::TimelineMath::ValueAxis values;
                std::vector<UI::TimelineGraph::Curve> curves;
                RunOnRenderThread([&] {
                    plot = UI::TimelineGraph::GetPlot();
                    values = UI::TimelineGraph::GetValueAxis();
                    curves = UI::TimelineGraph::GetCurves();
                });
                std::string text;
                for (const auto& curve : curves)
                    text += std::format("{}.{}{} ", Components::KeyframeManager::GetTypeName(curve.type), curve.index,
                                        curve.shown ? "" : "(hidden)");
                Log(std::format("GRAPH plot {:.0f} {:.0f} {:.0f} {:.0f} values {:.3f} to {:.3f} curves {}", plot.Min.x,
                                plot.Min.y, plot.Max.x, plot.Max.y, values.min, values.max, text));
            }
            else if (action == "point" || action == "click" || action == "dclick" || action == "rclick")
            {
                std::string property, modifiers;
                std::uint32_t index = 0;
                std::size_t key = 0;
                args >> property >> index >> key >> modifiers;
                if (const auto at = keyPoint(property, index, key))
                {
                    if (action == "point")
                        Log(std::format("GRAPHPOINT {} {} {} at {} {}", property, index, key, at->x, at->y));
                    else
                        clickAt(*at, action, modifiers);
                }
            }
            else if (action == "drag")
            {
                std::string property, modifiers;
                std::uint32_t index = 0;
                std::size_t key = 0;
                double tick = 0, value = 0;
                args >> property >> index >> key >> tick >> value >> modifiers;
                const auto from = keyPoint(property, index, key);
                if (!from)
                    return;
                const auto to = pointAt(tick, value);
                PostMouse(WM_MOUSEMOVE, from->x, from->y);
                Sleep(60);
                hold(modifiers, true);
                PostMouse(WM_LBUTTONDOWN, from->x, from->y, MK_LBUTTON);
                for (int i = 1; i <= 10; i++)
                    PostMouse(WM_MOUSEMOVE, from->x + (to.x - from->x) * i / 10, from->y + (to.y - from->y) * i / 10,
                              MK_LBUTTON);
                Sleep(100);
                PostMouse(WM_LBUTTONUP, to.x, to.y);
                Sleep(60);
                hold(modifiers, false);
                Log(std::format("GRAPH dragged {} {} {} from {} {} to {} {}", property, index, key, from->x, from->y,
                                to.x, to.y));
            }
            else if (action == "at")
            {
                std::string how, modifiers;
                double tick = 0, value = 0;
                args >> how >> tick >> value;
                int notches = 0;
                if (how == "wheel")
                    args >> notches;
                args >> modifiers;
                const auto at = pointAt(tick, value);
                if (how == "wheel")
                {
                    PostMouse(WM_MOUSEMOVE, at.x, at.y);
                    Sleep(60);
                    hold(modifiers, true);
                    PostInput(WM_MOUSEWHEEL, MAKEWPARAM(0, notches * WHEEL_DELTA), MAKELPARAM(at.x, at.y));
                    Sleep(60);
                    hold(modifiers, false);
                }
                else
                {
                    clickAt(at, how, modifiers);
                }
            }
            else if (action == "expect")
            {
                std::string what;
                args >> what;
                std::vector<UI::TimelineGraph::Curve> curves;
                RunOnRenderThread([&] { curves = UI::TimelineGraph::GetCurves(); });
                if (what == "shown")
                {
                    std::size_t expected = 0;
                    args >> expected;
                    const auto shown = std::ranges::count_if(curves, [](const auto& curve) { return curve.shown; });
                    check(static_cast<std::size_t>(shown) == expected,
                          std::format("shows {} values (expected {})", shown, expected));
                }
                else if (what == "open" || what == "closed")
                {
                    check(curves.empty() == (what == "closed"), std::format("{} values in it", curves.size()));
                }
                else if (what == "inview")
                {
                    std::string property;
                    std::uint32_t index = 0;
                    args >> property >> index;
                    const auto type = parseType(property);
                    std::size_t keys = 0, drawn = 0;
                    bool inside = true;
                    RunOnRenderThread([&] {
                        if (!type)
                            return;
                        auto& keyframeManager = Components::KeyframeManager::Get();
                        keys = keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type)).size();
                        const auto plot = UI::TimelineGraph::GetPlot();
                        for (std::size_t k = 0; k < keys; ++k)
                        {
                            if (const auto point = UI::TimelineGraph::GetKeyPoint(*type, index, k))
                            {
                                ++drawn;
                                inside = inside && point->y >= plot.Min.y && point->y <= plot.Max.y;
                            }
                        }
                    });
                    check(keys > 0 && drawn == keys && inside,
                          std::format("{} {}: {} of {} keys drawn within the plot", property, index, drawn, keys));
                }
            }
        }
        else if (name == "tl")
        {
            // The timeline (UI::Timeline), driven by where its rows are: a row is ruler, markers, campath, look, a
            // property's type (FilmtweakBrightness) or actor.<id|last>.<events|clips|keys>, and a tick says where
            // across it. Modifiers are ctrl, shift, alt or ctrl+shift, held for the clicks.
            //   tl rows: logs the rows and the view (lanes is the row of all that shows of the tracks)
            //   tl view <start> <end> | tl fit: shows those ticks, or the whole demo
            //   tl click|dclick|rclick|move <row> <tick> [modifiers]
            //   tl press <button>: clicks a button of the transport (freeze, time, speed, loop, snap, keys), or
            //     actor.<id|last>.lightfold, a light's arrow
            //   tl drag <row> <tick> <row> <tick> [modifiers] | tl mdrag ...: drags with the left (middle) button
            //   tl wheel <row> <tick> <notches> [modifiers]
            //   tl selection: logs what is selected
            //   tl expect selected <n> | node <index> <tick> [tolerance] | clip <actor> <index> <start> <end>
            //     [tolerance] | offset <actor> <clip index> <seconds> | range <start> <end> [tolerance] |
            //     markers <n> | marker <index> <tick> [tolerance] | view <start> <end> [tolerance] | fitted (the
            //     whole demo shows) | playhead <tick> [tolerance] | popup (a menu or popup is open) | unmoved <row>
            //     [tolerance] (it is where tl mark found it) | speed <timescale> [tolerance]: PASS or FAIL
            //   tl mark <row>: remembers where a row (a button of the transport too) is, for tl expect unmoved
            std::string action;
            args >> action;
            auto timeline = UI::UIManager::Get().GetUIComponent<UI::Timeline>(UI::Component::Timeline);
            // (where tl mark found rows: min x, min y, max x, max y)
            static std::map<std::string, std::array<float, 4>> markedRows;
            const auto rowName = [](std::string row) {
                // actor.<id>.<part> is "actor <id> <part>"
                if (row.starts_with("actor."))
                {
                    const auto dot = row.find('.', 6);
                    if (dot != std::string::npos)
                        return std::format("actor {} {}", ParseActorId(row.substr(6, dot - 6)), row.substr(dot + 1));
                }
                return row;
            };
            // a row out of sight, or behind a group's header at the top, is scrolled to first, as a person would (the
            // groups' headers take room)
            const auto scrollToRow = [&](const std::string& row) {
                bool scrolled = false;
                RunOnRenderThread([&] {
                    const auto& rows = timeline->GetRows();
                    const auto lanes = std::ranges::find(rows, std::string("lanes"), &UI::Timeline::Row::name);
                    const auto wanted = std::ranges::find(rows, rowName(row), &UI::Timeline::Row::name);
                    if (lanes == rows.end() || wanted == rows.end() || row == "lanes" || row == "ruler" || row == "markers")
                        return;
                    const auto over = wanted->min.y < lanes->min.y   ? wanted->min.y - lanes->min.y
                                      : wanted->max.y > lanes->max.y ? wanted->max.y - lanes->max.y
                                                                     : 0.0f;
                    if (over == 0.0f)
                        return;
                    for (auto* window : ImGui::GetCurrentContext()->Windows)
                        if (std::string_view(window->Name).find("##tracks") != std::string_view::npos)
                            ImGui::SetScrollY(window, window->Scroll.y + over);
                    scrolled = true;
                });
                if (scrolled)
                    Sleep(200);
            };
            // a point on a row at a tick, in client coordinates
            const auto pointAt = [&](const std::string& row, double tick) -> std::optional<POINT> {
                std::optional<POINT> point;
                scrollToRow(row);
                RunOnRenderThread([&] {
                    const auto& rows = timeline->GetRows();
                    const auto pointOn = [&](const UI::Timeline::Row& each) {
                        return POINT{static_cast<LONG>(std::lround(timeline->GetX(tick))),
                                     static_cast<LONG>(std::lround((each.min.y + each.max.y) / 2))};
                    };
                    for (const auto& each : rows)
                    {
                        if (each.name == rowName(row))
                            point = pointOn(each);
                    }
                    // (lanes: the first actor's row wholly in sight, as their middle can be the gap between two rows)
                    const auto lanes = std::ranges::find(rows, std::string("lanes"), &UI::Timeline::Row::name);
                    if (row == "lanes" && lanes != rows.end())
                    {
                        const auto inSight = std::ranges::find_if(rows, [&](const UI::Timeline::Row& each) {
                            return each.name.starts_with("actor ") && each.min.y >= lanes->min.y &&
                                   each.max.y <= lanes->max.y;
                        });
                        if (inSight != rows.end())
                            point = pointOn(*inSight);
                    }
                });
                if (!point)
                    Log(std::format("FAIL tl: no row {}", row));
                return point;
            };
            const auto hold = [&](const std::string& modifiers, bool down) {
                if (modifiers.empty())
                    return;
                RunOnRenderThread([&] {
                    auto& io = ImGui::GetIO();
                    if (modifiers.find("ctrl") != std::string::npos)
                        io.AddKeyEvent(ImGuiMod_Ctrl, down);
                    if (modifiers.find("shift") != std::string::npos)
                        io.AddKeyEvent(ImGuiMod_Shift, down);
                    if (modifiers.find("alt") != std::string::npos)
                        io.AddKeyEvent(ImGuiMod_Alt, down);
                });
                Sleep(60);
            };
            const auto check = [&](bool passed, const std::string& what) {
                Log(std::format("{} tl {}", passed ? "PASS" : "FAIL", what));
            };

            if (action == "rows")
            {
                std::vector<UI::Timeline::Row> rows;
                UI::TimelineMath::View view;
                RunOnRenderThread([&] {
                    rows = timeline->GetRows();
                    view = timeline->GetView();
                });
                for (const auto& row : rows)
                    Log(std::format("TLROW {} {:.0f} {:.0f} {:.0f} {:.0f}", row.name, row.min.x, row.min.y, row.max.x,
                                    row.max.y));
                Log(std::format("TLVIEW {} {}", view.start, view.end));
            }
            else if (action == "view" || action == "fit")
            {
                std::int32_t start = 0, end = 0;
                args >> start >> end;
                RunOnRenderThread([&] {
                    if (action == "fit")
                        timeline->FitDemo();
                    else
                        timeline->SetView({start, end});
                });
                Sleep(100);
            }
            else if (action == "press")
            {
                // (at a fraction of the way across it, held a moment, for a slider: menu.easein.amount 0.4)
                std::string button;
                double across = 0.5;
                args >> button;
                const bool slider = static_cast<bool>(args >> across);
                if (!slider)
                    across = 0.5;
                if (rowName(button).starts_with("actor "))
                    scrollToRow(button);
                std::optional<POINT> point;
                RunOnRenderThread([&] {
                    for (const auto& each : timeline->GetRows())
                    {
                        if (each.name == rowName(button))
                            point = POINT{static_cast<LONG>(each.min.x + (each.max.x - each.min.x) * across),
                                          static_cast<LONG>((each.min.y + each.max.y) / 2)};
                    }
                });
                if (!point)
                {
                    Log(std::format("FAIL tl press: no button {}", button));
                }
                else
                {
                    PostMouse(WM_MOUSEMOVE, point->x, point->y);
                    Sleep(60);
                    PostMouse(WM_LBUTTONDOWN, point->x, point->y, MK_LBUTTON);
                    // (a slider takes the value where it is pressed while the button is down)
                    if (slider)
                        Sleep(150);
                    PostMouse(WM_LBUTTONUP, point->x, point->y);
                }
            }
            else if (action == "click" || action == "dclick" || action == "rclick" || action == "move")
            {
                std::string row, modifiers;
                double tick = 0;
                args >> row >> tick >> modifiers;
                if (const auto point = pointAt(row, tick))
                {
                    PostMouse(WM_MOUSEMOVE, point->x, point->y);
                    Sleep(60);
                    hold(modifiers, true);
                    const bool right = action == "rclick";
                    for (int i = 0; i < (action == "dclick" ? 2 : action == "move" ? 0 : 1); i++)
                    {
                        PostMouse(right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, point->x, point->y,
                                  right ? MK_RBUTTON : MK_LBUTTON);
                        PostMouse(right ? WM_RBUTTONUP : WM_LBUTTONUP, point->x, point->y);
                    }
                    Sleep(60);
                    hold(modifiers, false);
                }
            }
            else if (action == "dropfrom")
            {
                // tl dropfrom <control> <row> <tick> [modifiers]: drags from a control that records where it was drawn
                // (an animation of the animation browser, browser.<animation>) to a row at a tick and lets go there
                std::string control, toRow, modifiers;
                double toTick = 0;
                args >> control >> toRow >> toTick >> modifiers;
                std::optional<POINT> from;
                RunOnRenderThread([&] {
                    for (const auto& [each, drawn] : ImGuiEx::Keyframeable::GetDrawn())
                        if (each == control)
                            from = POINT{static_cast<LONG>(std::lround((drawn.min.x + drawn.max.x) / 2)),
                                         static_cast<LONG>(std::lround((drawn.min.y + drawn.max.y) / 2))};
                });
                const auto to = pointAt(toRow, toTick);
                if (!from)
                    Log(std::format("FAIL tl dropfrom: no {} drawn", control));
                else if (to)
                {
                    PostMouse(WM_MOUSEMOVE, from->x, from->y);
                    Sleep(60);
                    hold(modifiers, true);
                    PostMouse(WM_LBUTTONDOWN, from->x, from->y, MK_LBUTTON);
                    for (int i = 1; i <= 15; i++)
                        PostMouse(WM_MOUSEMOVE, from->x + (to->x - from->x) * i / 15,
                                  from->y + (to->y - from->y) * i / 15, MK_LBUTTON);
                    // (held there a moment, as a hand would)
                    Sleep(200);
                    PostMouse(WM_LBUTTONUP, to->x, to->y);
                    Sleep(60);
                    hold(modifiers, false);
                    Log(std::format("TL dropped {} on {} at {}", control, toRow, toTick));
                }
            }
            else if (action == "drag" || action == "mdrag")
            {
                std::string fromRow, toRow, modifiers;
                double fromTick = 0, toTick = 0;
                args >> fromRow >> fromTick >> toRow >> toTick >> modifiers;
                const auto from = pointAt(fromRow, fromTick);
                const auto to = pointAt(toRow, toTick);
                if (from && to)
                {
                    const bool middle = action == "mdrag";
                    const WPARAM button = middle ? MK_MBUTTON : MK_LBUTTON;
                    PostMouse(WM_MOUSEMOVE, from->x, from->y);
                    Sleep(60);
                    hold(modifiers, true);
                    PostMouse(middle ? WM_MBUTTONDOWN : WM_LBUTTONDOWN, from->x, from->y, button);
                    for (int i = 1; i <= 10; i++)
                        PostMouse(WM_MOUSEMOVE, from->x + (to->x - from->x) * i / 10,
                                  from->y + (to->y - from->y) * i / 10, button);
                    // (held a moment at the end, so the last frames of the drag see it there)
                    Sleep(100);
                    PostMouse(middle ? WM_MBUTTONUP : WM_LBUTTONUP, to->x, to->y);
                    Sleep(60);
                    hold(modifiers, false);
                }
            }
            else if (action == "wheel")
            {
                std::string row, modifiers;
                double tick = 0;
                int notches = 0;
                args >> row >> tick >> notches >> modifiers;
                if (const auto point = pointAt(row, tick))
                {
                    PostMouse(WM_MOUSEMOVE, point->x, point->y);
                    Sleep(60);
                    hold(modifiers, true);
                    PostInput(WM_MOUSEWHEEL, MAKEWPARAM(0, notches * WHEEL_DELTA), MAKELPARAM(point->x, point->y));
                    Sleep(60);
                    hold(modifiers, false);
                }
            }
            else if (action == "mark")
            {
                std::string row;
                args >> row;
                std::optional<UI::Timeline::Row> found;
                RunOnRenderThread([&] {
                    for (const auto& each : timeline->GetRows())
                    {
                        if (each.name == rowName(row))
                            found = each;
                    }
                });
                if (found)
                {
                    markedRows[row] = {found->min.x, found->min.y, found->max.x, found->max.y};
                    Log(std::format("TLMARK {} {:.1f} {:.1f} {:.1f} {:.1f}", row, found->min.x, found->min.y,
                                    found->max.x, found->max.y));
                }
                else
                    Log(std::format("FAIL tl mark: no row {}", row));
            }
            else if (action == "selection")
            {
                std::string text;
                RunOnRenderThread([&] {
                    for (const auto& item : UI::TimelineSelection::Get())
                        text += std::format("{}:{}:{} ", magic_enum::enum_name(item.kind), item.owner, item.id);
                });
                Log("TLSELECTION " + text);
            }
            else if (action == "expect")
            {
                std::string what;
                args >> what;
                if (what == "selected")
                {
                    // (selected atleast <n>: that many or more)
                    std::string word;
                    args >> word;
                    const bool atLeast = word == "atleast";
                    if (atLeast)
                        args >> word;
                    const auto expected = static_cast<std::size_t>(std::strtoull(word.c_str(), nullptr, 10));
                    std::size_t count = 0;
                    std::string items;
                    RunOnRenderThread([&] {
                        count = UI::TimelineSelection::Count();
                        for (const auto& item : UI::TimelineSelection::Get())
                        {
                            const auto owner =
                                item.kind == UI::TimelineSelection::Kind::Key
                                    ? Components::KeyframeManager::GetTypeName(
                                          static_cast<Types::KeyframeablePropertyType>(item.owner))
                                    : std::to_string(item.owner);
                            const auto span = UI::TimelineSelection::GetSpan(item);
                            items += std::format(" {}:{}@{}", magic_enum::enum_name(item.kind), owner,
                                                 span ? span->start : -1);
                        }
                    });
                    const bool passed = atLeast ? count >= expected : count == expected;
                    // (what it was, for a run where it can't be looked at)
                    check(passed, std::format("selected {} (expected {}{}){}", count, atLeast ? "at least " : "", expected,
                                              passed ? "" : ":" + items));
                }
                else if (what == "node" || what == "marker")
                {
                    std::size_t index = 0;
                    double tick = 0, tolerance = 0;
                    args >> index >> tick;
                    if (!(args >> tolerance))
                        tolerance = 0;
                    std::optional<std::uint32_t> found;
                    RunOnRenderThread([&] {
                        if (what == "node")
                        {
                            auto& keyframeManager = Components::KeyframeManager::Get();
                            const auto& nodes = keyframeManager.GetKeyframes(
                                keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera));
                            if (index < nodes.size())
                                found = nodes[index].tick;
                        }
                        else if (index < Components::Markers::GetAll().size())
                        {
                            found = Components::Markers::GetAll()[index].tick;
                        }
                    });
                    check(found && std::abs(*found - tick) <= tolerance,
                          std::format("{} {} at {} (expected {} +- {})", what, index,
                                      found ? std::to_string(*found) : std::string("none"), tick, tolerance));
                }
                else if (what == "markers")
                {
                    std::size_t expected = 0;
                    args >> expected;
                    std::size_t count = 0;
                    RunOnRenderThread([&] { count = Components::Markers::GetAll().size(); });
                    check(count == expected, std::format("markers {} (expected {})", count, expected));
                }
                else if (what == "clip" || what == "offset")
                {
                    std::string actorText;
                    std::size_t index = 0;
                    double first = 0, second = 0, tolerance = 0;
                    args >> actorText >> index >> first;
                    if (what == "clip")
                        args >> second;
                    if (!(args >> tolerance))
                        tolerance = what == "clip" ? 0 : 0.001;
                    const auto actor = Components::Actors::GetActor(ParseActorId(actorText));
                    if (!actor || index >= actor->clips.size())
                    {
                        check(false, std::format("{}: actor {} has no clip {}", what, actorText, index));
                    }
                    else if (what == "clip")
                    {
                        const auto& clip = actor->clips[index];
                        check(std::abs(clip.start - first) <= tolerance && std::abs(clip.End() - second) <= tolerance,
                              std::format("clip {} from {} to {} (expected {} to {} +- {})", index, clip.start,
                                          clip.End(), first, second, tolerance));
                    }
                    else
                    {
                        const auto& clip = actor->clips[index];
                        check(std::abs(clip.offset - first) <= tolerance,
                              std::format("clip {} offset {:.3f} s (expected {:.3f})", index, clip.offset, first));
                    }
                }
                else if (what == "range" || what == "view")
                {
                    double start = 0, end = 0, tolerance = 0;
                    args >> start >> end;
                    if (!(args >> tolerance))
                        tolerance = 0;
                    double actualStart = 0, actualEnd = 0;
                    RunOnRenderThread([&] {
                        if (what == "range")
                        {
                            const auto& settings = Components::CaptureManager::Get().GetCaptureSettings();
                            actualStart = settings.startTick;
                            actualEnd = settings.endTick;
                        }
                        else
                        {
                            actualStart = timeline->GetView().start;
                            actualEnd = timeline->GetView().end;
                        }
                    });
                    check(std::abs(actualStart - start) <= tolerance && std::abs(actualEnd - end) <= tolerance,
                          std::format("{} {} to {} (expected {} to {} +- {})", what, actualStart, actualEnd, start, end,
                                      tolerance));
                }
                else if (what == "fitted" || what == "popup")
                {
                    bool passed = false;
                    std::string seen;
                    RunOnRenderThread([&] {
                        if (what == "popup")
                        {
                            passed =
                                ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
                            seen = passed ? "open" : "none";
                        }
                        else
                        {
                            const auto view = timeline->GetView();
                            const auto end = Mod::GetGameInterface()->GetDemoInfo().endTick;
                            passed = view.start == 0 && static_cast<std::uint32_t>(view.end) == end;
                            seen = std::format("{} to {} of {}", view.start, view.end, end);
                        }
                    });
                    check(passed, std::format("{} ({})", what, seen));
                }
                else if (what == "playhead")
                {
                    double tick = 0, tolerance = 0;
                    args >> tick;
                    if (!(args >> tolerance))
                        tolerance = 0;
                    const auto now = Components::Playback::GetTimelineTick();
                    check(std::abs(now - tick) <= tolerance,
                          std::format("playhead at {} (expected {} +- {})", now, tick, tolerance));
                }
                else if (what == "unmoved")
                {
                    std::string row;
                    float tolerance = 0;
                    args >> row;
                    if (!(args >> tolerance))
                        tolerance = 0;
                    std::optional<UI::Timeline::Row> found;
                    RunOnRenderThread([&] {
                        for (const auto& each : timeline->GetRows())
                        {
                            if (each.name == rowName(row))
                                found = each;
                        }
                    });
                    const auto marked = markedRows.find(row);
                    if (!found || marked == markedRows.end())
                        Log(std::format("FAIL tl expect unmoved: no row {}{}", row,
                                        marked == markedRows.end() ? " marked" : ""));
                    else
                    {
                        const std::array now = {found->min.x, found->min.y, found->max.x, found->max.y};
                        float moved = 0;
                        for (std::size_t i = 0; i < now.size(); ++i)
                            moved = std::max(moved, std::abs(now[i] - marked->second[i]));
                        check(moved <= tolerance,
                              std::format("{} at {:.1f} {:.1f} {:.1f} {:.1f}, {:.1f} px from where it was marked", row,
                                          now[0], now[1], now[2], now[3], moved));
                    }
                }
                else if (what == "speed")
                {
                    float expected = 0, tolerance = 0;
                    args >> expected;
                    if (!(args >> tolerance))
                        tolerance = 0.0001f;
                    float speed = 0;
                    RunOnRenderThread([&] {
                        if (const auto dvar = Mod::GetGameInterface()->GetDvar("timescale"))
                            speed = dvar->value->floating_point;
                    });
                    check(std::abs(speed - expected) <= tolerance,
                          std::format("speed {} (expected {} +- {})", speed, expected, tolerance));
                }
                else
                {
                    Log(std::format("FAIL tl expect: no {}", what));
                }
            }
            else
            {
                Log(std::format("FAIL tl: no {}", action));
            }
        }
        else if (name == "tab")
        {
            // tab <demo|camera|look|scene|record> | tab <moments|browse|actors|players|bots|you>: a tab of the side
            // panel, or a page of the Demo or Scene tab (bots is actors, you a game you host's); the old names still
            // work (demos: browse, kills: moments, visuals: look)
            static const std::map<std::string, std::pair<UI::Tab, UI::Page>> tabs = {
                {"demo", {UI::Tab::Demo, UI::Page::None}},        {"camera", {UI::Tab::Camera, UI::Page::None}},
                {"look", {UI::Tab::Look, UI::Page::None}},        {"scene", {UI::Tab::Scene, UI::Page::None}},
                {"record", {UI::Tab::Record, UI::Page::None}},    {"moments", {UI::Tab::Demo, UI::Page::Moments}},
                {"browse", {UI::Tab::Demo, UI::Page::Browse}},    {"actors", {UI::Tab::Scene, UI::Page::Actors}},
                {"players", {UI::Tab::Scene, UI::Page::Players}}, {"demos", {UI::Tab::Demo, UI::Page::Browse}},
                {"kills", {UI::Tab::Demo, UI::Page::Moments}},    {"visuals", {UI::Tab::Look, UI::Page::None}},
                {"bots", {UI::Tab::Scene, UI::Page::Actors}},     {"you", {UI::Tab::Scene, UI::Page::You}},
            };
            std::string tab;
            args >> tab;
            if (const auto it = tabs.find(tab); it != tabs.end())
                RunOnRenderThread([&] { UI::UIManager::Get().SelectTab(it->second.first, it->second.second); });
            else
                Log(std::format("FAIL tab: no {}", tab));
        }
        else if (name == "panel")
        {
            // panel: logs the side panel's tab and page and the path shown over it; panel expect tab <name> [page] |
            // panel expect path <text...|none>: PASS when it shows that tab (and page), or a path with the text in
            // it (none: no path); panel scroll <top|bottom|pixels> (see below)
            std::string action, what;
            args >> action >> what;
            UI::Tab tab{};
            UI::Page page{};
            std::string path;
            RunOnRenderThread([&] {
                tab = UI::UIManager::Get().GetSelectedTab();
                page = UI::UIManager::Get().GetPage(tab);
                path = UI::UIManager::Get().GetUIComponent<UI::PrimaryTabs>(UI::Component::PrimaryTabs)->GetShownPath();
            });
            const auto lower = [](std::string_view text) {
                std::string result(text);
                std::ranges::transform(result, result.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return result;
            };
            const auto tabName = lower(magic_enum::enum_name(tab));
            const auto pageName = lower(magic_enum::enum_name(page));
            if (action == "expect" && what == "tab")
            {
                std::string expectedTab, expectedPage;
                args >> expectedTab >> expectedPage;
                const bool same = tabName == expectedTab && (expectedPage.empty() || pageName == expectedPage);
                Log(std::format("{} panel on {} {} (expected {} {})", same ? "PASS" : "FAIL", tabName, pageName,
                                expectedTab, expectedPage));
            }
            else if (action == "expect" && what == "path")
            {
                std::string expected;
                std::getline(args >> std::ws, expected);
                const bool found = expected == "none" ? path.empty() : path.find(expected) != std::string::npos;
                Log(std::format("{} panel path '{}' (expected {})", found ? "PASS" : "FAIL", path, expected));
            }
            else if (action == "scroll")
            {
                // panel scroll <top|bottom|pixels>: the window of the tab (or page) shown scrolled there, or by that
                // much (what is below the fold, such as the Camera tab's nodes, isn't drawn until it shows)
                const std::map<std::string, const char*> windows = {
                    {"demo moments", "Kills"}, {"demo browse", "Demos"},   {"camera none", "Camera"},
                    {"look none", "Visuals"},  {"scene actors", "Actors"}, {"scene players", "Players"},
                    {"scene you", "Actors"},   {"record none", "Capture"}};
                const auto windowName = windows.find(tabName + " " + pageName);
                float scrolled = -1.0f;
                RunOnRenderThread([&] {
                    auto window = windowName != windows.end() ? ImGui::FindWindowByName(windowName->second) : nullptr;
                    if (!window)
                        return;
                    const auto to = what == "top"      ? 0.0f
                                    : what == "bottom" ? window->ScrollMax.y
                                                       : window->Scroll.y + std::strtof(what.c_str(), nullptr);
                    ImGui::SetScrollY(window, std::clamp(to, 0.0f, window->ScrollMax.y));
                    scrolled = to;
                });
                Log(scrolled >= 0.0f ? std::format("PANEL scrolled {} {} to {:.0f}", tabName, pageName, scrolled)
                                     : std::format("FAIL panel scroll: no window for {} {}", tabName, pageName));
            }
            else
            {
                Log(std::format("PANEL {} {} path '{}'", tabName, pageName, path));
            }
        }
        else if (name == "firstrun")
        {
            // firstrun: logs the first-run card's steps done and whether it is closed (UI::GettingStarted);
            // firstrun reset (no step done, shown) | hide | show (as Help > Getting started does) | firstrun expect
            // <step 0-3> <done|todo> | firstrun expect <shown|hidden>: PASS when it is so
            std::string action, what, expected;
            args >> action >> what >> expected;
            UI::GettingStartedLogic::State state;
            RunOnRenderThread([&] {
                if (action == "reset")
                    UI::GettingStarted::Reset();
                else if (action == "hide")
                {
                    auto hidden = UI::GettingStarted::GetState();
                    hidden.hidden = true;
                    UI::GettingStarted::SetState(hidden);
                }
                else if (action == "show")
                    UI::GettingStarted::Show();
                state = UI::GettingStarted::GetState();
            });
            std::string done;
            for (std::size_t i = 0; i < state.done.size(); i++)
                done += state.done[i] ? "1" : "0";
            if (action == "expect" && (what == "shown" || what == "hidden"))
                Log(std::format("{} firstrun {} (expected {})", state.hidden == (what == "hidden") ? "PASS" : "FAIL",
                                state.hidden ? "hidden" : "shown", what));
            else if (action == "expect")
            {
                const auto step = static_cast<std::size_t>(std::clamp(std::atoi(what.c_str()), 0, 3));
                Log(std::format("{} firstrun step {} {} (expected {})",
                                state.done[step] == (expected == "done") ? "PASS" : "FAIL", step,
                                state.done[step] ? "done" : "todo", expected));
            }
            else
                Log(std::format("FIRSTRUN done {} {}", done, state.hidden ? "hidden" : "shown"));
        }
        else if (name == "layout")
        {
            // layout: logs where the view, the side panel and the timeline are (UI::Layout), the splitters' sizes
            // in ems, the font's size and the UI scale; layout expect <view|viewheight|panel|timeline|font> <pixels>
            // [tolerance] | layout expect ems <panel|timeline|graph> <ems> [tolerance]: PASS when it is so;
            // layout <default|editing|watching>: a layout of the View menu
            std::string action, what;
            args >> action >> what;
            ImRect view, panel, timeline;
            UI::LayoutMath::Sizes sizes;
            float font = 0.0f, scale = 0.0f;
            RunOnRenderThread([&] {
                if (action == "default" || action == "editing" || action == "watching")
                    UI::Layout::Apply(action == "default"   ? UI::Layout::Preset::Default
                                      : action == "editing" ? UI::Layout::Preset::Editing
                                                            : UI::Layout::Preset::Watching);
                view = UI::Layout::GetView();
                panel = UI::Layout::GetPanel();
                timeline = UI::Layout::GetTimeline();
                sizes = UI::Layout::GetSizes();
                font = ImGui::GetFontSize();
                scale = PreferencesConfiguration::Get().uiScale;
            });
            if (action == "expect")
            {
                double expected = 0, tolerance = 0;
                if (what == "saved")
                {
                    // what config.json has, as the splitters left it
                    std::optional<UI::LayoutMath::Sizes> saved;
                    float savedScale = 0.0f;
                    try
                    {
                        std::ifstream file(PathUtils::GetModPath() / "config.json");
                        const auto json = nlohmann::json::parse(file)["preferences"];
                        saved = UI::LayoutMath::Sizes{json["layout"]["panel"].get<float>(),
                                                      json["layout"]["timeline"].get<float>(),
                                                      json["layout"]["timelineWithGraph"].get<float>()};
                        savedScale = json["uiScale"].get<float>();
                    }
                    catch (const std::exception& e)
                    {
                        Log(std::format("FAIL layout saved: {}", e.what()));
                    }
                    if (saved)
                    {
                        const bool same = std::abs(saved->panel - sizes.panel) < 0.01f &&
                                          std::abs(saved->timeline - sizes.timeline) < 0.01f &&
                                          std::abs(saved->timelineWithGraph - sizes.timelineWithGraph) < 0.01f &&
                                          std::abs(savedScale - scale) < 0.001f;
                        Log(
                            std::format("{} layout saved {:.2f} {:.2f} {:.2f} scale {} (in use {:.2f} {:.2f} {:.2f} "
                                        "scale {})",
                                        same ? "PASS" : "FAIL", saved->panel, saved->timeline, saved->timelineWithGraph,
                                        savedScale, sizes.panel, sizes.timeline, sizes.timelineWithGraph, scale));
                    }
                }
                else if (what == "ems")
                {
                    std::string part;
                    args >> part >> expected;
                    if (!(args >> tolerance))
                        tolerance = 0.01;
                    const auto ems = part == "panel"   ? sizes.panel
                                     : part == "graph" ? sizes.timelineWithGraph
                                                       : sizes.timeline;
                    Log(std::format("{} layout {} {:.2f} ems (expected {} +- {})",
                                    std::abs(ems - expected) <= tolerance ? "PASS" : "FAIL", part, ems, expected,
                                    tolerance));
                }
                else
                {
                    args >> expected;
                    if (!(args >> tolerance))
                        tolerance = 0;
                    const std::map<std::string, float> pixels = {
                        {"view", view.GetWidth()},
                        {"viewheight", view.GetHeight()},
                        {"panel", panel.GetWidth()},
                        {"timeline", timeline.GetHeight()},
                        {"font", font},
                    };
                    if (const auto it = pixels.find(what); it != pixels.end())
                        Log(std::format("{} layout {} {} (expected {} +- {})",
                                        std::abs(it->second - expected) <= tolerance ? "PASS" : "FAIL", what,
                                        it->second, expected, tolerance));
                    else
                        Log(std::format("FAIL layout expect: no {}", what));
                }
            }
            else
            {
                Log(
                    std::format("LAYOUT view {} {} {} {} panel {} {} {} {} timeline {} {} {} {} ems {:.2f} {:.2f} "
                                "{:.2f} font {} scale {}",
                                view.Min.x, view.Min.y, view.Max.x, view.Max.y, panel.Min.x, panel.Min.y, panel.Max.x,
                                panel.Max.y, timeline.Min.x, timeline.Min.y, timeline.Max.x, timeline.Max.y,
                                sizes.panel, sizes.timeline, sizes.timelineWithGraph, font, scale));
            }
        }
        else if (name == "splitter")
        {
            // splitter <panel|timeline> drag <pixels> | dclick: drags a splitter of the layout by so many pixels
            // (right or down; the panel and the timeline grow the other way) with the mouse, or double-clicks it
            std::string which, action;
            int pixels = 0;
            args >> which >> action >> pixels;
            std::optional<ImRect> rect;
            RunOnRenderThread([&] { rect = UI::Layout::GetSplitter(which); });
            if (!rect)
            {
                Log(std::format("FAIL splitter: no {} splitter drawn", which));
            }
            else
            {
                const auto x = static_cast<int>(rect->GetCenter().x), y = static_cast<int>(rect->GetCenter().y);
                const bool vertical = which == "panel";
                if (action == "dclick")
                {
                    PostMouse(WM_MOUSEMOVE, x, y);
                    Sleep(60);
                    for (int i = 0; i < 2; i++)
                    {
                        PostMouse(WM_LBUTTONDOWN, x, y, MK_LBUTTON);
                        PostMouse(WM_LBUTTONUP, x, y);
                    }
                }
                else
                {
                    // (each step in a frame of its own)
                    const auto toX = vertical ? x + pixels : x, toY = vertical ? y : y + pixels;
                    PostMouse(WM_MOUSEMOVE, x, y);
                    Sleep(60);
                    PostMouse(WM_LBUTTONDOWN, x, y, MK_LBUTTON);
                    WaitForOverlayFrames();
                    for (int i = 1; i <= 10; i++)
                    {
                        PostMouse(WM_MOUSEMOVE, x + (toX - x) * i / 10, y + (toY - y) * i / 10, MK_LBUTTON);
                        WaitForOverlayFrames();
                    }
                    PostMouse(WM_LBUTTONUP, toX, toY);
                }
            }
        }
        else if (name == "uiscale")
        {
            // uiscale <scale>: the UI scale (1 for the font sized from the window), as the View menu sets it
            float scale = 1.0f;
            args >> scale;
            RunOnRenderThread([&] { UI::Layout::SetScale(scale); });
        }
        else if (name == "pause")
        {
            // pause [on|off]: toggles pause, or turns it on or off
            std::string state;
            args >> state;
            if (state.empty() || (state == "on") != Components::Playback::IsPaused())
                Components::Playback::TogglePaused();
        }
        else if (name == "camera")
        {
            // camera <first|third|free|orbit|dolly|bone>
            static const std::map<std::string, Components::Camera::Mode> modes = {
                {"first", Components::Camera::Mode::FirstPerson}, {"third", Components::Camera::Mode::ThirdPerson},
                {"free", Components::Camera::Mode::Free},         {"orbit", Components::Camera::Mode::Orbit},
                {"dolly", Components::Camera::Mode::Dolly},       {"bone", Components::Camera::Mode::Bone},
            };
            std::string mode;
            args >> mode;
            if (const auto it = modes.find(mode); it != modes.end())
                Components::CameraManager::Get().SetActiveCamera(it->second);
        }
        else if (name == "campos")
        {
            // campos [x y z pitch yaw roll [fov]]: logs the active camera's position and rotation, or sets them;
            // campos mark, campos expect moved <distance>: PASS when it moved at least that far since the mark
            static glm::vec3 marked{};
            auto& camera = Components::CameraManager::Get().GetActiveCamera();
            args >> std::ws;
            if (std::isalpha(args.peek()))
            {
                std::string action, what;
                float distance = 0.0f;
                args >> action >> what >> distance;
                const auto moved = glm::distance(camera->GetPosition(), marked);
                if (action == "mark")
                    marked = camera->GetPosition();
                else
                    Log(std::format("{} campos moved {:.1f} (expected at least {:.1f})",
                                    moved >= distance ? "PASS" : "FAIL", moved, distance));
                return;
            }
            glm::vec3 position, rotation;
            if (args >> position.x >> position.y >> position.z >> rotation.x >> rotation.y >> rotation.z)
            {
                camera->GetPosition() = position;
                camera->GetRotation() = rotation;
                if (float fov; args >> fov)
                    camera->GetFov() = fov;
            }
            const auto& p = camera->GetPosition();
            const auto& r = camera->GetRotation();
            Log(std::format("CAMPOS {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} {:.1f}", p.x, p.y, p.z, r.x, r.y, r.z));
        }
        else if (name == "hud")
        {
            // hud <2d|player|shellshock|crosshair|score|icons|blood|killfeed> <0|1>, like the Visuals menu sets them;
            // only sticks while the overlay is hidden, as the menu writes them every frame
            std::string setting;
            int show = 1;
            args >> setting >> show;
            auto hudInfo = Mod::GetGameInterface()->GetHudInfo();
            const std::map<std::string, bool*> settings = {
                {"2d", &hudInfo.show2DElements},     {"player", &hudInfo.showPlayerHUD},
                {"shellshock", &hudInfo.showShellshock}, {"crosshair", &hudInfo.showCrosshair},
                {"score", &hudInfo.showScore},        {"icons", &hudInfo.showIconsAndText},
                {"blood", &hudInfo.showBloodOverlay}, {"killfeed", &hudInfo.showKillfeed},
            };
            if (const auto it = settings.find(setting); it != settings.end())
            {
                *it->second = show != 0;
                Mod::GetGameInterface()->SetHudInfo(hudInfo);
            }
        }
        else if (name == "dvar")
        {
            // dvar <name>: logs the current value as four floats, which also covers bools and vectors
            std::string dvarName;
            args >> dvarName;
            const auto dvar = Functions::FindDvar(dvarName);
            if (dvar && dvar->type == Structures::DVAR_TYPE_STRING)
                Log(std::format("DVAR {} '{}'", dvarName, dvar->current.string ? dvar->current.string : ""));
            else if (dvar)
                Log(std::format("DVAR {} type {} {} {} {} {}", dvarName, static_cast<int>(dvar->type),
                                dvar->current.vector[0], dvar->current.vector[1], dvar->current.vector[2],
                                dvar->current.vector[3]));
            else
                Log("DVAR " + dvarName + " not found");
        }
        else if (name == "elements")
        {
            // elements <everything|worldandplayers|onlygun|onlyworld|onlyplayers>, as a pass shows them
            static const std::map<std::string, Components::VisibleElements> elements = {
                {"everything", Components::VisibleElements::Everything},
                {"worldandplayers", Components::VisibleElements::WorldAndPlayers},
                {"onlygun", Components::VisibleElements::OnlyGun},
                {"onlyworld", Components::VisibleElements::OnlyWorld},
                {"onlyplayers", Components::VisibleElements::OnlyPlayers},
            };
            std::string element;
            args >> element;
            if (const auto it = elements.find(element); it != elements.end())
                Components::Rendering::SetVisibleElements(it->second);
        }
        else if (name == "capture")
        {
            // capture <directory> <ms> <pass>...: captures ProRes 422 LT (at 50 fps unless fps= says) from the current tick into the
            // directory, one file per pass; a pass is default, depth or normal, with :everything, :onlygun,
            // :onlyworld, :onlyplayers or :worldandplayers. The saved output directory and the capture's settings are
            // put back afterwards (the framerate left at 50 made the timeline's frame steps 20 ms in the scenarios
            // after, timeline-move and timeline-transport).
            std::string outputDirectory;
            std::int32_t length = 1000;
            args >> outputDirectory >> length;
            std::vector<Components::PassData> passes;
            std::optional<Components::Resolution> renderResolution;
            std::optional<Components::Resolution> outputResolution;
            std::int32_t framerate = 50;
            std::optional<std::int32_t> from;
            for (std::string pass; args >> pass;)
            {
                // from=<tick> starts there instead of at the current tick
                if (pass.starts_with("from="))
                {
                    from = std::stoi(pass.substr(5));
                    continue;
                }

                // fps=<framerate> captures at that framerate
                if (pass.starts_with("fps="))
                {
                    framerate = std::stoi(pass.substr(4));
                    continue;
                }

                // render=<width>x<height> renders at that size, output=<width>x<height> encodes at that size
                if (pass.starts_with("render=") || pass.starts_with("output="))
                {
                    Components::Resolution resolution{};
                    if (sscanf_s(pass.c_str() + 7, "%dx%d", &resolution.width, &resolution.height) == 2)
                        (pass[0] == 'r' ? renderResolution : outputResolution) = resolution;
                    continue;
                }

                const auto colon = pass.find(':');
                const auto type = magic_enum::enum_cast<Components::PassType>(pass.substr(0, colon),
                                                                               magic_enum::case_insensitive);
                const auto elements = colon == std::string::npos
                                          ? std::optional(Components::VisibleElements::Everything)
                                          : magic_enum::enum_cast<Components::VisibleElements>(
                                                pass.substr(colon + 1), magic_enum::case_insensitive);
                if (type && elements)
                    passes.push_back({type.value(), elements.value()});
            }

            // (a capture doesn't start while the renderer restarts, as for Shadow quality)
            for (int i = 0; i < 6600 && Components::CaptureManager::Get().IsRestarting(); ++i)
                Sleep(50);

            auto& preferences = PreferencesConfiguration::Get();
            const auto savedDirectory = preferences.captureOutputDirectory;
            std::optional<Components::CaptureSettings> savedSettings;
            RunOnRenderThread([&] {
                auto& settings = Components::CaptureManager::Get().GetCaptureSettings();
                savedSettings = settings;
                settings.startTick = from.value_or(Components::Playback::GetTimelineTick());
                settings.endTick = settings.startTick + length;
                settings.outputFormat = Components::OutputFormat::Video;
                settings.videoCodec = Components::VideoCodec::Prores422LT;
                settings.framerate = framerate;
                settings.passes = passes;
                settings.renderResolution = renderResolution;
                settings.resolution = outputResolution.value_or(
                    renderResolution.value_or(Components::CaptureManager::Get().GetGameResolution()));
                preferences.captureOutputDirectory = outputDirectory;
                Components::CaptureManager::Get().StartCapture();
            });
            Log("CAPTURING");
            Sleep(500);
            capturedShadowMapSize = 0;
            while (Components::CaptureManager::Get().IsSwitchingRenderSize() ||
                   Components::CaptureManager::Get().IsCapturing())
            {
                if (Components::CaptureManager::Get().IsCapturing())
                    capturedShadowMapSize = Hooks::Rendering::GetSunShadowMapSize();
                Sleep(50);
            }
            RunOnRenderThread([&] {
                preferences.captureOutputDirectory = savedDirectory;
                if (savedSettings)
                    Components::CaptureManager::Get().GetCaptureSettings() = *savedSettings;
            });
            const auto first = Components::CaptureManager::Get().GetFirstFrame();
            Log(std::format("CAPTURED {} frames, {} without ReShade's effects; the first frame: Pack {} swaps, {} left, "
                            "Surface depth {} draws, {} left",
                            Components::CaptureManager::Get().GetCapturedFrameCount(),
                            Components::CaptureManager::Get().GetFramesWithoutReShade(), first.packSwaps, first.packLeft,
                            first.surfaceDraws, first.surfaceLeft));
        }
        else if (name == "rendersize")
        {
            // rendersize <width> <height> | rendersize off
            std::string width;
            std::int32_t height = 0;
            args >> width >> height;
            if (width == "off")
                Mod::GetGameInterface()->SetRenderSize(std::nullopt);
            else
                Mod::GetGameInterface()->SetRenderSize(std::make_pair(std::stoi(width), height));
        }
        else if (name == "backbuffer")
        {
            // logs the size of the back buffer and of the window's client area
            D3DSURFACE_DESC desc{};
            RunOnRenderThread([&] {
                IDirect3DSurface9* backBuffer = nullptr;
                if (SUCCEEDED(D3D9::GetDevice()->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
                {
                    backBuffer->GetDesc(&desc);
                    backBuffer->Release();
                }
            });
            RECT client{};
            GetClientRect(GetGameWindow(), &client);
            Log(std::format("BACKBUFFER {}x{} client {}x{}", desc.Width, desc.Height, client.right, client.bottom));
        }
        else if (name == "materials")
        {
            // materials: logs the names of the loaded materials (rgp.sortedMaterials)
            const auto sortedMaterials = reinterpret_cast<const char***>(GetGameAddresses().rgp_sortedMaterials());
            std::string line;
            for (int i = 0; i < 0x1000; i++)
            {
                if (!sortedMaterials[i] || !*sortedMaterials[i])
                    continue;
                line += std::format("{}:{} ", i, *sortedMaterials[i]);
                if (line.size() > 900)
                {
                    Log("MATERIALS " + line);
                    line.clear();
                }
            }
            Log("MATERIALS " + line);
        }
        else if (name == "closepopups")
            // closes the Readme and other popups of the overlay, which escape would leave the demo for (ImGui asserts
            // when there are none)
            RunOnRenderThread([] {
                if (ImGui::GetCurrentContext()->OpenPopupStack.Size > 0)
                    ImGui::ClosePopupToLevel(0, true);
            });
        else if (name == "deathanimwindow")
            // (the death animations are on the Scene tab's Players page now)
            RunOnRenderThread([] {
                UI::UIManager::Get()
                    .GetUIComponent<UI::PlayersMenu>(UI::Component::PlayersMenu)
                    ->Open(UI::PlayersMenu::Section::DeathAnimation);
            });
        else if (name == "deathanim")
        {
            // deathanim all <animation|original> | deathanim player <client> <animation|original|default> |
            // deathanim team <team> <animation|original|default>
            std::string target, animation;
            std::int32_t client = -1;
            args >> target;
            if (target == "expect")
            {
                // deathanim expect team<team>|<client>|all <animation>: PASS when that is chosen for them
                args >> animation;
                std::string wanted;
                args >> wanted;
                const auto got = animation.starts_with("team")
                                     ? Components::PlayerAnimation::GetAnimationForTeam(std::stoi(animation.substr(4)))
                                 : animation == "all" ? std::optional(Components::PlayerAnimation::GetAnimationForAll())
                                     : Components::PlayerAnimation::GetAnimationForPlayer(std::stoi(animation));
                Log(std::format("{} deathanim {} {} (expected {})", got == wanted ? "PASS" : "FAIL", animation,
                                got.value_or("none"), wanted));
                return;
            }
            if (target == "player" || target == "team")
                args >> client;
            args >> animation;
            const auto value = animation == "original" ? std::string() : animation;
            if (target == "all")
                Components::PlayerAnimation::SetAnimationForAll(value);
            else if (target == "team")
                Components::PlayerAnimation::SetAnimationForTeam(
                    client, animation == "default" ? std::nullopt : std::optional<std::string_view>(value));
            else if (animation == "default")
                Components::PlayerAnimation::SetAnimationForPlayer(client, std::nullopt);
            else
                Components::PlayerAnimation::SetAnimationForPlayer(client, value);
        }
        else if (name == "pack")
        {
            // pack <zone>: loads a single player or Zombies zone's characters for the demo's actors, as the actors UI
            // does (see ContentPacks.cpp), and logs what it kept and left out
            std::string zone;
            args >> zone;
            Components::Actors::SetPack(zone);
            // checked before the next frame sorts the materials, which crashes on an incomplete one
            std::vector<std::string> incomplete;
            RunOnMainThread([&] {
                ContentPacks::RunRequestedLoad();
                incomplete = ContentPacks::FindIncompleteMaterials();
            });
            const auto loaded = ContentPacks::GetLastLoadResult();
            std::string counts;
            for (std::size_t type = 0; type < loaded.kept.size(); ++type)
                if (loaded.kept[type] || loaded.skipped[type] || loaded.poolFull[type] || loaded.shared[type])
                    counts += std::format("{}:{}/{}/{}/{} ", type, loaded.kept[type], loaded.skipped[type],
                                          loaded.poolFull[type], loaded.shared[type]);
            Log(std::format("PACK {} loaded {} error '{}' memory {} -> {} -> {} MB, {} invalid names, {} registered late",
                            zone, loaded.loaded, loaded.error, loaded.memoryBefore >> 20, loaded.memorySurveyed >> 20,
                            loaded.memoryAfter >> 20, loaded.invalidNames, loaded.registeredLate));
            Log("PACK type:kept/skipped/poolfull/shared " + counts);
            Log(std::format("PACK released {} textures, {} MB, {} world buffers; replaced {} technique sets (adding {} "
                            "constants and samplers), removed {} techniques",
                            loaded.releasedTextures, loaded.releasedTextureBytes >> 20, loaded.releasedWorldBuffers,
                            loaded.replacedTechniqueSets, loaded.addedArguments, loaded.removedTechniques));
            std::string models;
            for (const auto& model : loaded.models)
                models += model + " ";
            Log("PACK models " + models);
            for (const auto& note : loaded.notes)
                Log("PACK note " + note);
            for (const auto& material : incomplete)
                Log("MATERIAL incomplete " + material);
            Log(std::format("{} pack {}: {} materials lack arguments", incomplete.empty() ? "PASS" : "FAIL", zone,
                            incomplete.size()));
        }
        else if (name == "scriptthreads")
        {
            // scriptthreads: logs, by thread, how often script strings, variables and memory of each instance were
            // changed (with IWXMVM_DIAG_THREADS, see Diagnostics.cpp)
            Log("SCRIPTTHREADS" + Diagnostics::DescribeScriptThreads());
        }
        else if (name == "materialcheck")
        {
            // materialcheck: logs the registered materials that lack a constant or sampler of their technique set or
            // have them out of order, which crashes the renderer (see ContentPacks::MakeDrawable), and PASS when there
            // are none
            std::vector<std::string> incomplete;
            RunOnMainThread([&] { incomplete = ContentPacks::FindIncompleteMaterials(); });
            for (const auto& material : incomplete)
                Log("MATERIAL incomplete " + material);
            Log(std::format("{} materialcheck: {} materials lack arguments", incomplete.empty() ? "PASS" : "FAIL",
                            incomplete.size()));
        }
        else if (name == "bonecheck")
        {
            // bonecheck [built]: builds every character the Actors tab offers as actors are built, with and without
            // the gun of the most bones, and PASS when none is built of more bones than a DObj can have nor without a
            // body that fits alone (see Actors::CheckBones); up to <built> (default 300) of those that had too many are
            // made into DObjs, which the game counts itself (it drops out of the demo when there are too many)
            std::size_t maxBuilt = 300;
            args >> maxBuilt;
            Actors::BoneCheck check;
            RunOnMainThread([&] { check = Actors::CheckBones(maxBuilt); });
            for (const auto& example : check.examples)
                Log("BONECHECK over " + example);
            Log(
                std::format("{} bonecheck: {} characters (gun {}), {} over {} bones as chosen, {} still over as built, "
                            "{} without their body, {} made into DObjs",
                            check.characters > 0 && check.overBuilt == 0 && check.bodyLeftOut == 0 ? "PASS" : "FAIL",
                            check.characters, check.gun, check.overLimit, Structures::MAX_DOBJ_BONES, check.overBuilt,
                            check.bodyLeftOut, check.built));
        }
        else if (name == "assets")
        {
            // assets <type> <part> [max]: logs the names of registered assets of a type that contain part
            std::int32_t type = 0;
            std::string part;
            std::size_t max = 50;
            args >> type >> part >> max;
            std::vector<std::string> names;
            RunOnMainThread([&] {
                const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
                const auto entries =
                    reinterpret_cast<const Structures::XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
                for (std::size_t hash = 0; hash < Structures::DB_HASH_TABLE_SIZE; ++hash)
                {
                    for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
                    {
                        const auto& entry = entries[index];
                        if (entry.type != type || !entry.header)
                            continue;
                        const std::string_view assetName = *static_cast<const char* const*>(entry.header);
                        if (assetName.find(part) != std::string_view::npos)
                            names.emplace_back(assetName);
                    }
                }
            });
            std::ranges::sort(names);
            std::string text;
            for (std::size_t i = 0; i < names.size() && i < max; ++i)
                text += names[i] + " ";
            Log(std::format("ASSETS {} '{}' {} found: {}", type, part, names.size(), text));
        }
        else if (name == "actor")
        {
            // actor <model>[+<model>...] <animation|-> [x y z yaw] | actor clear | actor list | actor clip <actor id>
            // <animation> <start tick> <length> [blend in]: adds an actor to the demo's scene as the actors UI does
            // (with a 10 s clip from the current tick, on the ground in front of the camera without a position), or
            // a clip to an actor (a copy of an animation that differs from the game's has ~ for the space before its
            // zone)
            std::string model, animation;
            args >> model >> animation;
            if (model.find('+') != std::string::npos || model.starts_with("c_"))
                std::ranges::replace(animation, '~', ' ');
            if (model == "clear")
            {
                // (each scenario starts with it: the Actors panel's sections open again and its Advanced off, as a
                // scenario may leave them otherwise)
                Components::Actors::Clear();
                RunOnRenderThread([] { UI::ResetActorPanel(); });
                Log("ACTOR cleared");
            }
            else if (model == "count")
            {
                // actor count <n>: PASS when the scene has that many actors
                const auto count = Components::Actors::GetActors().size();
                Log(std::format("{} actor count {} (expected {})", std::to_string(count) == animation ? "PASS" : "FAIL",
                                count, animation));
            }
            else if (model == "select")
            {
                // actor select <id|last|none>: as clicking it in the list
                const auto id = animation == "none" ? std::nullopt : std::optional(ParseActorId(animation));
                Components::Actors::Select(id);
                Log(std::format("ACTOR selected {}", id.value_or(0)));
            }
            else if (model == "selectclip")
            {
                // actor selectclip <id|last> [index | last | animation]: selects an actor's clip (its first, its last,
                // or the first of that animation), which opens the Actors tab's Animation page on it
                std::string which;
                args >> which;
                const auto id = ParseActorId(animation);
                const auto actor = Components::Actors::GetActor(id);
                std::size_t index = 0;
                if (which == "last" && actor && !actor->clips.empty())
                    index = actor->clips.size() - 1;
                else if (!which.empty() && std::isdigit(static_cast<unsigned char>(which.front())))
                    index = std::stoul(which);
                else if (!which.empty() && actor)
                    index = static_cast<std::size_t>(
                        std::ranges::find(actor->clips, which, &Types::ActorClip::animation) - actor->clips.begin());
                if (!actor || index >= actor->clips.size())
                {
                    Log("FAIL actor selectclip: no such actor or clip");
                    return;
                }
                Components::Actors::Select(id);
                Components::Actors::SelectClip(actor->clips[index].id);
                Log(std::format("ACTOR selected clip {} of {}", actor->clips[index].id, id));
            }
            else if (model == "placing")
            {
                // actor placing <0|1>: the Movement page's Click to place
                const bool on = animation == "1";
                RunOnRenderThread([&] { GFX::SetPlacingPathPoints(on); });
                Log(std::format("ACTOR placing {}", on));
            }
            else if (model == "clip")
            {
                std::uint32_t id = ParseActorId(animation);
                std::string clipAnimation;
                std::uint32_t start = 0, length = 3000, blendIn = 250;
                int loop = 1;
                args >> clipAnimation >> start >> length;
                if (!(args >> blendIn))
                    blendIn = 250;
                if (!(args >> loop))
                    loop = 1;
                // (a copy of an animation that differs from the game's is named with its zone after a space, written ~
                // here: pb_stand_alert_pistol~(common_zombie))
                std::ranges::replace(clipAnimation, '~', ' ');
                const auto clip = Components::Actors::AddClip(id, {.animation = clipAnimation,
                                                                   .start = start,
                                                                   .length = length,
                                                                   .loop = loop != 0,
                                                                   .blendIn = blendIn});
                Log(std::format("ACTOR clip {} added to {}", clip, id));
            }
            else if (model == "path" || model == "key" || model == "still")
            {
                // actor path <id> <x y z>...: walks it along points; actor key <id> <tick> <x y z yaw>: keys it;
                // actor still <id>
                auto actor = Components::Actors::GetActor(ParseActorId(animation));
                if (actor)
                {
                    if (model == "path")
                    {
                        actor->movement = Types::ActorMovement::Path;
                        actor->path.clear();
                        for (float x, y, z; args >> x >> y >> z;)
                            actor->path.emplace_back(x, y, z);
                    }
                    else if (model == "key")
                    {
                        actor->movement = Types::ActorMovement::Keyframes;
                        std::uint32_t tick = 0;
                        float x = 0, y = 0, z = 0, yaw = 0;
                        args >> tick >> x >> y >> z >> yaw;
                        Components::Actors::ApplyPlacement(*actor, tick, {x, y, z}, yaw);
                    }
                    else
                    {
                        actor->movement = Types::ActorMovement::Still;
                    }
                    Components::Actors::Update(*actor);
                    const auto placement =
                        Components::Actors::EvaluatePlacement(*actor, Components::Playback::GetTimelineTick());
                    Log(std::format("ACTOR {} moves {}; now at {:.1f} {:.1f} {:.1f} yaw {:.1f}", actor->id, model,
                                    placement.position.x, placement.position.y, placement.position.z, placement.yaw));
                }
            }
            else if (model == "weapon")
            {
                // actor weapon <id> <weapon|-> [camo [attachment...]]: puts a gun of the loadouts in its hand
                if (auto actor = Components::Actors::GetActor(ParseActorId(animation)))
                {
                    std::string weapon;
                    std::int32_t camo = 0;
                    args >> weapon >> camo;
                    actor->weapon = weapon == "-" ? "" : weapon;
                    actor->camo = camo;
                    actor->attachments.clear();
                    for (std::string attachment; args >> attachment;)
                        actor->attachments.push_back(attachment);
                    std::ranges::sort(actor->attachments);
                    Components::Actors::Update(*actor);
                    Log(std::format("ACTOR {} holds '{}' camo {}", actor->id, Components::Actors::ResolveWeapon(*actor),
                                    camo));
                }
            }
            else if (model == "event")
            {
                // actor event <id> fire|hit|effect|show|hide|look <tick|now> [shots interval | effect [tag] |
                // ahead|camera|point <x y z>|actor <id>|player <client> [turn ms]]; now is the playhead's tick, as
                // the Actors tab's Event button adds them
                std::string type, when;
                args >> type >> when;
                const auto tick = when == "now" ? Components::Playback::GetActorTick()
                                                : static_cast<std::uint32_t>(std::strtoul(when.c_str(), nullptr, 10));
                Types::ActorEvent event{.tick = tick};
                if (type == "fire")
                {
                    event.type = Types::ActorEventType::Fire;
                    if (!(args >> event.shots >> event.interval))
                        event.interval = 100;
                }
                else if (type == "hit" || type == "effect")
                {
                    event.type = type == "hit" ? Types::ActorEventType::Hit : Types::ActorEventType::Effect;
                    args >> event.effect >> event.tag;
                    if (event.effect == "-")
                        event.effect.clear();
                }
                else if (type == "look")
                {
                    event.type = Types::ActorEventType::Look;
                    std::string target;
                    args >> target;
                    if (target == "ahead")
                        event.lookTarget = Types::ActorLookTarget::Ahead;
                    else if (target == "point")
                    {
                        event.lookTarget = Types::ActorLookTarget::Point;
                        args >> event.lookPoint.x >> event.lookPoint.y >> event.lookPoint.z;
                    }
                    else if (target == "actor")
                    {
                        event.lookTarget = Types::ActorLookTarget::Actor;
                        std::string other;
                        args >> other;
                        event.lookActor = ParseActorId(other);
                    }
                    else if (target == "player")
                    {
                        event.lookTarget = Types::ActorLookTarget::Player;
                        args >> event.lookPlayer;
                    }
                    if (!(args >> event.lookTurn))
                        event.lookTurn = 400;
                }
                else
                {
                    event.type = type == "show" ? Types::ActorEventType::Show : Types::ActorEventType::Hide;
                }
                const auto id = Components::Actors::AddEvent(ParseActorId(animation), event);
                // (a Hit dies with a clip of the scenario's that starts with it, as "and dies" makes them)
                if (auto actor = Components::Actors::GetActor(ParseActorId(animation));
                    actor && event.type == Types::ActorEventType::Hit)
                {
                    ActorLogic::LinkDeaths(*actor);
                    Components::Actors::Update(*actor, true);
                }
                Log(std::format("ACTOR event {} {} at {} added to {}", id, type, tick, animation));
            }
            else if (model == "where")
            {
                // actor where <id>: logs where it stands now and how fast its clips walk
                if (const auto actor = Components::Actors::GetActor(ParseActorId(animation)))
                {
                    const auto placement =
                        Components::Actors::EvaluatePlacement(*actor, Components::Playback::GetTimelineTick());
                    std::string motions;
                    for (const auto& clip : actor->clips)
                    {
                        const auto motion = Components::Actors::GetAnimationMotion(clip.animation);
                        const auto walk = Components::Actors::GetClipWalk(*actor, clip);
                        motions += std::format("{}=({:.1f} {:.1f}) walks {:.1f}/s at {} ", clip.animation,
                                               motion ? motion->x : 0.0f, motion ? motion->y : 0.0f, walk.speed,
                                               walk.angle ? std::format("{:.0f}", *walk.angle) : "-");
                    }
                    Log(std::format("ACTOR {} at {:.1f} {:.1f} {:.1f} yaw {:.1f}; motion per play {}", actor->id,
                                    placement.position.x, placement.position.y, placement.position.z, placement.yaw,
                                    motions));
                }
            }
            else if (model == "expect")
            {
                // actor expect <id> <x> <y> <yaw> <distance> <degrees>: PASS when it stands within the distance of
                // x y and faces within the degrees of yaw now
                float x = 0, y = 0, yaw = 0, distance = 0, degrees = 0;
                args >> x >> y >> yaw >> distance >> degrees;
                if (const auto actor = Components::Actors::GetActor(ParseActorId(animation)))
                {
                    const auto placement =
                        Components::Actors::EvaluatePlacement(*actor, Components::Playback::GetTimelineTick());
                    const auto off = glm::distance(glm::vec2(placement.position), glm::vec2(x, y));
                    const auto turn = std::abs(std::remainder(placement.yaw - yaw, 360.0f));
                    Log(std::format("{} actor {} at {:.1f} {:.1f} yaw {:.1f}: {:.1f} units and {:.1f} degrees off "
                                    "(at most {} and {})",
                                    off <= distance && turn <= degrees ? "PASS" : "FAIL", actor->id,
                                    placement.position.x, placement.position.y, placement.yaw, off, turn, distance,
                                    degrees));
                }
                else
                    Log("FAIL actor expect: no such actor");
            }
            else if (model == "has")
            {
                // actor has <id> movement <still|keys|path|attached> | clips <n> | deaths <n> | count <animation> <n> |
                // clip <index> <animation|*> [<start> <end> [tolerance]] | events <n> | event <index>
                // <fire|hit|effect|show|hide|look> [<tick> [tolerance]] | weapon <weapon|->: PASS when the actor has
                // that
                std::string what;
                args >> what;
                const auto actor = Components::Actors::GetActor(ParseActorId(animation));
                if (!actor)
                {
                    Log("FAIL actor has: no such actor");
                    return;
                }
                const auto within = [](std::int64_t value, std::int64_t wanted, std::int64_t tolerance) {
                    return wanted < 0 || std::abs(value - wanted) <= tolerance;
                };
                bool pass = false;
                std::string seen, wanted;
                if (what == "movement")
                {
                    constexpr std::array NAMES{"still", "keys", "path", "attached"};
                    args >> wanted;
                    seen = NAMES[static_cast<std::size_t>(actor->movement)];
                    pass = seen == wanted;
                }
                else if (what == "clips" || what == "events")
                {
                    args >> wanted;
                    seen = std::to_string(what == "clips" ? actor->clips.size() : actor->events.size());
                    pass = seen == wanted;
                }
                else if (what == "deaths")
                {
                    // (how many of its clips are deaths)
                    args >> wanted;
                    const auto animations = Components::Actors::GetAnimationLookup();
                    seen = std::to_string(std::ranges::count_if(actor->clips, [&](const Types::ActorClip& clip) {
                        return !clip.loop && ActorLogic::IsDeath(clip.animation, animations(clip.animation));
                    }));
                    pass = seen == wanted;
                }
                else if (what == "count")
                {
                    // (how many of its clips play an animation)
                    std::string counted;
                    args >> counted >> wanted;
                    seen = std::format(
                        "{} of {}", std::ranges::count(actor->clips, counted, &Types::ActorClip::animation), counted);
                    pass = seen == std::format("{} of {}", wanted, counted);
                }
                else if (what == "clip")
                {
                    std::size_t index = 0;
                    std::int64_t start = -1, end = -1, tolerance = 0;
                    args >> index >> wanted >> start >> end >> tolerance;
                    if (index < actor->clips.size())
                    {
                        const auto& clip = actor->clips[index];
                        seen = std::format("{} {} to {}", clip.animation, clip.start, clip.End());
                        pass = (wanted == "*" || clip.animation == wanted) && within(clip.start, start, tolerance) &&
                               within(clip.End(), end, tolerance);
                    }
                    wanted += std::format(" {} to {} +- {}", start, end, tolerance);
                }
                else if (what == "event")
                {
                    std::size_t index = 0;
                    std::int64_t tick = -1, tolerance = 0;
                    args >> index >> wanted >> tick >> tolerance;
                    if (index < actor->events.size())
                    {
                        const auto& event = actor->events[index];
                        auto type = std::string(magic_enum::enum_name(event.type));
                        std::ranges::transform(type, type.begin(),
                                               [](char c) { return static_cast<char>(std::tolower(c)); });
                        seen = std::format("{} at {}", type, event.tick);
                        pass = type == wanted && within(event.tick, tick, tolerance);
                    }
                    wanted += std::format(" at {} +- {}", tick, tolerance);
                }
                else if (what == "weapon")
                {
                    args >> wanted;
                    seen = actor->weapon.empty() ? "-" : actor->weapon;
                    pass = seen == wanted;
                }
                Log(std::format("{} actor {} has {} {} (expected {})", pass ? "PASS" : "FAIL", actor->id, what,
                                seen.empty() ? "none" : seen, wanted));
            }
            else if (model == "pack")
            {
                // actor pack [expect <zone|none>]: logs the scene's content pack and what the game has loaded or is
                // loading; with expect, PASS when the scene's pack is that one and the game has it loaded (waiting up
                // to a minute for it to load)
                std::string wanted;
                if (animation == "expect")
                {
                    args >> wanted;
                    if (wanted == "none")
                        wanted.clear();
                    for (int waited = 0; waited < 120; ++waited)
                    {
                        const auto now = Components::Actors::GetCatalog();
                        if (now && now->loadingPack.empty() && (wanted.empty() || now->loadedPack == wanted))
                            break;
                        Sleep(500);
                    }
                }
                const auto pack = Components::Actors::GetPack();
                const auto catalog = Components::Actors::GetCatalog();
                const auto loaded = catalog ? catalog->loadedPack : std::string();
                const auto loading = catalog ? catalog->loadingPack : std::string();
                if (animation == "expect")
                {
                    const bool pass = pack == wanted && (wanted.empty() || loaded == wanted);
                    Log(std::format("{} actor pack '{}' loaded '{}' (expected '{}')", pass ? "PASS" : "FAIL", pack,
                                    loaded, wanted));
                }
                else
                    Log(std::format("ACTOR pack '{}' loaded '{}' loading '{}'", pack, loaded, loading));
            }
            else if (model == "favorite" || model == "recent")
            {
                // actor favorite <animation> <0|1>: stars an animation in the browser, or takes its star away;
                // actor recent [clear]: logs (or forgets) the animations chosen last
                if (model == "favorite")
                {
                    std::string on;
                    args >> on;
                    RunOnRenderThread([&] { Components::Actors::SetFavoriteAnimation(animation, on == "1"); });
                    Log(std::format("ACTOR favorite {} {}", animation, on));
                }
                else
                {
                    if (animation == "clear")
                        RunOnRenderThread([] { PreferencesConfiguration::Get().recentActorAnimations.clear(); });
                    std::string names;
                    for (const auto& name : Components::Actors::GetRecentAnimations())
                        names += " " + name;
                    Log("ACTOR recent" + names);
                }
            }
            else if (model == "look")
            {
                // actor look <id> [<degrees>]: logs how far the face of its head is off from what it looks at; with
                // degrees, PASS when it is within them
                float degrees = -1.0f;
                args >> degrees;
                const auto id = ParseActorId(animation);
                std::optional<float> error;
                RunOnMainThread([&] { error = Actors::GetLookError(id); });
                if (!error)
                    Log(std::format("{}actor {} looks at nothing", degrees >= 0 ? "FAIL " : "", id));
                else
                    Log(std::format("{}actor {} looks {:.1f} degrees off{}",
                                    degrees < 0 ? "" : *error <= degrees ? "PASS " : "FAIL ", id, *error,
                                    degrees < 0 ? "" : std::format(" (at most {})", degrees)));
            }
            else if (model == "prop")
            {
                // actor prop <id> <model> <tag> [x y z pitch yaw roll]: puts a model on a bone of the actor
                Types::ActorProp prop;
                args >> prop.model >> prop.tag;
                args >> prop.offset.x >> prop.offset.y >> prop.offset.z >> prop.angles.x >> prop.angles.y >>
                    prop.angles.z;
                if (auto actor = Components::Actors::GetActor(ParseActorId(animation)))
                {
                    actor->props.push_back(prop);
                    Components::Actors::Update(*actor);
                    Log(std::format("ACTOR prop {} on {} of {}", prop.model, prop.tag, actor->id));
                }
            }
            else if (model == "props")
            {
                // actor props <id> [<distance>]: logs where the models on its bones were drawn; with a distance, PASS
                // when each is drawn within it of where its bone and offset put it
                float distance = -1.0f;
                args >> distance;
                const auto id = ParseActorId(animation);
                std::vector<Actors::PropState> states;
                RunOnMainThread([&] { states = Actors::GetPropStates(id); });
                bool pass = !states.empty();
                std::string text;
                for (const auto& state : states)
                {
                    pass &= state.drawn && state.distance <= distance;
                    text += std::format("{} {} at {:.1f} {:.1f} {:.1f} ({:.2f} off); ", state.model,
                                        state.drawn ? "drawn" : "not drawn", state.position.x, state.position.y,
                                        state.position.z, state.distance);
                }
                Log(std::format("{}actor {} props: {}", distance < 0 ? "" : pass ? "PASS " : "FAIL ", id,
                                text.empty() ? "none" : text));
            }
            else if (model == "attach")
            {
                // actor attach <id> actor <id>|player <client> <tag> [x y z yaw]: attaches the actor to a bone
                std::string kind, target;
                auto actor = Components::Actors::GetActor(ParseActorId(animation));
                args >> kind >> target;
                if (actor)
                {
                    auto& attachment = actor->attachment;
                    attachment.kind = kind == "player" ? Types::ActorAttachment::Kind::Player
                                                       : Types::ActorAttachment::Kind::Actor;
                    if (attachment.kind == Types::ActorAttachment::Kind::Player)
                        attachment.player = std::stoi(target);
                    else
                        attachment.actor = ParseActorId(target);
                    args >> attachment.tag;
                    args >> attachment.offset.x >> attachment.offset.y >> attachment.offset.z >> attachment.yaw;
                    actor->movement = Types::ActorMovement::Attached;
                    Components::Actors::Update(*actor);
                    Log(std::format("ACTOR {} attached to {} {} {}", actor->id, kind, target, attachment.tag));
                }
            }
            else if (model == "group")
            {
                // actor group <id> <count> <per row> <spacing> <row spacing> [stagger]: copies the actor in a
                // formation, as the Look page does; the last copy becomes "last", the actor "previous"
                ActorLogic::Formation formation;
                args >> formation.count >> formation.columns >> formation.spacing.x >> formation.spacing.y;
                if (!(args >> formation.stagger))
                    formation.stagger = 0;
                const auto ids = Components::Actors::AddFormation(ParseActorId(animation), formation,
                                                                  Components::Playback::GetTimelineTick());
                if (!ids.empty())
                    previousActorId = std::exchange(lastActorId, ids.back());
                Log(std::format("ACTOR group of {} copies", ids.size()));
            }
            else if (model == "models")
            {
                // actor models <id> <model>[+<model>...]: makes it of these models, as a scene or preset can
                std::string models;
                args >> models;
                if (auto actor = Components::Actors::GetActor(ParseActorId(animation)))
                {
                    actor->models.clear();
                    std::istringstream parts(models);
                    for (std::string part; std::getline(parts, part, '+');)
                        actor->models.push_back(part);
                    Components::Actors::Update(*actor);
                    Log(std::format("ACTOR {} made of {}", actor->id, models));
                }
            }
            else if (model == "leftout")
            {
                // actor leftout <id> [<model>[+<model>...]|- [gun]]: logs what the game left out of it, as it has too
                // many bones, and the models it built it of; with models (- for none), PASS when it is built and
                // exactly those were left out, and the gun with "gun"
                std::string expected, gun;
                const bool expecting = static_cast<bool>(args >> expected);
                args >> gun;
                const auto id = ParseActorId(animation);
                std::vector<std::string> builtModels;
                RunOnMainThread([&] { builtModels = Actors::GetBuiltModels(id); });
                const auto leftOut = Components::Actors::GetLeftOut(id).value_or(Components::Actors::LeftOut{});
                std::string left, builtText;
                for (const auto& name : leftOut.models)
                    left += (left.empty() ? "" : "+") + name;
                for (const auto& name : builtModels)
                    builtText += name + " ";
                const auto text = std::format("actor {} left out {}{} ({} bones), built of {}", id,
                                              left.empty() ? "-" : left, leftOut.gun ? " and the gun" : "",
                                              leftOut.bones, builtText.empty() ? "nothing" : builtText);
                if (!expecting)
                    Log("ACTOR " + text);
                else
                    Log(std::format(
                        "{} {}",
                        !builtModels.empty() && (left.empty() ? "-" : left) == expected && leftOut.gun == (gun == "gun")
                            ? "PASS"
                            : "FAIL",
                        text));
            }
            else if (model == "reload")
            {
                // actor reload: reads the demo's scene from its file again, as when the demo loads; last and previous
                // are its last two actors then
                std::vector<std::uint32_t> before;
                for (const auto& actor : Components::Actors::GetActors())
                    before.push_back(actor.id);
                Components::Actors::ReloadScene();
                std::vector<std::uint32_t> after = before;
                for (int i = 0; i < 100 && after == before; ++i)
                {
                    Sleep(50);
                    after.clear();
                    for (const auto& actor : Components::Actors::GetActors())
                        after.push_back(actor.id);
                }
                if (after.size() >= 2)
                    previousActorId = after[after.size() - 2];
                if (!after.empty())
                    lastActorId = after.back();
                Log(std::format("ACTOR scene reloaded: {} actors{}", after.size(),
                                after == before ? " (the same ids, so maybe not)" : ""));
            }
            else if (model == "copylook")
            {
                // actor copylook <id> <client>: makes a player look like the actor; PASS when it could
                std::int32_t client = 0;
                args >> client;
                if (const auto actor = Components::Actors::GetActor(ParseActorId(animation)))
                {
                    const auto error = Components::Actors::CopyLookToPlayer(*actor, client);
                    Log(error.empty() ? std::format("PASS actor {} look copied to client {}", actor->id, client)
                                      : std::format("FAIL actor {} look not copied: {}", actor->id, error));
                }
            }
            else if (model == "points")
            {
                // actor points <id> <count> [<index> <x> <y> <distance>]: PASS when its path has that many points,
                // the one at the index within the distance of x y
                std::size_t count = 0, index = 0;
                float x = 0, y = 0, distance = 0;
                args >> count;
                const bool where = static_cast<bool>(args >> index >> x >> y >> distance);
                if (const auto actor = Components::Actors::GetActor(ParseActorId(animation)))
                {
                    const auto& path = actor->path;
                    bool pass = path.size() == count;
                    std::string text = std::format("{} points", path.size());
                    if (where)
                    {
                        const auto off = index < path.size()
                                             ? glm::distance(glm::vec2(path[index]), glm::vec2(x, y))
                                             : FLT_MAX;
                        pass &= off <= distance;
                        if (index < path.size())
                            text += std::format(", point {} at {:.1f} {:.1f} {:.1f} is {:.1f} units off", index,
                                                path[index].x, path[index].y, path[index].z, off);
                    }
                    Log(std::format("{} actor {} has {} (expected {})", pass ? "PASS" : "FAIL", actor->id, text,
                                    count));
                }
                else
                    Log("FAIL actor points: no such actor");
            }
            else if (model == "list")
            {
                for (const auto& actor : Components::Actors::GetActors())
                {
                    std::string models, clips;
                    for (const auto& m : actor.models)
                        models += m + " ";
                    for (const auto& clip : actor.clips)
                        clips += std::format("[{} {} {}+{}{}] ", clip.id, clip.animation, clip.start, clip.length,
                                             clip.loop ? " loop" : "");
                    std::string events;
                    for (const auto& event : actor.events)
                        events += std::format("({} {} {}{}) ", event.id, static_cast<int>(event.type), event.tick,
                                              event.death != 0 ? std::format(" dies {}", event.death) : "");
                    std::string path;
                    for (const auto& point : actor.path)
                        path += std::format("<{:.0f} {:.0f} {:.0f}> ", point.x, point.y, point.z);
                    Log(std::format("ACTOR {} '{}' {}{}{}{}{} keys at {:.1f} {:.1f} {:.1f} yaw {:.1f}{}", actor.id,
                                    actor.name, models, clips, events, path, actor.keys.size(), actor.position.x,
                                    actor.position.y, actor.position.z, actor.yaw, actor.hidden ? " hidden" : ""));
                }
            }
            else
            {
                Types::Actor actor;
                std::istringstream parts(model);
                for (std::string part; std::getline(parts, part, '+');)
                    actor.models.push_back(part);
                if (animation != "-")
                {
                    actor.clips.push_back({.animation = animation,
                                           .start = Components::Playback::GetTimelineTick(),
                                           .length = 10000});
                }
                float x, y, z, yaw;
                const bool placed = static_cast<bool>(args >> x >> y >> z >> yaw);
                if (placed)
                {
                    actor.position = {x, y, z};
                    actor.yaw = yaw;
                }
                const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                if (!placed)
                    actor.yaw = camera->GetRotation().y + 180.0f;
                const auto id = Components::Actors::Add(actor);
                previousActorId = std::exchange(lastActorId, id);
                if (!placed)
                    Components::Actors::RequestPlacement(id, camera->GetPosition(), camera->GetForwardVector());
                Log(std::format("ACTOR {} added ({} actors)", id, Components::Actors::GetActors().size()));
            }
        }
        else if (name == "frametimes")
        {
            // frametimes <ms>: logs how evenly the main and render threads ran their frames meanwhile
            int ms = 1000;
            args >> ms;
            const auto mainStart = mainFrameTimes.count.load(), renderStart = renderFrameTimes.count.load();
            Sleep(ms);
            for (const auto& [label, intervals] :
                 {std::pair{"main", mainFrameTimes.Since(mainStart)}, std::pair{"render", renderFrameTimes.Since(renderStart)}})
            {
                if (intervals.empty())
                    continue;
                const auto mean = std::accumulate(intervals.begin(), intervals.end(), 0.0) / intervals.size();
                double variance = 0;
                for (const auto interval : intervals)
                    variance += (interval - mean) * (interval - mean);
                const auto spikes = std::ranges::count_if(intervals, [&](double i) { return i > mean * 1.5; });
                Log(std::format("FRAMETIMES {} {} frames, mean {:.2f} ms, sd {:.2f}, max {:.2f}, {} over 1.5x mean", label,
                                intervals.size(), mean, std::sqrt(variance / intervals.size()),
                                std::ranges::max(intervals), spikes));
            }
        }
        else if (name == "movetest")
        {
            // movetest <ms>: activates the free camera's controls, holds W meanwhile and logs how evenly the view moved
            // from frame to frame
            int ms = 1000;
            args >> ms;
            PostInput(WM_KEYDOWN, 'F', 1);
            PostInput(WM_KEYUP, 'F', 0xC0000001);
            Sleep(300);
            RunOnMainThread([] { viewTrace.clear(); });
            renderTraced = renderUnfocused = renderNoKey = renderNoTime = 0;
            tracingView = true;
            PostInput(WM_KEYDOWN, 'W', 1);
            Sleep(ms);
            PostInput(WM_KEYUP, 'W', 0xC0000001);
            tracingView = false;
            Sleep(100);
            PostInput(WM_KEYDOWN, 'F', 1);
            PostInput(WM_KEYUP, 'F', 0xC0000001);

            std::vector<std::pair<std::int64_t, std::array<float, 3>>> trace;
            RunOnMainThread([&] { trace = viewTrace; });
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            std::vector<double> speeds, steps;
            std::size_t still = 0;
            for (std::size_t i = 1; i < trace.size(); ++i)
            {
                const auto& [t0, a] = trace[i - 1];
                const auto& [t1, b] = trace[i];
                const auto step = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
                const auto dt = (t1 - t0) / static_cast<double>(frequency.QuadPart);
                steps.push_back(step);
                if (step < 0.001)
                    ++still;
                else if (dt > 0)
                    speeds.push_back(step / dt);
            }
            if (speeds.size() < 2)
            {
                Log(std::format("MOVETEST {} frames, the view didn't move", trace.size()));
            }
            else
            {
                const auto mean = std::accumulate(speeds.begin(), speeds.end(), 0.0) / speeds.size();
                double variance = 0;
                for (const auto speed : speeds)
                    variance += (speed - mean) * (speed - mean);
                // the steps once it moves, as distance/ms
                std::string first;
                const auto start = std::ranges::find_if(steps, [](double step) { return step >= 0.001; }) - steps.begin();
                for (std::size_t i = start; i < steps.size() && i < start + 40; ++i)
                    first += std::format("{:.1f}/{:.0f} ", steps[i],
                                         (trace[i + 1].first - trace[i].first) * 1000.0 / frequency.QuadPart);
                Log(std::format("MOVETEST {} frames, {} without movement, speed {:.0f} units/s, sd {:.0f}; render frames "
                                "{}, unfocused {}, no key {}, no time {}; steps {}",
                                trace.size(), still, mean, std::sqrt(variance / speeds.size()), renderTraced.load(),
                                renderUnfocused.load(), renderNoKey.load(), renderNoTime.load(), first));
            }
        }
        else if (name == "hang")
        {
            // hang <ms>: blocks the main thread, for testing send.ps1's stall detection
            int ms = 0;
            args >> ms;
            std::lock_guard lock(mainThreadMutex);
            mainThreadActions.push_back([ms] { Sleep(ms); });
        }
        else if (name == "project")
        {
            // project <x y z>: logs where a world point is in the view, in client coordinates for click and drag
            glm::vec3 point{};
            args >> point.x >> point.y >> point.z;
            std::optional<glm::vec2> screen;
            RunOnRenderThread([&] { screen = GFX::WorldToScreen(point); });
            if (screen)
                Log(std::format("PROJECT {:.0f} {:.0f}", screen->x, screen->y));
            else
                Log("PROJECT behind");
        }
        else if (name == "gizmo")
        {
            // gizmo [expect <none|node|actor|any> [hovered <id|none>]]: logs what the gizmo moves, whether it shows and
            // the actor under the mouse; with expect, PASS or FAIL
            // gizmo mode <local|world|rotate>: PASS when the gizmo is in that mode (the view's buttons at its top left,
            // controls view.gizmo.local|world|rotate, choose it)
            const auto graphics = GFX::GetGizmoState();
            std::string word, expected, hoverWord, expectedHover;
            args >> word >> expected;
            if (word == "mode")
            {
                Log(std::format("{} gizmo mode {} (expected {})", graphics.mode == expected ? "PASS" : "FAIL",
                                graphics.mode, expected));
                return;
            }
            static const std::map<GFX::GizmoTarget, std::string> targets = {
                {GFX::GizmoTarget::None, "none"},
                {GFX::GizmoTarget::CampathNode, "node"},
                {GFX::GizmoTarget::Actor, "actor"},
            };
            const auto target = targets.at(graphics.target);
            const auto hovered = graphics.hoveredActor;
            const auto selected = Components::Actors::GetSelected();
            const auto node = graphics.selectedNode;
            const auto state = std::format("target {} shown {} hovered {} selected actor {} node {} mode {} over {} object {}", target,
                                           graphics.shown ? 1 : 0, hovered ? std::to_string(*hovered) : "none",
                                           selected ? std::to_string(*selected) : "none",
                                           node ? std::to_string(*node) : "none", graphics.mode, graphics.overView ? 1 : 0,
                                           graphics.objectHovered ? 1 : 0);
            if (!expected.empty())
            {
                bool passed = expected == "any" || expected == target;
                if (args >> hoverWord >> expectedHover)
                    passed = passed && (hovered ? expectedHover != "none" && *hovered == ParseActorId(expectedHover)
                                                : expectedHover == "none");
                Log(std::format("{} gizmo {} (expected {} {})", passed ? "PASS" : "FAIL", state, expected,
                                expectedHover));
            }
            else
                Log("GIZMO " + state);
        }
        else if (name == "node")
        {
            // node add | node list | node clear | node expect <x y z> <distance>: adds a campath node where the camera is
            // at the timeline's tick, logs the nodes, removes them, or logs PASS when the first is within the distance
            std::string action;
            args >> action;
            auto& keyframes = Components::KeyframeManager::Get();
            const auto& property = keyframes.GetProperty(Types::KeyframeablePropertyType::CampathCamera);
            if (action == "add")
            {
                RunOnRenderThread([&] {
                    const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                    Types::CameraData data;
                    data.position = camera->GetPosition();
                    data.rotation = camera->GetRotation();
                    data.fov = camera->GetFov();
                    keyframes.AddKeyframe(property,
                                          Types::Keyframe(property, Components::Playback::GetTimelineTick(), data));
                });
            }
            else if (action == "clear")
                RunOnRenderThread([&] { keyframes.ClearKeyframes(property); });
            else if (action == "angles" || action == "turned")
            {
                // node angles <pitch yaw roll> <degrees> | node turned <pitch yaw roll> <degrees>: PASS when the first
                // node's angles are all within the degrees of those, or when one is at least that far from them
                glm::vec3 expected{};
                float degrees = 0;
                args >> expected.x >> expected.y >> expected.z >> degrees;
                std::optional<glm::vec3> rotation;
                RunOnRenderThread([&] {
                    if (!keyframes.GetKeyframes(property).empty())
                        rotation = keyframes.GetKeyframes(property).front().value.cameraData.rotation;
                });
                float off = 180.0f;
                if (rotation)
                {
                    off = 0.0f;
                    for (int i = 0; i < 3; ++i)
                        off = std::max(off, std::abs(std::remainder((*rotation)[i] - expected[i], 360.0f)));
                }
                const bool passed = action == "angles" ? off <= degrees : off >= degrees;
                Log(std::format("{} node angles {:.1f} {:.1f} {:.1f}: {:.1f} degrees off ({} {})",
                                passed ? "PASS" : "FAIL", rotation ? rotation->x : 0.0f, rotation ? rotation->y : 0.0f,
                                rotation ? rotation->z : 0.0f, off, action == "angles" ? "at most" : "at least",
                                degrees));
                return;
            }
            else if (action == "expect")
            {
                glm::vec3 expected{};
                float distance = 0;
                args >> expected.x >> expected.y >> expected.z >> distance;
                std::optional<glm::vec3> position;
                RunOnRenderThread([&] {
                    if (!keyframes.GetKeyframes(property).empty())
                        position = keyframes.GetKeyframes(property).front().value.cameraData.position;
                });
                const auto off = position ? glm::distance(*position, expected) : FLT_MAX;
                Log(std::format("{} node at {:.3f} {:.3f} {:.3f}: {:.3f} units off (at most {})",
                                off <= distance ? "PASS" : "FAIL", position ? position->x : 0.0f,
                                position ? position->y : 0.0f, position ? position->z : 0.0f, off, distance));
                return;
            }
            std::string text;
            RunOnRenderThread([&] {
                for (const auto& keyframe : keyframes.GetKeyframes(property))
                    text += std::format("{} at {} ({:.1f} {:.1f} {:.1f}) ", keyframe.id, keyframe.tick,
                                        keyframe.value.cameraData.position.x, keyframe.value.cameraData.position.y,
                                        keyframe.value.cameraData.position.z);
            });
            Log("NODES " + text);
        }
        else if (name == "keyframes")
        {
            // keyframes <path>: loads a keyframe file, as File > Open does; keyframes write <path>: writes the keys to
            // one, as File > Export does
            std::string path;
            std::getline(args >> std::ws, path);
            if (path.starts_with("write "))
                RunOnRenderThread([path] { Components::KeyframeSerializer::Write(path.substr(6)); });
            else
                RunOnRenderThread([path] { Components::KeyframeSerializer::Read(path); });
            Sleep(200);
        }
        else if (name == "players")
        {
            // players: logs the players' and corpses' weapon fields and models, and the demo player's
            RunOnMainThread([&] {
                const auto entities = Structures::GetEntities();
                const auto cg = Structures::GetClientGlobals();
                if (!entities || !cg || !cg->snap)
                    return;
                const auto models = [](std::int32_t number) {
                    std::string text;
                    if (const auto obj = Functions::Com_GetClientDObj(number, 0))
                        for (int m = 0; m < obj->numModels; ++m)
                            text += std::string(obj->models[m]->name) + " ";
                    return text;
                };
                for (std::size_t i = 0; i < Structures::MAX_GENTITIES; ++i)
                {
                    const auto& es = entities[i].nextState;
                    if (es.eType != Structures::ET_PLAYER && es.eType != Structures::ET_PLAYER_CORPSE)
                        continue;
                    Log(std::format("PLAYER {} type {} client {} weapon {} primary {} stowed {} render {:08x} models {}", i,
                                    static_cast<int>(es.eType), es.clientNum, es.weapon, es.primaryWeapon,
                                    es.stowedWeapon, es.renderOptions, models(static_cast<std::int32_t>(i))));
                }
                for (std::size_t i = 0; i < Structures::MAX_CLIENTS; ++i)
                {
                    const auto& info = cg->clientInfo[i];
                    if (!info.infoValid)
                        continue;
                    std::string attached;
                    for (std::size_t a = 0; a < std::size(info.attachModelNames) && info.attachModelNames[a][0]; ++a)
                        attached += std::format("{}[{}] ", info.attachModelNames[a], info.attachTagNames[a]);
                    Log(std::format("CLIENT {} {} team {} body {} attached {}", i, info.name,
                                    Components::Loadouts::GetTeam(static_cast<std::int32_t>(i)), info.model, attached));
                }
                const auto& ps = cg->predictedPlayerState;
                std::string held;
                for (const auto& weapon : ps.heldWeapons)
                    if (weapon.weapon)
                        held += std::format("{}:{:08x} ", weapon.weapon, weapon.renderOptions);
                Log(std::format("PS client {} weapon {} view model {} render {:08x} held {}", ps.clientNum, ps.weapon,
                                cg->viewModelWeapon, ps.renderOptions, held));
            });
        }
        else if (name == "loadout")
        {
            // loadout <all|client> body|head <model|keep|none>, loadout <all|client> worn <model,...|none|keep>,
            // loadout <all|client> character <id>, loadout <all|client> facepaint <number|keep>,
            // loadout <all|client> rule <from|any> <weapon|keep> <attachment,...|none|keep> <camo|keep>, loadout reset,
            // loadout window, loadout catalog
            std::string target, what;
            args >> target >> what;
            if (target == "reset")
            {
                Components::Loadouts::ResetLoadouts();
                return;
            }
            if (target == "window")
            {
                // (the Scene tab's Players page)
                RunOnRenderThread([] {
                    UI::UIManager::Get()
                        .GetUIComponent<UI::PlayersMenu>(UI::Component::PlayersMenu)
                        ->Open(UI::PlayersMenu::Section::Loadout);
                });
                return;
            }
            if (target == "expect")
            {
                // loadout expect <all|client|team1|team2> <text>: PASS when what is chosen for them, as the scene keeps
                // it, has that text
                std::string text;
                args >> text;
                Components::Loadouts::Target who;
                if (what.starts_with("team"))
                    who.team = std::stoi(what.substr(4));
                else if (what != "all")
                    who.client = std::stoi(what);
                Types::PlayerChoices choices;
                choices.loadoutForAll = Components::Loadouts::GetLoadoutFor(who);
                const auto dump = Components::PlayerLookLogic::ChoicesToJson(choices)["all"].dump();
                Log(std::format("{} loadout {}: {}", dump.find(text) != std::string::npos ? "PASS" : "FAIL", what, dump));
                return;
            }
            if (target == "catalog")
            {
                const auto catalog = Components::Loadouts::GetCatalog();
                if (!catalog)
                {
                    Log("CATALOG none");
                    return;
                }
                Log(std::format("CATALOG weapons {} variants {} attachments {} bodies {} heads {}",
                                catalog->weapons.size(), catalog->variants.size(), catalog->attachments.size(),
                                catalog->bodies.size(), catalog->heads.size()));
                std::string line;
                for (const auto& weapon : catalog->weapons)
                    line += std::format("{}={}({}) ", weapon.name, weapon.displayName, weapon.category);
                Log("CATALOG " + line);
                line.clear();
                for (const auto& attachment : catalog->attachments)
                    line += attachment.name + "=" + attachment.displayName + " ";
                Log("CATALOG " + line);
                return;
            }
            // (team1 and team2 for a team)
            Components::Loadouts::Target who;
            if (target.starts_with("team"))
                who.team = std::stoi(target.substr(4));
            else if (target != "all")
                who.client = std::stoi(target);
            auto loadout = Components::Loadouts::GetLoadoutFor(who);
            const auto optional = [](const std::string& value) {
                return value == "keep" ? std::nullopt : std::optional(value);
            };
            if (what == "body" || what == "head")
            {
                std::string model;
                args >> model;
                (what == "body" ? loadout.body : loadout.head) =
                    model == "none" ? std::optional(std::string()) : optional(model);
            }
            else if (what == "worn")
            {
                std::string models;
                args >> models;
                loadout.worn.reset();
                if (models != "keep")
                {
                    loadout.worn.emplace();
                    std::istringstream list(models == "none" ? "" : models);
                    for (std::string model; std::getline(list, model, ',');)
                        loadout.worn->push_back(model);
                }
            }
            else if (what == "character")
            {
                std::string id;
                args >> id;
                if (!Components::Actors::SetLoadoutCharacter(loadout, id))
                    Log("LOADOUT no character " + id);
            }
            else if (what == "facepaint")
            {
                std::string paint;
                args >> paint;
                loadout.facePaint = paint == "keep" ? std::nullopt : std::optional(std::stoi(paint));
            }
            else if (what == "stowed")
            {
                // (a base weapon; none takes the demo's off, keep leaves it)
                std::string weapon;
                args >> weapon;
                loadout.stowed = weapon == "keep"   ? std::nullopt
                                 : weapon == "none" ? std::optional(std::string())
                                                    : std::optional(weapon);
            }
            else if (what == "rule")
            {
                std::string from, weapon, attachments, camo;
                args >> from >> weapon >> attachments >> camo;
                Types::WeaponRule rule{from == "any"       ? ""
                                       : from == "primary"   ? std::string(Types::WeaponRule::ANY_PRIMARY)
                                       : from == "secondary" ? std::string(Types::WeaponRule::ANY_SECONDARY)
                                                             : from,
                                       optional(weapon)};
                if (attachments != "keep")
                {
                    rule.attachments.emplace();
                    std::istringstream list(attachments == "none" ? "" : attachments);
                    for (std::string attachment; std::getline(list, attachment, ',');)
                        rule.attachments->push_back(attachment);
                    std::ranges::sort(*rule.attachments);
                }
                if (camo != "keep")
                    rule.camo = std::stoi(camo);
                std::erase(loadout.weaponRules, Types::WeaponRule{});
                std::erase_if(loadout.weaponRules, [&](const auto& r) { return r.from == rule.from; });
                loadout.weaponRules.push_back(rule);
            }
            Components::Loadouts::SetLoadoutFor(who, loadout);
            Log("LOADOUT set");
        }
        else if (name == "corpses")
        {
            // corpses: logs the drawn corpses, their animation, where they are and the floor under them
            RunOnMainThread([&] {
                const auto entities = Structures::GetEntities();
                const auto cg = Structures::GetClientGlobals();
                if (!entities || !cg || !cg->animScriptData)
                    return;
                for (std::size_t i = 0; i < Structures::MAX_GENTITIES; ++i)
                {
                    const auto& cent = entities[i];
                    if (cent.nextState.eType != Structures::ET_PLAYER_CORPSE || (cent.nextState.eFlags & 0x20))
                        continue;
                    const auto anim = cent.nextState.legsAnim & ~static_cast<std::uint32_t>(Structures::ANIM_TOGGLEBIT);
                    const auto& pos = cent.currentState.pos.trBase;
                    const float start[3] = {pos[0], pos[1], pos[2] + 24.0f};
                    const float end[3] = {pos[0], pos[1], pos[2] - 256.0f};
                    Structures::trace_t trace{};
                    Functions::CG_LocationalTrace(&trace, start, end, static_cast<std::int32_t>(i),
                                                  Structures::MASK_SHOT_NO_CHARACTERS);
                    Log(std::format("CORPSE {} client {} anim {} pos {:.1f} {:.1f} {:.1f} floor {:.1f}", i,
                                    cent.nextState.clientNum,
                                    anim < cg->animScriptData->numAnimations ? cg->animScriptData->animations[anim].name : "?",
                                    pos[0], pos[1], pos[2], start[2] + (end[2] - start[2]) * trace.fraction));
                }
            });
        }
        else if (name == "bones")
        {
            // bones <entity>: logs an entity's models, its animation and time, and every bone's world position and axes;
            // what checks a decoder of the animation data against the game (see AnimationPose.cpp)
            std::int32_t entity = 0;
            args >> entity;
            RunOnMainThread([&] {
                const auto obj = Functions::Com_GetClientDObj(entity, 0);
                const auto entities = Structures::GetEntities();
                if (!obj || !entities)
                {
                    Log("BONES none");
                    return;
                }
                const auto& es = entities[entity].nextState;
                const auto anim = es.legsAnim & ~static_cast<std::uint32_t>(Structures::ANIM_TOGGLEBIT);
                Log(std::format("BONES obj {:x} tree {:x} anims {:x} anim {} time {} models {}",
                                reinterpret_cast<std::uintptr_t>(obj), reinterpret_cast<std::uintptr_t>(obj->tree),
                                reinterpret_cast<std::uintptr_t>(obj->tree ? obj->tree->anims : nullptr), anim,
                                obj->tree ? Functions::XAnimGetTime(obj->tree, anim) : -1.0f, obj->numModels));
                std::uint32_t first = 0;
                for (int m = 0; m < obj->numModels; ++m)
                {
                    const auto model = obj->models[m];
                    Log(std::format("BONES model {} {:x} {} bones {}", m, reinterpret_cast<std::uintptr_t>(model),
                                    model->name, model->numBones));
                    const auto names = *reinterpret_cast<std::uint16_t* const*>(reinterpret_cast<const std::uint8_t*>(model) + 8);
                    std::string line;
                    for (int b = 0; b < model->numBones; ++b)
                    {
                        float axis[3][3], origin[3];
                        if (Functions::CG_DObjGetWorldTagMatrix(Functions::CG_GetPose(0, entity), obj, names[b], axis, origin))
                            line += std::format("{}:{:.3f},{:.3f},{:.3f}|{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f} ",
                                                first + b, origin[0], origin[1], origin[2], axis[0][0], axis[0][1],
                                                axis[0][2], axis[1][0], axis[1][1], axis[1][2], axis[2][0], axis[2][1],
                                                axis[2][2]);
                        if (line.size() > 800)
                        {
                            Log("BONES " + line);
                            line.clear();
                        }
                    }
                    Log("BONES " + line);
                    first += model->numBones;
                }
            });
        }
        else if (name == "pose")
        {
            // pose <animation> <time>: logs the preview's pose of an animation, each bone's parent and position
            std::string animation;
            float time = 0.0f;
            args >> animation >> time;
            const auto& anims = Components::PlayerAnimation::GetAnimations();
            const auto it = std::ranges::find(anims, animation, &std::pair<std::string_view, std::uint32_t>::first);
            const auto pose = it != anims.end() ? Mod::GetGameInterface()->GetAnimationPose(it->second, time) : std::nullopt;
            if (!pose)
            {
                Log("POSE none");
            }
            else
            {
                std::string line;
                for (std::size_t b = 0; b < pose->positions.size(); ++b)
                    line += std::format("{}<{}:{:.1f},{:.1f},{:.1f} ", b, pose->parents[b], pose->positions[b].x,
                                        pose->positions[b].y, pose->positions[b].z);
                Log("POSE " + line);
            }
        }
        else if (name == "trace")
        {
            // trace <x y z> <x y z> [mask [expect clear|hit]]: logs what the client's trace hits
            float start[3], end[3];
            std::int32_t mask = Structures::MASK_SHOT_NO_CHARACTERS;
            std::string word, expected;
            args >> start[0] >> start[1] >> start[2] >> end[0] >> end[1] >> end[2] >> mask >> word >> expected;
            Structures::trace_t result{};
            RunOnMainThread([&] { Functions::CG_LocationalTrace(&result, start, end, Structures::ENTITYNUM_NONE, mask); });
            const auto clear = result.fraction >= 1.0f;
            Log(std::format("{}TRACE fraction {} normal {} {} {} hit {} {:x} surface {} (flags {:X})",
                            word != "expect" ? "" : (clear == (expected == "clear") ? "PASS " : "FAIL "), result.fraction,
                            result.normal[0], result.normal[1], result.normal[2], result.hitType, result.hitId,
                            (result.surfaceFlags >> 20) & 0x1F, result.surfaceFlags));
        }
        else if (name == "matkind")
        {
            // matkind <sorted index> <world|player|always|name>: which passes draw a material
            std::uint32_t index = 0;
            std::string kind;
            args >> index >> kind;
            static const std::map<std::string, int> kinds = {{"world", 0}, {"player", 1}, {"always", 2}, {"name", -1}};
            if (const auto it = kinds.find(kind); it != kinds.end())
                Hooks::Rendering::OverrideMaterialKind(index, it->second);
        }
        else if (name == "drawn")
        {
            // drawn <ms>: logs the names of the materials drawn in the next ms (3D surfaces only)
            std::int32_t length = 1000;
            args >> length;
            Hooks::Rendering::RecordDrawnMaterials(true);
            Sleep(length);
            Hooks::Rendering::RecordDrawnMaterials(false);
            const auto sortedMaterials = reinterpret_cast<const char***>(GetGameAddresses().rgp_sortedMaterials());
            std::string line;
            for (const auto i : Hooks::Rendering::GetDrawnMaterials())
            {
                if (!sortedMaterials[i] || !*sortedMaterials[i])
                    continue;
                line += std::format("{}:{} ", i, *sortedMaterials[i]);
                if (line.size() > 900)
                {
                    Log("DRAWN " + line);
                    line.clear();
                }
            }
            Log("DRAWN " + line);
        }
        else if (name == "kills")
        {
            // waits for the demo's scan and logs what it found
            std::shared_ptr<const Types::DemoEvents> events;
            for (int i = 0; i < 600 && !(events = Mod::GetGameInterface()->GetDemoEvents()); i++)
                Sleep(50);
            if (!events)
            {
                Log("KILLS none");
                return;
            }

            Log(std::format("KILLS {} recorder {} moments {}", events->kills.size(), events->recorder,
                            events->moments.size()));
            for (const auto& [number, player] : events->players)
                Log(std::format("PLAYER {} team {} '{}'", number, player.team, player.name));
            for (const auto& kill : events->kills)
            {
                Log(std::format("KILL tick {} '{}' -> '{}' pov {} method {} weapon '{}' distance {:.0f} air {} tk {}",
                                kill.tick, events->GetPlayerName(kill.attacker), events->GetPlayerName(kill.victim),
                                kill.povClient, static_cast<int>(kill.method), kill.weapon, kill.distance,
                                kill.attackerInAir, kill.teamKill));
            }
            for (const auto& moment : events->moments)
                Log(std::format("MOMENT tick {} type {}", moment.tick, static_cast<int>(moment.type)));
        }
        else if (name == "state")
        {
            const auto cg = Structures::GetClientGlobals();
            Log(std::format("STATE tick {} paused {} rewinding {} landTime {}", Components::Playback::GetTimelineTick(),
                            Components::Playback::IsPaused(),
                            Components::Rewinding::IsRewinding(), cg ? cg->landTime : 0));
        }
        else
        {
            // anything else goes to the console
            Functions::Cbuf_AddText(command);
            Sleep(500);
        }
    }

    void Install()
    {
        char value[MAX_PATH]{};
        if (!GetEnvironmentVariableA("IWXMVM_HARNESS_DIR", value, MAX_PATH))
            return;

        directory = value;
        // (launch.ps1 -ExtraZones: for the Actors tab's lists too)
        if (GetEnvironmentVariableA("IWXMVM_EXTRA_ZONES", value, MAX_PATH))
            Components::Actors::SetExtraZonesFolder(value);
        std::optional<std::size_t> resume;
        if (char line[32]{}; GetEnvironmentVariableA(RESUME_VARIABLE, line, sizeof(line)))
            resume = static_cast<std::size_t>(std::strtoul(line, nullptr, 10));
        std::filesystem::create_directories(directory / "shots");
        logFile.open(directory / "log.txt", resume ? std::ios::app : std::ios::trunc);
        if (resume)
            Log(std::format("INJECTED again, going on from line {} of live.txt", *resume + 1));
        Diagnostics::Install(directory);
        WriteHeartbeats();

        // CL_KeyEvent, for "gamekeys": the jne that CL_KeyEvent_DemoAnyKey finds is 0x3FB into it (Plutonium jumps
        // from its start to a hook of its own, which MinHook takes over)
        const auto keyEvent = GetGameAddresses().CL_KeyEvent_DemoAnyKey() - 0x3FB;
        if (std::memcmp(reinterpret_cast<const void*>(keyEvent), "\x81\xEC\x08\x04\x00\x00", 6) == 0 ||
            *reinterpret_cast<const std::uint8_t*>(keyEvent) == 0xE9)
            HookManager::CreateHook(keyEvent, reinterpret_cast<std::uintptr_t>(&CL_KeyEvent_Hook),
                                    reinterpret_cast<std::uintptr_t*>(&CL_KeyEvent_Trampoline));
        else
            Log(std::format("CL_KeyEvent isn't at {:X}: gamekeys counts nothing", keyEvent));

        Events::RegisterListener(EventType::OnFrame, [] {
            Diagnostics::OnRenderThreadFrame();
            ++renderThreadFrames;
            RecordInputFrame();
            RecordViewLag();
            renderFrameTimes.Record();
            if (tracingView)
            {
                ++renderTraced;
                if (!UI::UIManager::Get().GetUIComponent(UI::Component::GameView)->HasFocus())
                    ++renderUnfocused;
                if (!ImGui::IsKeyDown(ImGuiKey_W))
                    ++renderNoKey;
                if (ImGui::GetIO().DeltaTime <= 0.0001f)
                    ++renderNoTime;
            }
            std::lock_guard lock(renderThreadMutex);
            for (const auto& action : renderThreadActions)
                action();
            renderThreadActions.clear();
        });

        std::thread([module = ReferenceModule(), resume] {
            // lets the mod finish starting
            Sleep(4000);
            CommandSource source(resume);
            for (std::string command; !ejecting && source.Next(command);)
                Run(command);
            if (!ejecting)
            {
                Log("DONE");
                FreeLibraryAndExitThread(module, 0);
            }

            SetEnvironmentVariableA(RESUME_VARIABLE, std::to_string(source.GetNextLive()).c_str());
            Mod::RequestEject();
            // the mod unloads once this thread has ended as well; meanwhile it lets go of what the game holds of it
            // and takes its hooks out (see Mod::Initialize)
            Sleep(3000);
            Diagnostics::Uninstall();
            Log("EJECTED");
            logFile.close();
            FreeLibraryAndExitThread(module, 0);
        }).detach();
    }
}  // namespace IWXMVM::T5::Harness
