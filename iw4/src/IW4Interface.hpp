#pragma once
#include "StdInclude.hpp"
#include "GameInterface.hpp"

#include "Addresses.hpp"
#include "DemoParser.hpp"
#include "Events.hpp"
#include "Functions.hpp"
#include "Hooks.hpp"
#include "Hooks/Camera.hpp"
#include "Hooks/Playback.hpp"
#include "Patches.hpp"
#include "Structures.hpp"

#include "Components/CameraManager.hpp"
#include "Components/Rewinding.hpp"
#include "Utilities/PathUtils.hpp"

#include "glm/gtc/type_ptr.hpp"
#include "glm/vec3.hpp"

namespace IWXMVM::IW4
{
    // Modern Warfare 2 (2009) multiplayer, build 177, as the IW4M client starts it (iw4m.exe + iw4m.dll).
    class IW4Interface : public GameInterface
    {
       public:
        IW4Interface() : GameInterface(Types::Game::IW4)
        {
        }

        void InstallHooksAndPatches() final
        {
            Hooks::Install();
            Patches::GetGamePatches();
        }

        void SetupEventListeners() final
        {
            Events::RegisterListener(EventType::PostDemoLoad, DemoParser::Run);

            Events::RegisterListener(EventType::OnCameraChanged, Hooks::Camera::OnCameraChanged);
            Events::RegisterListener(EventType::OnRewindCompleted, Hooks::Camera::OnCameraChanged);

            Events::RegisterListener(EventType::PostDemoLoad, [&]() {
                // the kill feed's own settings, so that the toggle for it starts from the game's
                if (const auto dvar = Functions::TryFindDvar("con_gameMsgWindow0MsgTime"))
                    dvar->current.value = dvar->reset.value;
            });
        }

        void Quit() final
        {
            Functions::Cbuf_AddText("quit");
        }

        IDirect3DDevice9* GetGameDevicePtr() const final
        {
            return *reinterpret_cast<IDirect3DDevice9**>(Addresses::d3d9Device);
        }

        uintptr_t GetWndProc() final
        {
            return Addresses::MainWndProc;
        }

        void SetMouseMode(Types::MouseMode mode) final
        {
            if (mode == Types::MouseMode::Capture)
                Patches::GetGamePatches().IN_Frame.Apply();
            else
                Patches::GetGamePatches().IN_Frame.Revert();
        }

        Types::GameState GetGameState() final
        {
            const auto inGame = Functions::TryFindDvar("cl_ingame");
            if (!inGame || !inGame->current.enabled)
                return Types::GameState::MainMenu;

            if (Structures::GetClientConnection()->demoplaying)
                return Types::GameState::InDemo;

            return Types::GameState::InGame;
        }

        bool IsPlayingDemo() final
        {
            return Structures::GetClientConnection()->demoplaying != 0;
        }

        Types::Features GetSupportedFeatures() final
        {
            return Types::Features_None;
        }

        void InitializeGameAddresses() final
        {
            // one build only (see Addresses.hpp): what must be true of it is checked where each hook goes in
        }

        std::optional<std::span<HMODULE>> GetModuleHandles(Types::ModuleType type = Types::ModuleType::BaseModule) final
        {
            static std::vector<HMODULE> modules = []() { return std::vector<HMODULE>{::GetModuleHandle(nullptr)}; }();

            if (type == Types::ModuleType::BaseModule)
                return std::span{modules.begin(), modules.begin() + 1};

            return std::nullopt;
        }

        Types::DemoInfo GetDemoInfo() final
        {
            const std::string demoName = Structures::GetClientConnection()->demoName;
            const std::string tempDirectoryPrefix = std::string(DEMO_TEMP_DIRECTORY) + "/";
            demoInfo.name =
                demoName.starts_with(tempDirectoryPrefix) ? demoName.substr(tempDirectoryPrefix.size()) : demoName;

            // this gets called every frame, so avoid hitting the file system each time
            if (demoName != cachedDemoName || cachedDemoPath.empty())
            {
                cachedDemoName = demoName;
                cachedDemoPath = Functions::GetFilePath(demoName);
            }
            demoInfo.path = cachedDemoPath;

            const auto [demoStartTick, demoEndTick] = DemoParser::GetDemoTickRange();
            const auto serverTime = Structures::GetClientActive()->serverTime;
            if (serverTime > demoStartTick && serverTime < demoEndTick && !Components::Rewinding::IsRewinding())
                demoInfo.gameTick = static_cast<std::uint32_t>(serverTime - demoStartTick);
            demoInfo.endTick = static_cast<std::uint32_t>(demoEndTick - demoStartTick);

            return demoInfo;
        }

        std::string_view GetDemoExtension() final
        {
            return {".dm_13"};
        }

        // On the render thread (the Demo tab's PLAY). PreDemoLoad and PostDemoLoad are raised around the game's own
        // demo command, on the main thread (Hooks::Playback).
        void PlayDemo(std::filesystem::path demoPath) final
        {
            playDemoError.clear();
            try
            {
                LOG_INFO("Playing demo {0}", demoPath.string());

                if (!std::filesystem::exists(demoPath) || !std::filesystem::is_regular_file(demoPath))
                {
                    playDemoError = "the file isn't there";
                    return;
                }

                const auto base = Functions::GetFirstSearchDirectory();
                if (base.empty())
                {
                    playDemoError = "the game's folders couldn't be read";
                    return;
                }

                // a demo that is in one of the game's own demos folders is played from there, by its name
                std::string name = demoPath.filename().string();
                if (name.ends_with(".dm_13"))
                    name.resize(name.size() - 6);
                std::error_code error;
                const auto known = Functions::GetFilePath(name);
                if (!known.empty() && std::filesystem::equivalent(known, demoPath, error))
                {
                    Functions::Cbuf_AddText(std::format(R"(demo "{0}")", name));
                    return;
                }

                // one from elsewhere is copied where the game looks
                const auto tempDemoDirectory = base / "demos" / DEMO_TEMP_DIRECTORY;
                if (!std::filesystem::exists(tempDemoDirectory))
                    std::filesystem::create_directories(tempDemoDirectory);

                const auto targetPath = tempDemoDirectory / demoPath.filename();
                if (std::filesystem::exists(targetPath) && std::filesystem::is_regular_file(targetPath))
                    std::filesystem::remove(targetPath);

                std::filesystem::copy(demoPath, targetPath);

                Functions::Cbuf_AddText(std::format(R"(demo "{0}/{1}")", DEMO_TEMP_DIRECTORY, name));
            }
            catch (std::filesystem::filesystem_error& e)
            {
                playDemoError = e.what();
                LOG_ERROR("Failed to play demo file {0}: {1}", demoPath.string(), e.what());
            }
        }

        std::string GetPlayDemoError() final
        {
            return playDemoError;
        }

        void Disconnect() final
        {
            Functions::Cbuf_AddText("disconnect");
        }

        void Vid_Restart() final
        {
            Functions::Cbuf_AddText("vid_restart");
        }

        bool IsConsoleOpen() final
        {
            return (Structures::GetClientUIActive()->keyCatchers & Structures::KEYCATCH_CONSOLE) != 0;
        }

        // The console, a menu (the main menu has one, a playing demo none) and the chat take the keys from the binds
        bool IsGameUITakingKeys() final
        {
            constexpr int TAKING = Structures::KEYCATCH_CONSOLE | Structures::KEYCATCH_UI | Structures::KEYCATCH_CHAT;
            return (Structures::GetClientUIActive()->keyCatchers & TAKING) != 0;
        }

        std::optional<Types::Dvar> GetDvar(const std::string_view name) final
        {
            const std::string text{name};
            const auto gameDvar = Functions::TryFindDvar(text.c_str());
            if (!gameDvar)
                return std::nullopt;

            Types::Dvar dvar;
            dvar.name = gameDvar->name;
            dvar.value = reinterpret_cast<Types::Dvar::Value*>(&gameDvar->current);
            return dvar;
        }

        void SetFov(float fov) final
        {
            Functions::SetFloat("cg_fov", fov);
        }

        Types::Sun GetSun() final
        {
            const auto cg = Structures::GetClientGlobals();
            for (auto& light : std::span{cg->refdef.primaryLights})
            {
                if (light.type == Structures::GFX_LIGHT_TYPE_DIR)
                    return Types::Sun{glm::make_vec3(light.color), glm::degrees(glm::make_vec3(light.dir)), 1.0f};
            }
            return {};
        }

        void SetSun(Types::Sun sun) final
        {
            const auto cg = Structures::GetClientGlobals();
            for (auto& light : std::span{cg->refdef.primaryLights})
            {
                if (light.type != Structures::GFX_LIGHT_TYPE_DIR)
                    continue;

                for (int i = 0; i < 3; ++i)
                {
                    light.color[i] = sun.color[i];
                    light.dir[i] = glm::radians(sun.direction[i]);
                }
            }
        }

        Types::DoF GetDof() final
        {
            return Types::DoF{
                Functions::GetBool("r_dof_tweak") && Functions::GetBool("r_dof_enable"),
                Functions::GetFloat("r_dof_farBlur"),
                Functions::GetFloat("r_dof_farStart"),
                Functions::GetFloat("r_dof_farEnd"),
                Functions::GetFloat("r_dof_nearBlur"),
                Functions::GetFloat("r_dof_nearStart"),
                Functions::GetFloat("r_dof_nearEnd"),
                Functions::GetFloat("r_dof_bias"),
            };
        }

        void SetDof(Types::DoF dof) final
        {
            Functions::SetBool("r_dof_tweak", dof.enabled);
            Functions::SetBool("r_dof_enable", dof.enabled);

            Functions::SetFloat("r_dof_farBlur", dof.farBlur);
            Functions::SetFloat("r_dof_farStart", dof.farStart);
            Functions::SetFloat("r_dof_farEnd", dof.farEnd);

            // (as the other games' modules: under this the near blur draws wrongly)
            if (dof.nearBlur < 1.3f)
            {
                dof.nearBlur = 5;
                dof.nearStart = 0;
                dof.nearEnd = 0;
            }

            Functions::SetFloat("r_dof_nearBlur", dof.nearBlur);
            Functions::SetFloat("r_dof_nearStart", dof.nearStart);
            Functions::SetFloat("r_dof_nearEnd", dof.nearEnd);
            Functions::SetFloat("r_dof_bias", dof.bias);
        }

        Types::Filmtweaks GetFilmtweaks() final
        {
            Types::Filmtweaks filmtweaks{};
            filmtweaks.enabled = Functions::GetBool("r_filmUseTweaks") && Functions::GetBool("r_filmTweakEnable");
            filmtweaks.brightness = Functions::GetFloat("r_filmTweakBrightness");
            filmtweaks.contrast = Functions::GetFloat("r_filmTweakContrast", 1.0f);
            filmtweaks.desaturation = Functions::GetFloat("r_filmTweakDesaturation");
            filmtweaks.tintLight = GetVec3("r_filmTweakLightTint", glm::vec3(1.0f));
            filmtweaks.tintDark = GetVec3("r_filmTweakDarkTint", glm::vec3(1.0f));
            filmtweaks.invert = Functions::GetBool("r_filmTweakInvert");
            return filmtweaks;
        }

        void SetFilmtweaks(Types::Filmtweaks filmtweaks) final
        {
            Functions::SetBool("r_filmUseTweaks", filmtweaks.enabled);
            Functions::SetBool("r_filmTweakEnable", filmtweaks.enabled);
            Functions::SetFloat("r_filmTweakBrightness", filmtweaks.brightness);
            Functions::SetFloat("r_filmTweakContrast", filmtweaks.contrast);
            Functions::SetFloat("r_filmTweakDesaturation", filmtweaks.desaturation);
            SetVec3("r_filmTweakLightTint", filmtweaks.tintLight);
            SetVec3("r_filmTweakDarkTint", filmtweaks.tintDark);
            Functions::SetBool("r_filmTweakInvert", filmtweaks.invert);
        }

        Types::HudInfo GetHudInfo() final
        {
            return Types::HudInfo{
                Functions::GetBool("cg_draw2D", true),
                !Functions::GetBool("ui_hud_hardcore"),
                Functions::GetBool("cg_drawShellshock", true),
                Functions::GetBool("ui_drawCrosshair", true),
                showScore,
                showIconsAndText,
                Functions::GetBool("cg_drawDamageDirection", true),
                Functions::GetFloat("con_gameMsgWindow0MsgTime", 5.0f) > 0.0f,
                GetColor("g_TeamColor_Allies"),
                GetColor("g_TeamColor_Axis"),
            };
        }

        void SetHudInfo(Types::HudInfo hudInfo) final
        {
            Functions::SetBool("cg_draw2D", hudInfo.show2DElements);

            if (!hudInfo.show2DElements)
            {
                hudInfo.showPlayerHUD = false;
                hudInfo.showShellshock = false;
                hudInfo.showCrosshair = false;
                hudInfo.showScore = false;
                hudInfo.showIconsAndText = false;
                hudInfo.showBloodOverlay = false;
                hudInfo.showKillfeed = false;
            }

            // (each goes back to the game's own value, which dvar_t::reset has)
            Functions::SetBool("ui_hud_hardcore", !hudInfo.showPlayerHUD);
            SetFloatOrDefault("cg_centertime", hudInfo.showPlayerHUD, 0.0f);
            SetFloatOrDefault("cg_overheadRankSize", hudInfo.showPlayerHUD, 0.0f);
            SetFloatOrDefault("cg_overheadNamesSize", hudInfo.showPlayerHUD, 0.0f);
            SetFloatOrDefault("cg_overheadIconSize", hudInfo.showPlayerHUD, 0.0f);

            Functions::SetBool("cg_drawShellshock", hudInfo.showShellshock);
            Functions::SetBool("ui_drawCrosshair", hudInfo.showCrosshair);
            Functions::SetBool("cg_drawCrosshair", hudInfo.showCrosshair);

            showScore = hudInfo.showScore;
            showIconsAndText = hudInfo.showIconsAndText;

            Functions::SetBool("cg_drawDamageDirection", hudInfo.showBloodOverlay);
            Functions::SetBool("cg_drawDamageFlash", hudInfo.showBloodOverlay);
            SetFloatOrDefault("painVisionTriggerHealth", hudInfo.showBloodOverlay, 0.0f);

            SetFloatOrDefault("con_gameMsgWindow0MsgTime", hudInfo.showKillfeed, 0.0f);

            Functions::Dvar_SetStringByName(
                "g_TeamColor_Allies", std::format("{} {} {} 1", hudInfo.killfeedTeam1Color[0],
                                                  hudInfo.killfeedTeam1Color[1], hudInfo.killfeedTeam1Color[2])
                                          .c_str());
            Functions::Dvar_SetStringByName(
                "g_TeamColor_Axis", std::format("{} {} {} 1", hudInfo.killfeedTeam2Color[0],
                                                hudInfo.killfeedTeam2Color[1], hudInfo.killfeedTeam2Color[2])
                                        .c_str());
        }

        std::vector<Types::Entity> GetEntities() final
        {
            std::vector<Types::Entity> entities;
            entities.reserve(Structures::MAX_GENTITIES);

            const auto cg_entities = Structures::GetEntities();
            for (int i = 0; i < static_cast<int>(Structures::MAX_GENTITIES); i++)
            {
                const auto& cent = cg_entities[i].entity;
                entities.push_back(Types::Entity{
                    .id = i,
                    .type = ToEntityType(cent.nextState.eType),
                    .clientNum = cent.nextState.clientNum,
                    .isValid = cent.nextValid != 0,
                });
            }

            return entities;
        }

        Types::BoneData GetBoneData(int32_t entityId, const std::string& name) final
        {
            if (entityId < 0 || entityId >= static_cast<int32_t>(Structures::MAX_GENTITIES))
                return {.id = -1};

            const auto dobj = Functions::Com_GetClientDObj(entityId);
            if (!dobj)
                return {.id = -1};

            // (a name no loaded model has is in no string table: nothing to look for)
            const auto tagName = Functions::SL_FindString(name.c_str());
            if (!tagName)
                return {.id = -1};

            const auto entity = &Structures::GetEntities()[entityId].entity;

            // As the other games' modules: the skeleton as it was last worked out is read, not worked out anew
            // (that is the game's own business, at its own moment in the frame).
            const auto orgTimeStamp = std::exchange(dobj->skel.timeStamp, Structures::GetClientStatic()->skelTimeStamp);

            float axis[3][3]{};
            float origin[3]{};
            const auto result = Functions::CG_DObjGetWorldTagMatrix(&entity->pose, dobj, tagName, axis, origin);
            dobj->skel.timeStamp = orgTimeStamp;

            if (!result)
                return {.id = -1};

            Types::BoneData boneData;
            boneData.id = static_cast<int32_t>(tagName);
            boneData.position = glm::make_vec3(origin);
            boneData.rotation = glm::make_mat3(&axis[0][0]);
            return boneData;
        }

        constexpr std::vector<std::string> GetSupportedBoneNames() final
        {
            return {"tag_weapon_right", "tag_weapon_left", "tag_flash",     "tag_eye",       "j_head",
                    "j_helmet",         "j_neck",          "j_spine4",      "j_mainroot",    "j_wrist_le",
                    "j_wrist_ri",       "j_shoulder_le",   "j_shoulder_ri", "j_elbow_le",    "j_elbow_ri",
                    "j_knee_le",        "j_knee_ri",       "j_ankle_le",    "j_ankle_ri",    "tag_origin"};
        }

        // CL_FirstSnapshot takes the client's number in esi; its call of Con_TimeJumped is left out for a rewind
        void CL_FirstSnapshot() final
        {
            const std::uintptr_t address = Addresses::CL_FirstSnapshot;

            Patches::GetGamePatches().Con_TimeJumped.Apply();
            _asm
            {
                pushad
                xor esi, esi
                call address
                popad
            }
            Patches::GetGamePatches().Con_TimeJumped.Revert();
        }

        void ResetClientData(int serverTime) final
        {
            const auto cl = Structures::GetClientActive();
            for (auto& snapshot : std::span{cl->snapshots})
                snapshot.valid = 0;

            cl->snap.serverTime = serverTime;
            cl->serverTime = 0;
            cl->oldServerTime = 0;
            cl->oldFrameServerTime = 0;
            cl->serverTimeDelta = 0;

            const auto cls = Structures::GetClientStatic();
            cls->realtime = 0;

            const auto cgs = Structures::GetClientGlobalsStatic();
            cgs->processedSnapshotNum = 0;

            const auto cg = Structures::GetClientGlobals();
            cg->latestSnapshotNum = 0;
            cg->latestSnapshotTime = 0;
            cg->snap = nullptr;
            cg->nextSnap = nullptr;
            cg->landTime = 0;
        }

        Types::PlaybackData GetPlaybackDataAddresses() const final
        {
            const auto cl = Structures::GetClientActive();
            const auto clc = Structures::GetClientConnection();
            const auto cgs = Structures::GetClientGlobalsStatic();
            const auto cls = Structures::GetClientStatic();
            const auto con = Structures::GetConsole();

            // (not found in this game yet: core clears what it is given, so it is given something of the editor's)
            static std::uint32_t noCompassActors = 0;

            return Types::PlaybackData{
                .cl =
                    {
                        .snap_serverTime = reinterpret_cast<uintptr_t>(&cl->snap.serverTime),
                        .serverTime = reinterpret_cast<uintptr_t>(&cl->serverTime),
                        .parseEntitiesNum = reinterpret_cast<uintptr_t>(&cl->parseEntitiesIndex),
                        .parseClientsNum = reinterpret_cast<uintptr_t>(&cl->parseClientsIndex),
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
                        .serverCommandSequence = reinterpret_cast<uintptr_t>(&cgs->serverCommandSequence),
                    },
                .cls =
                    {
                        .realtime = reinterpret_cast<uintptr_t>(&cls->realtime),
                    },
                .s_compassActors = {.address = reinterpret_cast<uintptr_t>(&noCompassActors),
                                    .size = sizeof(noCompassActors)},
                .teamChatMsgs = {.address = reinterpret_cast<uintptr_t>(&cgs->teamChatMsgs),
                                 .size = sizeof(cgs->teamChatMsgs)},
                .cg_entities = {.address = reinterpret_cast<uintptr_t>(Structures::GetEntities()),
                                .size = Structures::MAX_GENTITIES * sizeof(Structures::cgEntity_s)},
                .clientInfo = {.address = reinterpret_cast<uintptr_t>(Structures::GetClientInfo()),
                               .size = Structures::MAX_CLIENTS * sizeof(Structures::clientInfo_t)},
                .gameState = {.address = reinterpret_cast<uintptr_t>(Structures::GetGameState()),
                              .size = sizeof(Structures::gameState_t)},
                .killfeed = reinterpret_cast<uintptr_t>(&con->messageBuffer[0].gamemsgWindows[0].activeLineCount),
            };
        }

        // Called before each demo message is read. When the game has fallen far behind what it has been sent (a
        // long skip forward), the commands that would be lost are run now, as the other games' modules do.
        void ExecuteNewServerCommands() final
        {
            const auto clc = Structures::GetClientConnection();
            const auto cgs = Structures::GetClientGlobalsStatic();

            const auto oldServerCommandSequence = cgs->serverCommandSequence;
            const auto newServerCommandSequence = clc->serverCommandSequence;

            // half of the 128 the game keeps
            if (oldServerCommandSequence > 0 &&
                oldServerCommandSequence + std::ssize(clc->serverCommands) / 2 <= newServerCommandSequence)
            {
                Functions::CG_ExecuteNewServerCommands(newServerCommandSequence);
            }
        }

        // The first message of a demo is its gamestate (the map's load); one in the middle of a demo (a round
        // beginning again) is told by its size: about 50,000 bytes in the user's demos, where a demo's first, whole
        // snapshot was 10,290 (core's own rule, 10,000, took that one for a gamestate).
        std::optional<bool> IsDemoGamestateMessage(std::uint32_t bodyOffset, int length) final
        {
            constexpr int GAMESTATE_AT_LEAST = 30'000;
            return bodyOffset == Hooks::Playback::GetMessageHeaderSize() || length >= GAMESTATE_AT_LEAST;
        }

        std::uint32_t GetDemoMessageHeaderSize() final
        {
            return Hooks::Playback::GetMessageHeaderSize();
        }

       private:
        static Types::EntityType ToEntityType(int eType)
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

        static glm::vec3 GetVec3(const char* name, glm::vec3 fallback)
        {
            const auto dvar = Functions::FindDvar(name);
            return dvar ? glm::make_vec3(dvar->current.vector) : fallback;
        }

        static void SetVec3(const char* name, const glm::vec3& value)
        {
            if (const auto dvar = Functions::FindDvar(name))
                for (int i = 0; i < 3; ++i)
                    dvar->current.vector[i] = value[i];
        }

        // A team's colour, whichever way the game keeps it (four bytes or floats)
        static glm::vec3 GetColor(const char* name)
        {
            const auto dvar = Functions::FindDvar(name);
            if (!dvar)
                return glm::vec3(1.0f);

            if (dvar->type == Structures::DVAR_TYPE_COLOR)
                return glm::vec3(dvar->current.color[0], dvar->current.color[1], dvar->current.color[2]) / 255.0f;

            return glm::make_vec3(dvar->current.vector);
        }

        // The game's own value when shown, else the one that hides it
        static void SetFloatOrDefault(const char* name, bool shown, float hidden)
        {
            if (const auto dvar = Functions::FindDvar(name))
                dvar->current.value = shown ? dvar->reset.value : hidden;
        }

        Types::DemoInfo demoInfo{};
        std::string cachedDemoName;
        std::string cachedDemoPath;
        std::string playDemoError;
        bool showScore = true;
        bool showIconsAndText = true;
    };
}  // namespace IWXMVM::IW4
