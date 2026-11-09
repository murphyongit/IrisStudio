#include "StdInclude.hpp"
#include "Camera.hpp"

#include "Mod.hpp"
#include "Events.hpp"
#include "Components/BoneCamera.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/Playback.hpp"
#include "Components/Rewinding.hpp"
#include <deque>
#include <mutex>
#include "Utilities/HookManager.hpp"
#include "Utilities/MathUtils.hpp"
#include "Utilities/Patches.hpp"
#include "../Structures.hpp"
#include "../Functions.hpp"
#include "../Addresses.hpp"
#include "../Actors.hpp"
#include "../Bones.hpp"
#include "HUD.hpp"
#include "Loadouts.hpp"

namespace IWXMVM::T5::Hooks::Camera
{
    // T5 builds frames on its main thread, but the UI, and with it the camera, is updated on the render thread.
    // Reading the camera while the render thread may be moving it gives some frames two camera updates and others
    // none, which makes the free camera stutter. Frames are built from a copy instead, taken right after the main
    // thread has waited for the render thread to finish its frame.
    struct CameraState
    {
        Components::Camera::Mode mode = {};
        bool isModControlled = false;
        glm::vec3 position = {};
        glm::vec3 rotation = {};
        float fov = 0;
        std::optional<Types::BoneCameraSettings> bone;
    };
    std::optional<CameraState> cameraState;
    std::atomic<DWORD> mainThreadId = 0;

    // the copy the current frame is built from, so everything in the frame agrees on the camera
    CameraState frameState;
    std::optional<bool> frameThirdPerson;

    CameraState GetCameraState()
    {
        auto& camera = Components::CameraManager::Get().GetActiveCamera();
        CameraState state{camera->GetMode(), camera->IsModControlledCameraMode(), camera->GetPosition(),
                          camera->GetRotation(), camera->GetFov()};

        if (state.mode == Components::Camera::Mode::Bone)
            state.bone = static_cast<Components::BoneCamera&>(*camera).GetSettings();

        return state;
    }

    bool IsMainThread()
    {
        return GetCurrentThreadId() == mainThreadId;
    }

    double NowMs()
    {
        static const double frequency = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart);
        }();
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart) * 1000.0 / frequency;
    }

    // The free and orbit cameras move once per rendered frame, by that frame's duration as the render thread measures
    // it, but the main thread starts its frames at other moments. A copy of the camera then holds one render frame's
    // worth of movement in one frame and more or less in the next, so the camera speeds up and slows down from frame
    // to frame (by up to a quarter at 60 fps) while the world around it moves evenly. For these cameras every update
    // is recorded with its time, and each frame takes the camera as it was a fixed delay before the frame started,
    // interpolated between updates, which moves it on the same clock the game moves the world with. Cameras that
    // follow the demo (dolly, bone) are placed from the frame itself instead (see below), and capturing, which has to
    // stay frame-exact, uses the copy as it is.
    namespace Resampling
    {
        struct Sample
        {
            double time;
            CameraState state;
        };

        constexpr std::size_t MAX_SAMPLES = 64;
        // stay this far behind the newest update, so there is almost always an update on both sides
        constexpr double MARGIN_MS = 1.0;
        // how fast the delay shrinks again after a slow frame, small enough to be invisible
        constexpr double DELAY_DECAY_MS = 0.05;
        // A main thread stall (loading a content pack takes seconds) would otherwise leave a delay that takes minutes
        // to shrink, with the camera lagging behind its input and jumping between the few updates still kept
        constexpr double MAX_DELAY_MS = 50.0;

        std::mutex mutex;
        std::deque<Sample> samples;
        double delay = 0;

        bool AppliesTo(const CameraState& state)
        {
            return (state.mode == Components::Camera::Mode::Free || state.mode == Components::Camera::Mode::Orbit) &&
                   !Components::CaptureManager::Get().IsCapturing();
        }

        // render thread, after the camera has been updated for the frame
        void AddSample()
        {
            std::lock_guard lock(mutex);
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            {
                samples.clear();
                return;
            }

            samples.push_back({NowMs(), GetCameraState()});
            if (samples.size() > MAX_SAMPLES)
                samples.pop_front();
        }

        float LerpAngle(float from, float to, float t)
        {
            return from + std::remainder(to - from, 360.0f) * t;
        }

        // main thread, at the start of a frame
        void Apply(CameraState& state)
        {
            const auto now = NowMs();
            std::lock_guard lock(mutex);

            auto newest = std::find_if(samples.rbegin(), samples.rend(),
                                       [&](const Sample& sample) { return sample.state.mode == state.mode; });
            if (newest == samples.rend())
                return;

            delay = std::min(std::max(now - newest->time + MARGIN_MS, delay - DELAY_DECAY_MS), MAX_DELAY_MS);
            const auto target = now - delay;

            // walk back through this camera's updates to the two around the target
            auto later = newest;
            for (auto earlier = std::next(later);
                 earlier != samples.rend() && earlier->state.mode == state.mode && later->time > target;
                 later = earlier++)
            {
                if (earlier->time > target)
                    continue;

                const auto t = static_cast<float>((target - earlier->time) / (later->time - earlier->time));
                const auto& from = earlier->state;
                const auto& to = later->state;
                state.position = glm::mix(from.position, to.position, t);
                for (int i = 0; i < 3; i++)
                    state.rotation[i] = LerpAngle(from.rotation[i], to.rotation[i], t);
                state.fov = glm::mix(from.fov, to.fov, t);
                return;
            }

            // the camera was selected too recently to have an update from before the target
            state.position = later->state.position;
            state.rotation = later->state.rotation;
            state.fov = later->state.fov;
        }
    }  // namespace Resampling

    // The dolly camera follows the campath on the render thread, at the tick the main thread had reached at that moment,
    // and the main thread only takes that position over when its next frame starts. Measured while capturing at 125 fps,
    // the camera was 1 to 3 frames behind the frame it was drawn in, changing from frame to frame, so campaths stuttered
    // while the world moved evenly. The campath is evaluated here instead, at the frame's own tick, from a copy of the
    // keyframes taken while the render thread is idle, as the UI may be editing them.
    std::vector<Types::Keyframe> campathKeyframes;

    void CopyCampath()
    {
        auto& keyframeManager = Components::KeyframeManager::Get();
        campathKeyframes =
            keyframeManager.GetKeyframes(keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera));
    }

    std::optional<glm::vec3> GetDollyPosition(std::uint32_t timelineTick)
    {
        if (campathKeyframes.empty())
            return std::nullopt;
        auto& keyframeManager = Components::KeyframeManager::Get();
        const auto& property = keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        return keyframeManager.Interpolate(property, campathKeyframes, timelineTick).cameraData.position;
    }

    void ApplyDollyCamera(CameraState& state)
    {
        if (campathKeyframes.empty() || Components::Rewinding::IsRewinding())
            return;

        auto& keyframeManager = Components::KeyframeManager::Get();
        const auto& property = keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        const auto value =
            keyframeManager.Interpolate(property, campathKeyframes, Components::Playback::GetTimelineTick());

        state.position = value.cameraData.position;
        state.rotation = value.cameraData.rotation;
        state.fov = value.cameraData.fov;
    }

    // The bone camera follows its bone on the render thread, from the bones of the frame before, and the main thread
    // would only take that position over in the frame after the next, which left the camera trailing the model it is
    // attached to. Reading the bone while the view is built doesn't do either: the game works out a DObj's skeleton
    // once a frame, and the frame it draws the models in starts after the view is built (an actor posed for the frame's
    // tick there still gave the frame before's bones, 1.5 units behind a running one). So the camera is placed again
    // once the frame's players and actors are posed and added (CG_AddPacketEntities), from their bones as they are
    // drawn, and the view built earlier in the frame (from the render thread's camera) is moved there before the scene
    // is rendered: the camera is on the bone as drawn, at every tick a recording takes. A player that died is followed
    // through its corpse. The sway is steadied here (BoneCameraLogic::Steady), by the ticks of what it follows, from
    // the bone as it is when a recording starts, so a recording comes out the same each time.
    Components::BoneCameraLogic::Steady steady;
    bool wasCapturing = false;
    // Slowed down, the bone moves only on the frames the game's clock ticks (BoneCameraLogic::Glide). A recording's frames
    // all tick, whatever the speed, so it stays on the bone as drawn.
    Components::BoneCameraLogic::Glide glide;

    // Its keys (the offset, rotation and FOV) are applied here too, at the frame's own
    // timeline tick, from a copy taken as the campath's is: the UI's settings have them at the tick the UI had when
    // they were copied, which is 1 to 3 frames behind (as the dolly was), so a recording would judder.
    Components::BoneCameraLogic::Keys boneCameraKeys;

    void CopyBoneCameraKeys()
    {
        boneCameraKeys = Components::BoneCamera::CopyKeys();
    }

    std::atomic<bool> glideOn = true;

    void SetBoneCameraGlide(bool on)
    {
        glideOn = on;
    }

    // the latest frames' placements, for the harness (GetBoneCameraFrames)
    constexpr std::size_t KEPT_FRAMES = 512;
    std::mutex framesMutex;
    std::deque<BoneCameraFrame> frames;

    std::vector<BoneCameraFrame> GetBoneCameraFrames()
    {
        std::lock_guard lock(framesMutex);
        return {frames.begin(), frames.end()};
    }

    namespace
    {
        constexpr std::size_t VIEW_MATRIX = 0x0;
        constexpr std::size_t VIEW_PROJECTION_MATRIX = 0x40;
        constexpr std::size_t VIEW_VIEW_PROJECTION_MATRIX = 0x80;
        constexpr std::size_t VIEW_ORIGIN = 0x100;
        constexpr double DRAWN_VIEW_KEPT_MS = 500.0;

        std::optional<DrawnView> drawnView;
    }  // namespace

    void OnViewDrawn(const std::uint8_t* viewInfo)
    {
        const auto matrix = [&](std::size_t offset) {
            return glm::make_mat4(reinterpret_cast<const float*>(viewInfo + offset));
        };
        DrawnView view{matrix(VIEW_MATRIX), matrix(VIEW_PROJECTION_MATRIX), matrix(VIEW_VIEW_PROJECTION_MATRIX),
                       glm::make_vec3(reinterpret_cast<const float*>(viewInfo + VIEW_ORIGIN)), NowMs()};
        view.views = drawnView && view.time - drawnView->time < 1.0 ? drawnView->views + 1 : 1;
        drawnView = view;
    }

    std::optional<DrawnView> GetDrawnMatrices()
    {
        return drawnView;
    }

    std::optional<Types::DrawnView> GetDrawnView()
    {
        // (none of late: a frame with no view of the world, as during a load)
        if (!drawnView || NowMs() - drawnView->time > DRAWN_VIEW_KEPT_MS)
            return std::nullopt;
        const auto& view = drawnView->view;
        return Types::DrawnView{drawnView->origin, glm::vec3(view[0][2], view[1][2], view[2][2]),
                                glm::vec3(view[0][1], view[1][1], view[2][1]),
                                glm::degrees(std::atan(1.0f / drawnView->projection[0][0]) * 2.0f),
                                drawnView->projection[2][2], drawnView->projection[3][2]};
    }

    // the view the frame before was rendered from, as it is when the next frame starts to build its own
    void RecordRenderedView()
    {
        const auto cg = Structures::GetClientGlobals();
        std::lock_guard lock(framesMutex);
        if (cg && !frames.empty() && !frames.back().rendered)
            frames.back().rendered = glm::make_vec3(cg->refdef.vieworg);
    }

    void ApplyBoneCamera(CameraState& state)
    {
        const auto timelineTick = Components::Playback::GetTimelineTick();
        const auto settings = Components::BoneCameraLogic::ApplyKeys(*state.bone, boneCameraKeys, timelineTick);
        const bool actor = settings.target.kind == Types::BoneCameraTargetKind::Actor;
        const auto bone = actor ? Actors::GetDrawnBone(static_cast<std::uint32_t>(settings.target.id), settings.bone)
                                : Bones::GetTargetBone(settings.target, settings.bone);
        const auto tick =
            actor ? Components::Playback::GetActorTick() : Mod::GetGameInterface()->GetDemoInfo().gameTick;

        const bool capturing = Components::CaptureManager::Get().IsCapturing();
        if (capturing && !wasCapturing)
            steady.Reset();
        wasCapturing = capturing;
        BoneCameraFrame frame{tick, settings, bone};
        frame.timelineTick = timelineTick;
        frame.capturing = capturing;
        frame.fromUi = *state.bone;
        frame.time = NowMs();
        if (bone)
        {
            Components::BoneCameraLogic::Transform at{bone->position, bone->axes};
            const auto targetFrame = Components::BoneCameraLogic::MakeFrame(bone->origin, bone->yaw);
            if (settings.steady > 0.0f)
                at = steady.Step(at, targetFrame, tick, settings.steady, {settings.target, settings.bone});
            else
                steady.Reset();
            const auto placed = Components::BoneCameraLogic::Place(at, targetFrame, settings);
            auto view = Components::BoneCameraLogic::View{placed.position, placed.angles, settings.fov};
            static const auto timescale = Functions::FindDvar("timescale");
            if (capturing || !glideOn)
                glide.Reset();
            else
                view = glide.Step(view, timelineTick, NowMs(), timescale ? timescale->current.value : 1.0f,
                                  {settings.target, settings.bone});
            state.position = view.position;
            state.rotation = view.angles;
            state.fov = view.fov;
            frame.position = view.position;
            frame.angles = view.angles;
        }

        std::lock_guard lock(framesMutex);
        frames.push_back(frame);
        if (frames.size() > KEPT_FRAMES)
            frames.pop_front();
    }

    // cg_thirdPerson decides both the view CG_CalcViewValues builds and whether the player model is drawn, so it is
    // set here, before the view is built and from the same camera copy. Switching it from the render thread lands
    // mid-frame: the camera then sits inside the player model, or stays in third person after leaving the free
    // camera. The game also turns it off by itself, e.g. CG_SetThirdPerson(0) when a rewind replays the map restart
    // at the start of the demo, so it is kept on while a mod camera or the third person camera is selected.
    void UpdateThirdPerson()
    {
        const bool thirdPerson =
            frameState.isModControlled || frameState.mode == Components::Camera::Mode::ThirdPerson;

        static auto cg_thirdPerson = Functions::FindDvar("cg_thirdPerson");
        if (cg_thirdPerson && (thirdPerson || frameThirdPerson != thirdPerson))
            cg_thirdPerson->current.integer = thirdPerson ? 1 : 0;

        frameThirdPerson = thirdPerson;
    }

    // CG_CalcViewValues fills cg->refdef, but CG_DrawActiveFrame can still overwrite the view afterwards
    // (scripted cameras, and a path that rebuilds the view from cg's stored origin and angles). Every one of
    // those functions gets the same post-hook, so the last writer of the refdef is always us.
    void UpdateRefdef()
    {
        if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            return;

        auto cg = Structures::GetClientGlobals();
        if (!cg)
            return;

        auto& refdef = cg->refdef;
        auto& camera = Components::CameraManager::Get().GetActiveCamera();
        const auto& state = frameState;

        if (!state.isModControlled)
        {
            // the UI may have switched to a mod camera since the copy was taken, don't move that one
            if (!camera->IsModControlledCameraMode())
            {
                camera->GetPosition() = glm::make_vec3(refdef.vieworg);
                camera->GetRotation() = MathUtils::AxisToAngles(glm::make_mat3x3(&refdef.viewaxis[0][0]));
                camera->GetFov() = glm::degrees(std::atan(refdef.tanHalfFovX) * 2.0f);
            }
            return;
        }

        refdef.vieworg[0] = state.position[0];
        refdef.vieworg[1] = state.position[1];
        refdef.vieworg[2] = state.position[2];

        const auto axis = MathUtils::AnglesToAxis(state.rotation);
        for (int x = 0; x < 3; x++)
        {
            for (int y = 0; y < 3; y++)
            {
                refdef.viewaxis[x][y] = axis[x][y];
            }
        }

        for (int i = 0; i < 3; i++)
        {
            cg->refdefViewAngles[i] = state.rotation[i];
        }

        refdef.tanHalfFovX = std::tan(glm::radians(state.fov) * 0.5f);
        refdef.tanHalfFovY = refdef.tanHalfFovX * (static_cast<float>(refdef.height) / static_cast<float>(refdef.width));
    }

    typedef void(__cdecl* CG_ViewFunction_t)(int localClientNum);

    CG_ViewFunction_t CG_CalcViewValues_Trampoline;
    void __cdecl CG_CalcViewValues_Hook(int localClientNum)
    {
        mainThreadId = GetCurrentThreadId();

        if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
        {
            RecordRenderedView();
            frameState = cameraState ? *cameraState : GetCameraState();
            if (Resampling::AppliesTo(frameState))
                Resampling::Apply(frameState);
            if (frameState.mode == Components::Camera::Mode::Dolly)
                ApplyDollyCamera(frameState);
            // (the bone camera is placed once the frame's models are posed, see OnEntitiesAdded)

            UpdateThirdPerson();
            Loadouts::ProcessPredictedPlayer();
        }
        else
        {
            frameThirdPerson.reset();
            Loadouts::ProcessPredictedPlayer();
        }

        CG_CalcViewValues_Trampoline(localClientNum);
        UpdateRefdef();
    }

    void OnEntitiesAdded()
    {
        if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo || !frameState.bone ||
            !frameState.isModControlled)
            return;
        ApplyBoneCamera(frameState);
        UpdateRefdef();
    }

    CG_ViewFunction_t CG_ApplyScriptedCamera_Trampoline;
    void __cdecl CG_ApplyScriptedCamera_Hook(int localClientNum)
    {
        CG_ApplyScriptedCamera_Trampoline(localClientNum);
        UpdateRefdef();
    }

    CG_ViewFunction_t CG_SetViewFromViewAngles_Trampoline;
    void __cdecl CG_SetViewFromViewAngles_Hook(int localClientNum)
    {
        CG_SetViewFromViewAngles_Trampoline(localClientNum);
        UpdateRefdef();
    }

    typedef int(__cdecl* R_SyncRenderThread_t)();
    R_SyncRenderThread_t R_SyncRenderThread_Trampoline;
    int __cdecl R_SyncRenderThread_Hook()
    {
        const auto result = R_SyncRenderThread_Trampoline();

        // the render thread is idle now, so the camera can't be in the middle of an update
        if (IsMainThread())
        {
            cameraState = GetCameraState();
            if (cameraState->mode == Components::Camera::Mode::Dolly)
                CopyCampath();
            if (cameraState->mode == Components::Camera::Mode::Bone)
                CopyBoneCameraKeys();
            Bones::UpdateRequestedBones();

            // the render thread draws the frame built in this one next
            Components::CaptureManager::Get().OnFrameHandedOver();
        }

        return result;
    }

    // CG_EjectWeaponBrass ejects the casings of the player a demo was recorded from out of the first person gun whenever
    // the snapshot has that player in first person, even while the view is in third person, which cg_thirdPerson puts
    // it in for the mod cameras. The first person gun isn't posed then, so the casings came out wherever it was last
    // drawn, and after a rewind, which creates it anew, out of the middle of the map. Like CG_Player does for the player
    // model, they now come out of the gun in the player model's hands while the view is in third person.
    typedef bool(__cdecl* CG_IsFirstPersonPlayer_t)(Structures::snapshot_t* snap, std::int32_t entityNum);
    CG_IsFirstPersonPlayer_t CG_EjectWeaponBrass_IsFirstPerson_Original;
    std::optional<Patches::Patch<5>> ejectWeaponBrassCall;

    bool __cdecl CG_EjectWeaponBrass_IsFirstPerson_Hook(Structures::snapshot_t* snap, std::int32_t entityNum)
    {
        const auto cg = Structures::GetClientGlobals();
        if (cg && cg->renderingThirdPerson)
            return false;

        return CG_EjectWeaponBrass_IsFirstPerson_Original(snap, entityNum);
    }

    void RedirectEjectWeaponBrassCall()
    {
        const auto callAddress = GetGameAddresses().CG_EjectWeaponBrass_IsFirstPerson();
        const auto callOffset = *reinterpret_cast<std::int32_t*>(callAddress + 1);
        CG_EjectWeaponBrass_IsFirstPerson_Original =
            reinterpret_cast<CG_IsFirstPersonPlayer_t>(callAddress + 5 + callOffset);

        std::array<std::uint8_t, 5> call{0xE8};
        const auto hookOffset = static_cast<std::int32_t>(
            reinterpret_cast<std::uintptr_t>(CG_EjectWeaponBrass_IsFirstPerson_Hook) - (callAddress + 5));
        std::memcpy(&call[1], &hookOffset, sizeof(hookOffset));

        // reverted when the mod is unloaded
        ejectWeaponBrassCall.emplace(callAddress, call, Patches::PatchApplySetting::Immediately);
    }

    // The casings' effect only spawns its elements within reach of the effects system's camera (100 units for an M60's),
    // as the game only ever shows them from the player's own view or close behind it. A mod camera is often further
    // away, so in third person the reach is measured from the player instead, for the time the casings are spawned.
    typedef void(__cdecl* CG_EjectWeaponBrass_t)(int localClientNum, Structures::entityState_s* es, int event,
                                                 int weapon);
    CG_EjectWeaponBrass_t CG_EjectWeaponBrass_Trampoline;
    void __cdecl CG_EjectWeaponBrass_Hook(int localClientNum, Structures::entityState_s* es, int event, int weapon)
    {
        const auto cg = Structures::GetClientGlobals();
        const auto system = *reinterpret_cast<Structures::FxSystem**>(GetGameAddresses().fx_system());
        const auto pose = es && cg && cg->renderingThirdPerson ? Functions::CG_GetPose(localClientNum, es->number)
                                                                : nullptr;
        if (!system || !pose || Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
        {
            CG_EjectWeaponBrass_Trampoline(localClientNum, es, event, weapon);
            return;
        }

        float camera[3];
        std::memcpy(camera, system->cameraOrigin, sizeof(camera));
        std::memcpy(system->cameraOrigin, pose->origin, sizeof(camera));
        CG_EjectWeaponBrass_Trampoline(localClientNum, es, event, weapon);
        std::memcpy(system->cameraOrigin, camera, sizeof(camera));
    }

    // Effects attached to a model, like casings and muzzle flashes, find the model through CG_GetPose, the pose of the
    // entity with that model. For the player a demo was recorded from, the game draws cg.predictedPlayerEntity and
    // skips that player's own entity (see CG_AddPacketEntity), whose pose is then left as it was. Since the casings
    // and muzzle flashes of that player come out of the player model in third person, the model's bones were worked
    // out from that stale pose (with old aim angles) whenever one of them spawned, and on every shot the model was
    // drawn in another pose for one frame. Like CG_AddPacketEntity, effects now use the pose of the entity drawn.
    typedef Structures::cpose_t*(__cdecl* CG_GetPose_t)(int localClientNum, int handle);
    CG_GetPose_t CG_GetPose_Trampoline;
    Structures::cpose_t* __cdecl CG_GetPose_Hook(int localClientNum, int handle)
    {
        const auto cg = Structures::GetClientGlobals();
        if (cg && cg->nextSnap && (cg->nextSnap->ps.otherFlags & Structures::POF_FIRST_PERSON) &&
            handle == cg->nextSnap->ps.clientNum &&
            Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
        {
            return reinterpret_cast<Structures::cpose_t*>(&cg->predictedPlayerEntity);
        }

        return CG_GetPose_Trampoline(localClientNum, handle);
    }

    // The player's own screen: the game picks the frame's vision from the demo
    // player's state whatever the camera shows, so the Valkyrie's tvguided vision (no colour, green, contrast and
    // bloom) covered the free camera, and so did a concussion's (it takes over the naked vision for its time). Outside
    // the player's first person view the frame gets the map's own vision and none of the client scripts' screen
    // filters, while the game's state of them goes on untouched for when the view is back in first person.
    std::atomic<bool> visionKept = false, filtersKept = false;

    bool ShowsPlayerScreen()
    {
        return Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo ||
               frameState.mode == Components::Camera::Mode::FirstPerson;
    }

    void KeepPlayerScreen(bool vision, bool filters)
    {
        visionKept = vision;
        filtersKept = filters;
    }

    // The naked vision as it was before a shellshock's vision took it over, kept from every frame without one, and the
    // client's time until which the naked mode is the shock's (its lerp back) once the shock let it go
    std::optional<std::array<std::uint8_t, Structures::CG_VISION_SIZE>> mapVision;
    bool shockVisionWas = false;
    std::int32_t shockVisionFrom = 0, shockVisionUntil = 0;

    // whether the naked vision is a shellshock's now (keeping the map's while it isn't)
    bool TrackShockVision(std::uint8_t* cg, std::int32_t time)
    {
        const auto field = [&](std::size_t offset) { return *reinterpret_cast<std::int32_t*>(cg + offset); };
        const bool shock = field(Structures::CG_SHOCK_VISION) != 0;
        if (shockVisionWas && !shock)
        {
            shockVisionFrom = field(Structures::CG_VISION_LERP_START);
            shockVisionUntil = shockVisionFrom + field(Structures::CG_VISION_LERP_DURATION);
        }
        // (a rewind to before it)
        if (time < shockVisionFrom)
            shockVisionUntil = 0;
        shockVisionWas = shock;

        const bool shocked = shock || time < shockVisionUntil;
        if (!shocked)
        {
            mapVision.emplace();
            std::memcpy(mapVision->data(), cg + Structures::CG_VISION_CURRENT, Structures::CG_VISION_SIZE);
        }
        return shocked;
    }

    std::vector<std::uint8_t> GetMapVision()
    {
        return mapVision ? std::vector<std::uint8_t>(mapVision->begin(), mapVision->end()) : std::vector<std::uint8_t>();
    }

    typedef void(__cdecl* CG_VisionSetFrame_t)(int localClientNum, bool extraCam);
    CG_VisionSetFrame_t CG_VisionSetFrame_Trampoline;
    void __cdecl CG_VisionSetFrame_Hook(int localClientNum, bool extraCam)
    {
        const auto globals = Structures::GetClientGlobals();
        const auto cg = reinterpret_cast<std::uint8_t*>(globals);
        if (extraCam || !cg)
        {
            CG_VisionSetFrame_Trampoline(localClientNum, extraCam);
            return;
        }
        const bool shocked = TrackShockVision(cg, globals->time);
        if (ShowsPlayerScreen() || visionKept)
        {
            CG_VisionSetFrame_Trampoline(localClientNum, extraCam);
            return;
        }

        // the modes the player's state picks (night to tvguided), and the naked one while a shock has it, have the
        // map's vision for the call, whichever it picks
        constexpr auto LAST = Structures::VISION_TVGUIDED;
        const auto current = [&](std::size_t mode) {
            return cg + Structures::CG_VISION_CURRENT + mode * Structures::CG_VISION_SIZE;
        };
        const auto loaded = [&](std::size_t mode) {
            return reinterpret_cast<std::int32_t*>(cg + Structures::CG_VISION_LOADED +
                                                   mode * Structures::CG_VISION_STATE_SIZE);
        };
        std::array<std::uint8_t, Structures::CG_VISION_SIZE> map;
        std::memcpy(map.data(), shocked && mapVision ? mapVision->data() : current(Structures::VISION_NAKED),
                    map.size());
        std::array<std::array<std::uint8_t, Structures::CG_VISION_SIZE>, LAST + 1> kept;
        std::array<std::int32_t, LAST + 1> keptLoaded;
        for (std::size_t mode = 0; mode <= LAST; ++mode)
        {
            std::memcpy(kept[mode].data(), current(mode), Structures::CG_VISION_SIZE);
            keptLoaded[mode] = *loaded(mode);
            std::memcpy(current(mode), map.data(), Structures::CG_VISION_SIZE);
            *loaded(mode) = keptLoaded[Structures::VISION_NAKED];
        }
        CG_VisionSetFrame_Trampoline(localClientNum, extraCam);
        for (std::size_t mode = 0; mode <= LAST; ++mode)
        {
            std::memcpy(current(mode), kept[mode].data(), Structures::CG_VISION_SIZE);
            *loaded(mode) = keptLoaded[mode];
        }
    }

    typedef void(__cdecl* CG_ScreenFilters_t)(int localClientNum, int extraCam);
    CG_ScreenFilters_t CG_ScreenFilters_Trampoline;
    void __cdecl CG_ScreenFilters_Hook(int localClientNum, int extraCam)
    {
        const auto cg = reinterpret_cast<std::uint8_t*>(Structures::GetClientGlobals());
        if (extraCam || !cg || ShowsPlayerScreen() || filtersKept)
        {
            CG_ScreenFilters_Trampoline(localClientNum, extraCam);
            return;
        }

        // the code's filters (tvguided, infrared, scope) already go with the player's own view; the client scripts'
        // (the Tactical Mask's) are left out for the call
        const auto scripts = cg + Structures::CG_SCRIPT_FILTERS_ENABLED;
        std::array<std::uint8_t, Structures::CG_SCRIPT_FILTERS> kept;
        std::memcpy(kept.data(), scripts, kept.size());
        std::memset(scripts, 0, kept.size());
        CG_ScreenFilters_Trampoline(localClientNum, extraCam);
        std::memcpy(scripts, kept.data(), kept.size());
    }

    void Install()
    {
        RedirectEjectWeaponBrassCall();
        HookManager::CreateHook(GetGameAddresses().CG_VisionSetFrame(), (std::uintptr_t)CG_VisionSetFrame_Hook,
                                (std::uintptr_t*)&CG_VisionSetFrame_Trampoline);
        HookManager::CreateHook(GetGameAddresses().CG_ScreenFilters(), (std::uintptr_t)CG_ScreenFilters_Hook,
                                (std::uintptr_t*)&CG_ScreenFilters_Trampoline);
        HookManager::CreateHook(GetGameAddresses().CG_EjectWeaponBrass(), (std::uintptr_t)CG_EjectWeaponBrass_Hook,
                                (std::uintptr_t*)&CG_EjectWeaponBrass_Trampoline);

        HookManager::CreateHook(GetGameAddresses().CG_GetPose(), (std::uintptr_t)CG_GetPose_Hook,
                                (std::uintptr_t*)&CG_GetPose_Trampoline);
        HookManager::CreateHook(GetGameAddresses().R_SyncRenderThread(), (std::uintptr_t)R_SyncRenderThread_Hook,
                                (std::uintptr_t*)&R_SyncRenderThread_Trampoline);
        HookManager::CreateHook(GetGameAddresses().CG_CalcViewValues(), (std::uintptr_t)CG_CalcViewValues_Hook,
                                (std::uintptr_t*)&CG_CalcViewValues_Trampoline);
        HookManager::CreateHook(GetGameAddresses().CG_ApplyScriptedCamera(),
                                (std::uintptr_t)CG_ApplyScriptedCamera_Hook,
                                (std::uintptr_t*)&CG_ApplyScriptedCamera_Trampoline);
        HookManager::CreateHook(GetGameAddresses().CG_SetViewFromViewAngles(),
                                (std::uintptr_t)CG_SetViewFromViewAngles_Hook,
                                (std::uintptr_t*)&CG_SetViewFromViewAngles_Trampoline);

        // registered after CameraManager's own listener, so this runs once the camera has been updated for the frame
        Events::RegisterListener(EventType::OnFrame, Resampling::AddSample);
    }

    void ApplyShellshock()
    {
        if (const auto cg_drawShellshock = Functions::FindDvar("cg_drawShellshock"))
            cg_drawShellshock->current.enabled =
                HUD::showShellshock &&
                Components::CameraManager::Get().GetActiveCamera()->GetMode() == Components::Camera::Mode::FirstPerson;
    }

    void OnCameraChanged()
    {
        auto& camera = Components::CameraManager::Get().GetActiveCamera();
        auto isFreeCamera = camera->IsModControlledCameraMode();

        // cg_thirdPerson follows the camera in UpdateThirdPerson, on the main thread

        if (auto cg_draw2D = Functions::FindDvar("cg_draw2D"))
            cg_draw2D->current.enabled = !isFreeCamera;

        ApplyShellshock();

        constexpr float LODBIAS = -40000;
        for (const auto dvarName : {"r_lodBiasRigid", "r_lodBiasSkinned"})
        {
            if (auto dvar = Functions::FindDvar(dvarName))
                dvar->current.value = LODBIAS;
        }
    }
}  // namespace IWXMVM::T5::Hooks::Camera
