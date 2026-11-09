#pragma once
#include "StdInclude.hpp"

#include "D3D9.hpp"
#include "Components/Camera.hpp"
#include "Types/GameState.hpp"
#include "Types/Game.hpp"
#include "Types/DemoInfo.hpp"
#include "Types/MouseMode.hpp"
#include "Types/Dvar.hpp"
#include "Types/MapVisuals.hpp"
#include "Types/Sun.hpp"
#include "Types/Dof.hpp"
#include "Types/Filmtweaks.hpp"
#include "Types/AnimationPose.hpp"
#include "Types/BoneData.hpp"
#include "Types/BoneCamera.hpp"
#include "Types/Entity.hpp"
#include "Types/PlaybackData.hpp"
#include "Types/HudInfo.hpp"
#include "Types/RenderingFlags.hpp"
#include "Types/Features.hpp"
#include "Types/DemoEvents.hpp"
#include "Types/DrawnView.hpp"
#include "Types/WeaponRig.hpp"
#include "Types/LevelAreas.hpp"
#include "Types/MapEffects.hpp"
#include "Types/NormalMap.hpp"
#include "Types/PackImages.hpp"
#include "Utilities/PathUtils.hpp"

namespace IWXMVM
{
    const auto DEMO_TEMP_DIRECTORY = "IWXTMP";

    class GameInterface
    {
       public:
        GameInterface(const Types::Game game) : game(game)
        {
        }
        virtual ~GameInterface() = default;

        Types::Game GetGame() const
        {
            return game;
        }

        // Whether a demo is playing, from its first frame to its last and through rewinds. Pause, freeze and bullet
        // time hold a demo only
        virtual bool IsPlayingDemo()
        {
            return true;
        }

        // Whether this is a live game this process hosts, the only place live actors can be (see
        // Components::LiveActors)
        virtual bool IsHostingLiveGame()
        {
            return false;
        }

        // the client the game runs in when it isn't the game's own executable, for About ("Plutonium T5"); empty when
        // it is
        virtual std::string_view GetClientName()
        {
            return {};
        }

        virtual void ExecuteNewServerCommands() = 0;
        virtual void InstallHooksAndPatches() = 0;
        virtual void SetupEventListeners() = 0;

        // Ejecting (the debug panel's Eject, the harness) unloads the mod while the game goes on. Before the hooks are
        // taken out, the game lets go of what the mod gave it that goes with the mod's memory (models and animations
        // made up for actors, say), on its own threads. Returns once it has, or has had long enough.
        virtual void PrepareUnload()
        {
        }

        // File > Exit: closes the game
        virtual void Quit() = 0;

        virtual IDirect3DDevice9* GetGameDevicePtr() const = 0;
        virtual uintptr_t GetWndProc() = 0;
        virtual void SetMouseMode(Types::MouseMode mode) = 0;
        virtual Types::GameState GetGameState() = 0;

        virtual std::optional<std::span<HMODULE>> GetModuleHandles(
            Types::ModuleType type = Types::ModuleType::BaseModule)
        {
            assert(type != Types::ModuleType::SecondaryModules);

            static std::vector<HMODULE> modules{::GetModuleHandle(nullptr)};
            return std::span{modules};
        }
        virtual void InitializeGameAddresses() = 0;

        virtual Types::Features GetSupportedFeatures()
        {
            return Types::Features_None;
        };

        // The folder the mod keeps its files in (config, keyframes, recordings) and searches for demos by default.
        // Games whose player files live outside the executable's folder override it.
        virtual std::filesystem::path GetUserDirectory()
        {
            return PathUtils::GetCurrentGameDirectory();
        }

        virtual Types::DemoInfo GetDemoInfo() = 0;
        virtual std::string_view GetDemoExtension() = 0;

        // The kills and other events of the demo that is playing (see Features_DemoEvents); null while the demo is
        // still being read
        virtual std::shared_ptr<const Types::DemoEvents> GetDemoEvents()
        {
            return nullptr;
        }

        virtual void PlayDemo(std::filesystem::path demoPath) = 0;
        // why the last PlayDemo sent the game no demo command, empty when it sent one
        virtual std::string GetPlayDemoError()
        {
            return {};
        }
        // whether the game reads a demo it was asked to play, before it plays it (UI::DemoLoadLogic); none when it
        // can't tell
        virtual std::optional<bool> IsReadingDemo()
        {
            return std::nullopt;
        }
        virtual void Disconnect() = 0;
        virtual void Vid_Restart() = 0;

        // Game mods: the fs_game the game runs ("mods/mp_bots", empty for none), none
        // when the game has no mod switch, which leaves the Demo tab without the mods' tags, line and buttons
        virtual std::optional<std::string> GetLoadedFsGame()
        {
            return std::nullopt;
        }
        // the fs_game a demo was recorded with, read from its file on any thread; none when it can't be read
        virtual std::optional<std::string> GetDemoFsGame(const std::filesystem::path& demoPath)
        {
            return std::nullopt;
        }
        // the folders whose folders are the game's mods (see Components::DemoModLogic::FindMods)
        virtual std::vector<std::filesystem::path> GetModFolders()
        {
            return {};
        }
        // switches the game to a mod at the menu, empty to run none; the game reloads its files and renderer
        virtual void LoadMod(std::string_view fsGame)
        {
        }
        // how many frames the game's main thread has run, which it doesn't while it loads; none when unknown
        virtual std::optional<std::uint32_t> GetMainThreadFrames()
        {
            return std::nullopt;
        }

        virtual bool IsConsoleOpen() = 0;

        // Whether the game's own UI (its console, a menu, the chat field) has the keyboard, which takes keys instead of
        // the binds: then keys the editor doesn't use reach the game while the editor is shown (UI::InputRouting)
        virtual bool IsGameUITakingKeys()
        {
            return IsConsoleOpen();
        }

        // The size of the sun's shadow map (each half; Black Ops' 1,024, Features_ShadowQuality), and a new one, which
        // the renderer takes as it restarts: with restart, a restart now (about 0.6 s, the demo staying where it is), else
        // at the next one (a capture's change of render size); none and false in a game without
        virtual std::optional<std::int32_t> GetSunShadowMapSize()
        {
            return std::nullopt;
        }
        virtual bool SetSunShadowMapSize(std::int32_t size, bool restart)
        {
            return false;
        }
        // How many spawns of effects the game refused since the map loaded, all of its effects in use (Black Ops':
        // 1,024); 0 in a game that can't tell
        virtual std::uint32_t CountRefusedEffects()
        {
            return 0;
        }
        // The map's ambient effects that the mod plays (a campaign level's), none on a map
        // whose own scripts play its effects; and those of a level to hide, by definition, the rest shown
        virtual std::optional<Types::MapEffects> GetMapEffects()
        {
            return std::nullopt;
        }
        virtual void SetHiddenMapEffects(const std::string& level, const std::set<std::string>& definitions)
        {
        }
        // The parts of the level to go to, null on a map without them
        virtual std::shared_ptr<const Types::LevelAreas> GetLevelAreas()
        {
            return nullptr;
        }
        // where the demo's or live game's player is
        virtual glm::vec3 GetPlayerPosition()
        {
            return glm::vec3(0.0f);
        }
        // Moves the player of a live game so their eye is there, noclip on so they stay
        virtual void MovePlayer(const glm::vec3& eye, float yaw)
        {
        }
        // The normal maps of the map's surfaces and props (Components::SurfaceDepth), none in
        // a game that can't tell
        virtual std::vector<Types::NormalMap> GetMapNormalMaps()
        {
            return {};
        }
        // The images the game has registered now, with the map's zone, the zones loaded and what uses each image, for a
        // texture pack (Components::TexturePack); none in a game that can't tell, or when its
        // main thread didn't get to it. From any thread but the main one, which it waits for.
        virtual std::optional<Types::PackImages> GetPackImages()
        {
            return std::nullopt;
        }
        // the zones loaded now, as one string that changes when they do (any thread)
        virtual std::string GetLoadedZonesKey()
        {
            return {};
        }

        // TODO: Perhaps dvars shouldnt be exposed to core at all?
        virtual std::optional<Types::Dvar> GetDvar(const std::string_view name) = 0;

        // Required for setting the first person FOV
        // TODO: Perhaps this can be removed and consolidated into how we already set FOV for freecam?
        virtual void SetFov(float fov) = 0;

        // Required for all the settings of the "Visuals" tab
        virtual Types::Sun GetSun() = 0;
        virtual Types::DoF GetDof() = 0;
        virtual Types::Filmtweaks GetFilmtweaks() = 0;
        virtual Types::HudInfo GetHudInfo() = 0;
        virtual void SetSun(Types::Sun) = 0;
        virtual void SetDof(Types::DoF) = 0;
        virtual void SetFilmtweaks(Types::Filmtweaks) = 0;
        virtual void SetHudInfo(Types::HudInfo) = 0;
        // What the map loaded has of them, whatever was changed since (the tab's Reset); nothing in a game that can't
        // say, whose first demo's values stay the defaults
        virtual std::optional<Types::MapVisuals> GetMapVisuals()
        {
            return std::nullopt;
        }

        // Required for BoneCamera
        virtual std::vector<Types::Entity> GetEntities() = 0;
        virtual Types::BoneData GetBoneData(int32_t entityId, const std::string& name) = 0;

        // Where a bone of the bone camera's target is, with where the target stands and faces; nullopt while it isn't
        // there (a player not in the demo now, an actor not drawn) or hasn't the bone. Any thread: games that build
        // frames on another thread than the UI's give the UI the bone as of the frame built last.
        virtual std::optional<Types::TargetBone> GetTargetBone(const Types::BoneCameraTarget& target,
                                                               const std::string& name)
        {
            if (target.kind != Types::BoneCameraTargetKind::Player)
                return std::nullopt;
            const auto bone = GetBoneData(target.id, name);
            if (bone.id == -1)
                return std::nullopt;
            return Types::TargetBone{bone.position, bone.rotation, bone.position,
                                     glm::degrees(std::atan2(bone.rotation[0].y, bone.rotation[0].x))};
        }
        // The names of the bones of the target's models; empty while it isn't there. Any thread.
        // Render thread: where the frame being drawn was drawn from, which the camera has moved on from by then. None
        // for a game that can't tell, or with no view drawn of late.
        virtual std::optional<Types::DrawnView> GetDrawnView()
        {
            return std::nullopt;
        }

        virtual std::vector<std::string> GetTargetBones(const Types::BoneCameraTarget& target)
        {
            return target.kind == Types::BoneCameraTargetKind::Player ? GetSupportedBoneNames()
                                                                      : std::vector<std::string>{};
        }

        // A player model in a death animation (by the game's number, see Components::PlayerAnimation) at a time from 0
        // to 1, for previewing it; nullopt where the game can't tell. Called on the render thread.
        virtual std::optional<Types::AnimationPose> GetAnimationPose(std::uint32_t animation, float time)
        {
            return std::nullopt;
        }
        // A loaded model (by name) in a loaded animation at a time from 0 to 1, as an actor of that model shows it, for
        // previewing actors' animations and working out their reloads (Components::ReloadProfiles); nullopt where the
        // game can't tell, as while it loads a content pack. Any thread.
        virtual std::optional<Types::AnimationPose> GetAnimationPoseByName(std::string_view animation,
                                                                           std::string_view model, float time)
        {
            return std::nullopt;
        }
        // Where the parts of a gun (a weapon's name, as loadouts have it) are on it, from its world model at rest;
        // nullopt for one that isn't loaded, or where the game can't tell (as while it loads a content pack). Any
        // thread, while a demo plays.
        virtual std::optional<Types::WeaponRig> GetWeaponRig(std::string_view weapon)
        {
            return std::nullopt;
        }
        // The names of a loaded model's bones (by name), for the animations that fit it (Components::Actors::GetFits);
        // empty for one that isn't loaded, or where the game can't tell (as while it loads a content pack). Any thread,
        // while a demo plays.
        virtual std::vector<std::string> GetModelBones(std::string_view model)
        {
            return {};
        }
        virtual constexpr std::vector<std::string> GetSupportedBoneNames() = 0;

        // Required for working rewinding
        virtual void CL_FirstSnapshot() = 0;
        virtual void ResetClientData(int serverTime) = 0;
        virtual Types::PlaybackData GetPlaybackDataAddresses() const = 0;

        // Renders at the given size, in a window too, from the next frames on; nullopt goes back to the window's size.
        // Returns false when the game can't.
        virtual bool SetRenderSize(std::optional<std::pair<std::int32_t, std::int32_t>> size)
        {
            return false;
        }

        // Loads a content pack of characters (a name from the actor catalog's packs) in place of the one loaded before,
        // soon, on the game's thread; the catalog tells when it has. Does nothing for a pack that is already loaded.
        virtual void LoadContentPack(const std::string& name)
        {
        }

        // Some games have demo file footers/headers (see IW5)
        virtual uint32_t GetDemoFooterSize() { return 0; }
        virtual uint32_t GetDemoHeaderSize() { return 0; }

       private:
        Types::Game game;
    };
}  // namespace IWXMVM
