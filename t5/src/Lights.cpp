#include "StdInclude.hpp"
#include "Lights.hpp"

#include <deque>
#include <map>

#include "Addresses.hpp"
#include "AnimationPose.hpp"
#include "Bones.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "Functions.hpp"
#include "Hooks/Camera.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/Lights.hpp"
#include "Components/Playback.hpp"
#include "Mod.hpp"
#include "Structures.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::T5::Lights
{
    namespace
    {
        using Components::LightLogic::State;
        using Structures::GfxLight;
        using Structures::GfxPointLitSurfs;
        namespace LightLogic = Components::LightLogic;

        constexpr std::uint8_t LIGHT_SPOT = 2;
        constexpr std::uint8_t LIGHT_OMNI = 3;
        // the additive pass's techniques
        constexpr std::uint8_t TECHNIQUE_LIGHT_SPOT = 0x73;
        constexpr std::uint8_t TECHNIQUE_LIGHT_OMNI = 0x74;
        constexpr std::uint8_t TECHNIQUE_LIGHT_SPOT_SHADOW = 0x75;
        // what the view holds (GfxViewInfo): its list of lit surfaces, and its pass's lights, four at most, and count
        constexpr std::size_t VIEW_LIT_INFO = 0x2230;
        constexpr std::size_t VIEW_POINT_LIT_SURFS = 0x1a0;
        constexpr std::size_t VIEW_POINT_LIT_COUNT = 0x930;
        constexpr std::int32_t VIEW_POINT_LITS = 4;
        // the map's exposure: the spot's shaders multiply by it, the omni's don't, and a spot came out a quarter of a
        // point of the same colour past that (measured), so one Intensity is one
        // brightness
        constexpr std::size_t VIEW_EXPOSURE = 0x35d8;
        constexpr float SPOT_TO_POINT = 4.0f;
        // the view's view and projection, and the rectangle it draws in
        constexpr std::size_t VIEW_VIEW_PROJECTION = 0x80;
        constexpr std::size_t VIEW_VIEWPORT = 0x140;
        // the game's frame data: which shadowable lights have a shadow map, by bit, and those lights
        constexpr std::size_t FRONT_END_SHADOW_MAP_BITS = 0x15d780;
        constexpr std::size_t FRONT_END_SHADOWABLE_LIGHTS = 0x16cec0;
        // the shadow chooser moves on by the time since it last ran: this puts its fading in behind it at once
        constexpr std::int32_t SHADOW_CHOOSER_STEP = 1000;

        std::int32_t& GetAddedLightCount()
        {
            return *reinterpret_cast<std::int32_t*>(GetGameAddresses().scene_addedLightCount());
        }

        GfxLight* GetAddedLights()
        {
            return reinterpret_cast<GfxLight*>(GetGameAddresses().scene_addedLights());
        }

        std::uint8_t* GetFrontEndData()
        {
            return *reinterpret_cast<std::uint8_t**>(GetGameAddresses().frontEndData());
        }

        bool HasShadowMap()
        {
            const auto index = *reinterpret_cast<std::int32_t*>(GetGameAddresses().dynamicSpotIndex());
            const auto bits = reinterpret_cast<const std::uint32_t*>(GetFrontEndData() + FRONT_END_SHADOW_MAP_BITS);
            return (bits[index >> 5] >> (index & 31)) & 1;
        }

        // (see R_SetQuadMesh_Hook)
        std::uint32_t quadWholeView = 0;
        float quadWidth = 0.0f, quadHeight = 0.0f;

        // the frame's lights for the pass, the shadowed spot among them
        std::vector<GfxLight> frameLights;
        std::uint32_t frameLightCount = 0;  // (for the hook below, which is plain assembly)
        std::atomic<std::uint32_t> shadowedLight = 0;
        std::uint32_t frameShadowed = 0;
        bool unloaded = false;

        // What a view's pass draws. The backend reads it a frame after the front end wrote it, with the front end on
        // the next frame by then: each view has its slot, and the game has two of its frame data, so a view comes back
        // only once the backend is done with it.
        struct PassView
        {
            std::uint8_t* viewInfo = nullptr;
            std::vector<GfxPointLitSurfs> surfaces;
        };
        std::array<PassView, 32> passViews;

#ifdef IWXMVM_T5_HARNESS
        std::mutex placedMutex;
        std::deque<PlacedLight> placedLights;
        std::atomic<bool> keepsFollowed = true;
#else
        constexpr bool keepsFollowed = true;
#endif

        // (main thread) where each light on a bone was last, by the light's own id
        std::unordered_map<std::uint32_t, LightLogic::Placed> followed;

        std::atomic<std::int32_t> statPass = 0, statBatches = 0, statLitSurfaces = 0, statAdded = 0, statVisible = 0;
        std::atomic<bool> statShadowMap = false;
        std::atomic<float> statExposure = 0.0f;

        // R_GetVisibleDLights(visible): the view's copy of the shadowed spot lights nothing, so the pass alone draws
        // it. It stays in the view, which gathers what casts on it, and the shadow's own copy keeps the colour the pass
        // reads through it.
        typedef std::int32_t(__cdecl* R_GetVisibleDLights_t)(GfxLight* visible);
        R_GetVisibleDLights_t R_GetVisibleDLights_Trampoline;
        std::int32_t __cdecl R_GetVisibleDLights_Hook(GfxLight* visible)
        {
            const auto count = R_GetVisibleDLights_Trampoline(visible);
            statAdded = GetAddedLightCount();
            statVisible = count;
            if (frameShadowed)
                for (std::int32_t i = 0; i < count; ++i)
                    if (visible[i].type == LIGHT_SPOT)
                    {
                        std::memset(visible[i].diffuseColor, 0, sizeof(visible[i].diffuseColor));
                        std::memset(visible[i].specularColor, 0, sizeof(visible[i].specularColor));
                    }
            return count;
        }

        void ScaleColor(GfxLight& light, float scale)
        {
            for (std::int32_t i = 0; i < 3; ++i)
            {
                light.diffuseColor[i] *= scale;
                light.specularColor[i] *= scale;
            }
        }

        // R_EmitPointLitSurfs: nothing fills the per-light lists this reads in this game, so the pass it feeds never
        // ran. Each light gets the view's own list of lit surfaces instead, drawn inside the light's sphere.
        std::int32_t __cdecl EmitPointLitSurfs(std::int32_t, GfxLight*, std::uint8_t* viewInfo, const float*)
        {
            auto slot = std::ranges::find(passViews, viewInfo, &PassView::viewInfo);
            if (slot == passViews.end())
                slot = std::ranges::find(passViews, nullptr, &PassView::viewInfo);
            if (slot == passViews.end())
                return 0;
            slot->viewInfo = viewInfo;
            slot->surfaces.resize(frameLights.size());

            const auto& lit = *reinterpret_cast<const Structures::GfxDrawSurfListInfo*>(viewInfo + VIEW_LIT_INFO);
            statLitSurfaces = lit.drawSurfCount;
            const bool shadowMap = frameShadowed && HasShadowMap();
            statShadowMap = shadowMap;
            const auto exposure = *reinterpret_cast<const float*>(viewInfo + VIEW_EXPOSURE);
            statExposure = exposure;
            const auto spotScale = SPOT_TO_POINT / (exposure > 0.0f ? exposure : 1.0f);
            for (std::size_t i = 0; i < frameLights.size(); ++i)
            {
                auto& surfaces = slot->surfaces[i];
                surfaces.light = frameLights[i];
                surfaces.info = lit;
                surfaces.info.light = &surfaces.light;
                surfaces.info.cameraView = 1;
                const bool spot = surfaces.light.type == LIGHT_SPOT;
                const bool shadowed = spot && shadowMap && surfaces.light.canUseShadowMap;
                if (spot)
                    ScaleColor(surfaces.light, spotScale);
                surfaces.info.baseTechType = shadowed ? TECHNIQUE_LIGHT_SPOT_SHADOW
                                             : spot   ? TECHNIQUE_LIGHT_SPOT
                                                      : TECHNIQUE_LIGHT_OMNI;
                if (shadowed)
                {
                    // the shadow's chooser gave the scene's copy its map after this copy was made
                    const auto index = *reinterpret_cast<std::int32_t*>(GetGameAddresses().dynamicSpotIndex());
                    auto& shadowable =
                        reinterpret_cast<GfxLight*>(GetFrontEndData() + FRONT_END_SHADOWABLE_LIGHTS)[index];
                    surfaces.light.spotShadowIndex = shadowable.spotShadowIndex;
                    // (the pass reads the colour through this one)
                    ScaleColor(shadowable, spotScale);
                }
            }
            return static_cast<std::int32_t>(std::min<std::size_t>(frameLights.size(), VIEW_POINT_LITS));
        }

        // (count in ecx, the visible lights in edx, the view and its origin on the stack, the caller's to clear)
        std::uintptr_t R_EmitPointLitSurfs_Trampoline;
        __declspec(naked) void R_EmitPointLitSurfs_Hook()
        {
            __asm {
                cmp frameLightCount, 0
                je game
                push dword ptr [esp + 8]
                push dword ptr [esp + 8]
                push edx
                push ecx
                call EmitPointLitSurfs
                add esp, 16
                ret
            game:
                jmp dword ptr [R_EmitPointLitSurfs_Trampoline]
            }
        }

        // (the view in eax, the context on the stack, the caller's to clear)
        std::uintptr_t R_DrawPointLitSurfs_Trampoline;
        void CallDrawPointLitSurfs(std::uint8_t* viewInfo, void* context)
        {
            __asm {
                mov eax, viewInfo
                push context
                call dword ptr [R_DrawPointLitSurfs_Trampoline]
                add esp, 4
            }
        }

        // R_DrawPointLitSurfs: the view holds four at a time, so they go four by four
        void __cdecl DrawPointLitSurfs(std::uint8_t* viewInfo, void* context)
        {
            // (it comes once for each view of each frame)
            Hooks::Camera::OnViewDrawn(viewInfo);
            auto& count = *reinterpret_cast<std::int32_t*>(viewInfo + VIEW_POINT_LIT_COUNT);
            const auto slot = std::ranges::find(passViews, viewInfo, &PassView::viewInfo);
            // (no pass for this view this frame: it didn't come through EmitPointLitSurfs)
            if (slot == passViews.end() || count == 0)
                return CallDrawPointLitSurfs(viewInfo, context);

            const auto total = static_cast<std::int32_t>(slot->surfaces.size());
            const auto view = reinterpret_cast<GfxPointLitSurfs*>(viewInfo + VIEW_POINT_LIT_SURFS);
            const auto viewport = reinterpret_cast<const std::int32_t*>(viewInfo + VIEW_VIEWPORT);
            quadWidth = static_cast<float>(viewport[2]);
            quadHeight = static_cast<float>(viewport[3]);
            quadWholeView = 1;
            for (std::int32_t first = 0; first < total; first += VIEW_POINT_LITS)
            {
                count = std::min(total - first, VIEW_POINT_LITS);
                for (std::int32_t i = 0; i < count; ++i)
                    view[i] = slot->surfaces[first + i];
                CallDrawPointLitSurfs(viewInfo, context);
            }
            quadWholeView = 0;
        }

        __declspec(naked) void R_DrawPointLitSurfs_Hook()
        {
            __asm {
                push dword ptr [esp + 4]
                push eax
                call DrawPointLitSurfs
                add esp, 8
                ret
            }
        }

        // The data R_DrawPointLitSurfs gives its callback for a light: the view, its quad, its list, and its scissor.
        // The rectangle the pass works out goes wrong for many lights, so each light's is worked out again here.
        struct PointLitCallbackData
        {
            std::uint8_t* viewInfo;
            void* quad;
            Structures::GfxDrawSurfListInfo* list;
            std::int32_t x, y, width, height;
        };

        void __cdecl FixScissor(PointLitCallbackData* data)
        {
            const auto light = data->list->light;
            const auto viewInfo = data->viewInfo;
            const auto viewProjection = glm::make_mat4(reinterpret_cast<const float*>(viewInfo + VIEW_VIEW_PROJECTION));
            const auto viewport = reinterpret_cast<const std::int32_t*>(viewInfo + VIEW_VIEWPORT);
            const auto rect = LightLogic::GetScissor(glm::make_vec3(light->origin), light->radius, viewProjection,
                                                     {viewport[0], viewport[1], viewport[2], viewport[3]});
            data->x = rect.x;
            data->y = rect.y;
            data->width = rect.width;
            data->height = rect.height;
        }

        std::uintptr_t R_DrawPointLitSurfsCallback_Trampoline;
        __declspec(naked) void R_DrawPointLitSurfsCallback_Hook()
        {
            __asm {
                push dword ptr [esp + 4]
                call FixScissor
                add esp, 4
                jmp dword ptr [R_DrawPointLitSurfsCallback_Trampoline]
            }
        }

        // Before each light the pass clears the frame's alpha in a quad of its rectangle. The view has four of those
        // quads, and they keep their place in the vertex buffer: going four by four, the next four's rectangles land
        // under the draws of the last four, which the driver hasn't made yet, and cut their lights off with a hard
        // edge. With every quad the whole view, it doesn't matter which one a light gets.
        std::uintptr_t R_SetQuadMesh_Trampoline;
        __declspec(naked) void R_SetQuadMesh_Hook()
        {
            __asm {
                cmp quadWholeView, 0
                je game
                mov dword ptr [esp + 4], 0
                mov dword ptr [esp + 8], 0
                mov ecx, quadWidth
                mov dword ptr [esp + 12], ecx
                mov ecx, quadHeight
                mov dword ptr [esp + 16], ecx
            game:
                jmp dword ptr [R_SetQuadMesh_Trampoline]
            }
        }

        // The dvars the game's spot call takes its cone from
        struct SpotDvars
        {
            float* range = nullptr;
            float* startRadius = nullptr;
            float* endRadius = nullptr;
            float* innerFraction = nullptr;
            float* brightness = nullptr;
            std::int32_t* flashlight = nullptr;

            bool Found() const
            {
                return range && startRadius && endRadius && innerFraction && brightness && flashlight;
            }
        };

        const SpotDvars& GetSpotDvars()
        {
            static const auto dvars = [] {
                const auto find = [](const char* name) {
                    const auto dvar = Mod::GetGameInterface()->GetDvar(name);
                    return dvar ? dvar->value : nullptr;
                };
                const auto value = [&](const char* name) {
                    const auto found = find(name);
                    return found ? &found->floating_point : nullptr;
                };
                const auto flashlight = find("r_enableFlashlight");
                return SpotDvars{value("r_flashLightRange"),      value("r_flashLightStartRadius"),
                                 value("r_flashLightEndRadius"),  value("r_spotLightFovInnerFraction"),
                                 value("r_flashLightBrightness"), flashlight ? &flashlight->int32 : nullptr};
            }();
            return dvars;
        }

        // The game's forward, right and up for a direction, as the matrices of its spot want them
        std::array<glm::vec3, 3> GetAxes(glm::vec3 direction)
        {
            const auto forward = glm::normalize(direction);
            auto right = glm::cross(forward, glm::vec3(0.0f, 0.0f, 1.0f));
            if (glm::length(right) < 0.001f)
                right = glm::vec3(0.0f, -1.0f, 0.0f);
            right = glm::normalize(right);
            return {forward, right, glm::cross(right, forward)};
        }

        void SetColor(GfxLight& light, const State& state)
        {
            for (std::int32_t i = 0; i < 3; ++i)
            {
                light.diffuseColor[i] = state.color[i];
                light.specularColor[i] = state.color[i] * state.highlights;
            }
            light.diffuseColor[3] = light.specularColor[3] = 1.0f;
        }

        // A spot through the game's call, which is the flashlight's and makes its matrices: its cone comes from the
        // dvars, set around the call. It lands in the scene, where it stays only as the shadowed spot.
        bool AddSpot(const State& state, const LightLogic::Placed& placed)
        {
            const auto& dvars = GetSpotDvars();
            if (!dvars.Found())
                return false;
            // multiplayer never sets the flashlight's definition, which the call gives its light and the renderer
            // reads (a null one crashes the game), and which is how the shadows' chooser knows the light it puts first
            const auto definition = reinterpret_cast<void**>(GetGameAddresses().spotLightDef());
            *definition = *reinterpret_cast<void**>(GetGameAddresses().omniLightDef());

            const auto axes = GetAxes(placed.direction);
            const float axis[3][3] = {{axes[0].x, axes[0].y, axes[0].z},
                                      {-axes[1].x, -axes[1].y, -axes[1].z},
                                      {axes[2].x, axes[2].y, axes[2].z}};
            const float origin[3] = {placed.position.x, placed.position.y, placed.position.z};

            const float kept[] = {*dvars.range, *dvars.startRadius, *dvars.endRadius, *dvars.innerFraction,
                                  *dvars.brightness};
            const auto keptFlashlight = *dvars.flashlight;
            *dvars.range = state.radius;
            *dvars.startRadius = 0.0f;
            *dvars.endRadius = state.radius * std::tan(glm::radians(state.cone / 2.0f));
            *dvars.innerFraction = 1.0f - state.softness;
            *dvars.brightness = 1.0f;
            *dvars.flashlight = 0;

            const auto count = GetAddedLightCount();
            typedef void(__cdecl * R_AddSpotLightToScene_t)(const float*, const float*, float, float, float, float);
            reinterpret_cast<R_AddSpotLightToScene_t>(GetGameAddresses().R_AddSpotLightToScene())(
                origin, &axis[0][0], state.radius, 1.0f, 1.0f, 1.0f);

            *dvars.range = kept[0];
            *dvars.startRadius = kept[1];
            *dvars.endRadius = kept[2];
            *dvars.innerFraction = kept[3];
            *dvars.brightness = kept[4];
            *dvars.flashlight = keptFlashlight;
            if (GetAddedLightCount() != count + 1)
                return false;
            SetColor(GetAddedLights()[count], state);
            return true;
        }

        // A point as the game's call for it writes it; that call would merge it into a flash in the same place, cap its
        // radius, and the pass doesn't need the scene
        GfxLight MakePoint(const State& state, const LightLogic::Placed& placed)
        {
            GfxLight light{};
            light.type = LIGHT_OMNI;
            light.origin[0] = placed.position.x;
            light.origin[1] = placed.position.y;
            light.origin[2] = placed.position.z;
            light.radius = state.radius;
            light.spotShadowIndex = 0xffffffff;
            light.def = *reinterpret_cast<void**>(GetGameAddresses().omniLightDef());
            SetColor(light, state);
            return light;
        }
    }  // namespace

    namespace
    {
        // (main thread) each light on a bone's Steady, by the light's own id and whether it's the gun on
        // screen's: a light in first person goes by both in a frame (see ViewGunLight)
        std::map<std::pair<std::uint32_t, bool>, Components::BoneCameraLogic::Steady> steadies;
        bool wasCapturing = false;

        // A light of the frame that may go by the gun on screen. The game
        // poses that gun after the frame's entities (CG_AddViewWeapon, called by CG_DrawActiveFrame after
        // CG_AddPacketEntities), so the light is placed by the body first and again by the gun once it is posed
        // (OnViewGunAdded), before the scene goes to the renderer.
        struct ViewGunLight
        {
            Types::Actor actor;  // with its keyed place
            State state;
            std::size_t index = 0;  // in frameLights
            bool shadowed = false;
        };
        std::vector<ViewGunLight> viewGunLights;
        // the frame's shadowed spot, added again last when a spot is added after it (the game keeps one set of the
        // planes that cull what casts on it, the last call's)
        std::optional<std::pair<State, LightLogic::Placed>> frameCaster;
        std::size_t frameCasterIndex = 0;

#ifdef IWXMVM_T5_HARNESS
        // the frame's places, kept once the frame is done (the gun on screen's come late)
        std::vector<PlacedLight> framePlaced;
        // (whether the gun on screen's skeleton was the game's already when a light read it)
        std::atomic<std::uint32_t> viewGunSkeletonsCached = 0, viewGunSkeletonsMade = 0;

        void KeepFramePlaced()
        {
            std::lock_guard lock(placedMutex);
            for (auto& each : framePlaced)
                placedLights.push_back(each);
            while (placedLights.size() > 4096)
                placedLights.pop_front();
            framePlaced.clear();
        }
#endif

        // A bone steadied by its light's Steady, as the bone camera's is: in its target's frame, by the target's ticks
        Types::TargetBone Steady(const Types::Actor& actor, Types::TargetBone bone, bool viewGun)
        {
            const auto& follow = *actor.light->follow;
            auto& steady = steadies[{actor.light->id, viewGun}];
            if (follow.steady <= 0.0f)
            {
                steady.Reset();
                return bone;
            }
            const auto tick = follow.target.kind == Types::BoneCameraTargetKind::Actor
                                  ? Components::Playback::GetActorTick()
                                  : Mod::GetGameInterface()->GetDemoInfo().gameTick;
            const auto at =
                steady.Step({bone.position, bone.axes}, Components::BoneCameraLogic::MakeFrame(bone.origin, bone.yaw),
                            tick, follow.steady, {follow.target, follow.bone});
            bone.position = at.position;
            bone.axes = at.axes;
            return bone;
        }

        // Whether a light may go by the gun on screen this frame: on a bone of the player seen in first person, not
        // set to its body
        bool MayGoByViewGun(const Types::Actor& actor)
        {
            const auto& follow = actor.light->follow;
            const auto cg = Structures::GetClientGlobals();
            return follow && follow->target.kind == Types::BoneCameraTargetKind::Player &&
                   follow->firstPerson != Types::LightFirstPerson::Body && cg && !cg->renderingThirdPerson &&
                   cg->predictedPlayerState.clientNum == follow->target.id;
        }

        // A spot made again in its place, as AddSpot makes it, out of the scene again; nothing when it isn't made
        std::optional<GfxLight> MakeSpot(const State& state, const LightLogic::Placed& placed)
        {
            const auto index = GetAddedLightCount();
            if (!AddSpot(state, placed))
                return std::nullopt;
            auto made = GetAddedLights()[index];
            GetAddedLightCount() = index;
            return made;
        }
    }  // namespace

    void BeginFrame()
    {
#ifdef IWXMVM_T5_HARNESS
        KeepFramePlaced();
#endif
        frameLights.clear();
        frameLightCount = 0;
        frameShadowed = 0;
        viewGunLights.clear();
        frameCaster.reset();
    }

    void AddToScene(const std::vector<Types::Actor>& scene, std::uint32_t tick)
    {
        statPass = statBatches = 0;
        // a recording's Steady starts from the bones as they are, so a recording comes out the same each time
        const bool capturing = Components::CaptureManager::Get().IsCapturing();
        if (capturing && !wasCapturing)
            steadies.clear();
        wasCapturing = capturing;
        if (unloaded || std::ranges::none_of(scene, [](const Types::Actor& actor) { return actor.light.has_value(); }))
            return;
        const auto keys = Components::Lights::GetKeys();
        std::vector<std::optional<State>> states(scene.size());
        for (std::size_t i = 0; i < scene.size(); ++i)
        {
            if (!scene[i].light)
                continue;
            const auto found = keys ? keys->find(scene[i].light->id) : decltype(keys->end()){};
            states[i] = LightLogic::GetState(scene[i], keys && found != keys->end() ? found->second : LightLogic::Keys{},
                                             tick, Components::Playback::GetTimelineTick());
        }

        const auto shadowed = LightLogic::GetShadowed(scene, states);
        const auto findPlace = [&](const Types::Actor& actor) {
            if (const auto& follow = actor.light->follow)
            {
                if (const auto bone = Mod::GetGameInterface()->GetTargetBone(follow->target, follow->bone))
                {
                    const auto steadied = Steady(actor, *bone, false);
                    return followed[actor.light->id] = LightLogic::Place(actor, steadied.position, steadied.axes);
                }
                // what it follows isn't there now (an actor between its clips, a player gone): it stays where it last
                // was, as the bone camera does, not back at where it was first put
                if (const auto last = followed.find(actor.light->id); keepsFollowed && last != followed.end())
                    return last->second;
            }
            return LightLogic::Place(actor);
        };
        // (each with its keyed place; one that may go by the gun on screen is kept to place again)
        const auto place = [&](const Types::Actor& placedActor, const State& state, bool caster) {
            const auto actor =
                Components::Lights::WithKeyedPlacement(placedActor, Components::Playback::GetTimelineTick());
            const auto placed = findPlace(actor);
            if (MayGoByViewGun(actor))
                viewGunLights.push_back({actor, state, frameLights.size(), caster});
#ifdef IWXMVM_T5_HARNESS
            framePlaced.push_back({actor.light->id, tick, placed.position, placed.direction, capturing});
#endif
            return placed;
        };
        for (std::size_t i = 0; i < scene.size(); ++i)
        {
            if (!states[i] || (shadowed && scene[i].id == *shadowed))
                continue;
            if (states[i]->kind == Types::LightKind::Point)
            {
                const auto placed = place(scene[i], *states[i], false);
                frameLights.push_back(MakePoint(*states[i], placed));
                continue;
            }
            const auto light = MakeSpot(*states[i], place(scene[i], *states[i], false));
            if (!light)
            {
                // (not one of the frame's after all)
                if (!viewGunLights.empty() && viewGunLights.back().index == frameLights.size())
                    viewGunLights.pop_back();
                continue;
            }
            frameLights.push_back(*light);
            frameLights.back().canUseShadowMap = 0;
        }

        // the shadowed spot last: the game's call keeps the planes that cull what casts on it, one set for the scene
        const auto caster = shadowed ? std::ranges::find(scene, *shadowed, &Types::Actor::id) : scene.end();
        if (caster != scene.end())
        {
            const auto& state = *states[caster - scene.begin()];
            const auto index = GetAddedLightCount();
            const auto placed = place(*caster, state, true);
            if (AddSpot(state, placed))
            {
                auto& added = GetAddedLights()[index];
                added.canUseShadowMap = 1;
                // (the shadows' chooser leaves out a light farther than this from the camera)
                added.cullDist = std::numeric_limits<std::int16_t>::max();
                frameCasterIndex = frameLights.size();
                frameCaster = {state, placed};
                frameLights.push_back(added);
                // the game takes the first light added for the one spot that casts shadows
                if (index > 0)
                    std::swap(added, GetAddedLights()[0]);
                frameShadowed = caster->id;
                // and chooses who has shadow maps only as time moves: paused, a spot would never get one
                *reinterpret_cast<std::int32_t*>(GetGameAddresses().shadowChooserLastTime()) -= SHADOW_CHOOSER_STEP;
            }
            else if (!viewGunLights.empty() && viewGunLights.back().shadowed)
            {
                viewGunLights.pop_back();
            }
        }
        frameLightCount = static_cast<std::uint32_t>(frameLights.size());
        statPass = static_cast<std::int32_t>(frameLights.size());
        statBatches = (statPass + VIEW_POINT_LITS - 1) / VIEW_POINT_LITS;
        shadowedLight = frameShadowed;
    }

    void OnViewGunAdded()
    {
        if (viewGunLights.empty() || unloaded)
            return;
        const auto lights = std::exchange(viewGunLights, {});
        const auto obj = Functions::Com_GetClientDObj(VIEW_GUN_HANDLE, 0);
        const auto pose = Functions::CG_GetPose(0, VIEW_GUN_HANDLE);
        if (!obj || !pose || obj->numModels == 0)
            return;
        // its bones: the arms' (its first model) and the gun's (the others)
        const auto arms = AnimationPose::GetBoneNames(*obj->models[0]);
        std::vector<std::string> gun;
        for (std::uint32_t i = 1; i < obj->numModels; ++i)
            for (auto& name : AnimationPose::GetBoneNames(*obj->models[i]))
                gun.push_back(std::move(name));

        bool spotMade = false, casterMoved = false;
        for (const auto& each : lights)
        {
            const auto& follow = *each.actor.light->follow;
            const auto bone = LightLogic::GetViewGunBone(follow.firstPerson, follow.bone, arms, gun);
            float axis[3][3] = {}, origin[3] = {};
#ifdef IWXMVM_T5_HARNESS
            const auto stamp =
                *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(obj) + 0x50);
#endif
            if (!bone || !Functions::CG_DObjGetWorldTagMatrix(pose, obj, Bones::GetScriptString(*bone), axis, origin))
                continue;
#ifdef IWXMVM_T5_HARNESS
            const auto after =
                *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(obj) + 0x50);
            ++(after == stamp ? viewGunSkeletonsCached : viewGunSkeletonsMade);
#endif
            const auto steadied = Steady(
                each.actor,
                {glm::make_vec3(origin), glm::make_mat3(&axis[0][0]), glm::make_vec3(pose->origin), pose->angles[1]},
                true);
            const auto placed = followed[each.actor.light->id] =
                LightLogic::Place(each.actor, steadied.position, steadied.axes);
#ifdef IWXMVM_T5_HARNESS
            for (auto& kept : framePlaced)
                if (kept.id == each.actor.light->id)
                {
                    kept.position = placed.position;
                    kept.direction = placed.direction;
                    kept.viewGun = true;
                }
#endif
            if (each.shadowed)
            {
                frameCaster->second = placed;
                casterMoved = true;
            }
            else if (each.state.kind == Types::LightKind::Point)
            {
                frameLights[each.index] = MakePoint(each.state, placed);
            }
            else if (const auto light = MakeSpot(each.state, placed))
            {
                frameLights[each.index] = *light;
                frameLights[each.index].canUseShadowMap = 0;
                spotMade = true;
            }
        }

        // the shadowed spot made again last, in the first place the game takes it from
        if (frameCaster && (spotMade || casterMoved))
        {
            const auto index = GetAddedLightCount();
            if (AddSpot(frameCaster->first, frameCaster->second))
            {
                auto made = GetAddedLights()[index];
                made.canUseShadowMap = 1;
                made.cullDist = std::numeric_limits<std::int16_t>::max();
                GetAddedLights()[0] = made;
                GetAddedLightCount() = index;
                frameLights[frameCasterIndex] = made;
            }
        }
    }

    FrameStats GetFrameStats()
    {
        return {.added = statAdded,
                .visible = statVisible,
                .pass = statPass,
                .batches = statBatches,
                .litSurfaces = statLitSurfaces,
                .shadowMap = statShadowMap,
                .exposure = statExposure,
                .shadowed = shadowedLight};
    }

#ifdef IWXMVM_T5_HARNESS
    std::vector<PlacedLight> GetPlacedLights()
    {
        std::lock_guard lock(placedMutex);
        return {placedLights.begin(), placedLights.end()};
    }

    void SetKeepsFollowed(bool on)
    {
        keepsFollowed = on;
    }

    std::pair<std::uint32_t, std::uint32_t> GetViewGunSkeletons()
    {
        return {viewGunSkeletonsCached.load(), viewGunSkeletonsMade.load()};
    }
#endif

    namespace
    {
        // the gun on screen once the game has posed it (see OnViewGunAdded)
        typedef void(__cdecl* CG_AddPlayerWeapon_t)(int localClientNum, const void* placement, const void* ps,
                                                    void* cent, int drawGun);
        CG_AddPlayerWeapon_t CG_AddPlayerWeapon_Trampoline;
        void __cdecl CG_AddPlayerWeapon_Hook(int localClientNum, const void* placement, const void* ps, void* cent,
                                             int drawGun)
        {
            CG_AddPlayerWeapon_Trampoline(localClientNum, placement, ps, cent, drawGun);
            if (ps && drawGun && localClientNum == 0)
                OnViewGunAdded();
        }
    }  // namespace

    void Install()
    {
        HookManager::CreateHook(GetGameAddresses().CG_AddPlayerWeapon(), (std::uintptr_t)CG_AddPlayerWeapon_Hook,
                                (std::uintptr_t*)&CG_AddPlayerWeapon_Trampoline);
        HookManager::CreateHook(GetGameAddresses().R_GetVisibleDLights(), (std::uintptr_t)R_GetVisibleDLights_Hook,
                                (std::uintptr_t*)&R_GetVisibleDLights_Trampoline);
        HookManager::CreateHook(GetGameAddresses().R_EmitPointLitSurfs(), (std::uintptr_t)R_EmitPointLitSurfs_Hook,
                                &R_EmitPointLitSurfs_Trampoline);
        HookManager::CreateHook(GetGameAddresses().R_DrawPointLitSurfs(), (std::uintptr_t)R_DrawPointLitSurfs_Hook,
                                &R_DrawPointLitSurfs_Trampoline);
        HookManager::CreateHook(GetGameAddresses().R_DrawPointLitSurfsCallback(),
                                (std::uintptr_t)R_DrawPointLitSurfsCallback_Hook,
                                &R_DrawPointLitSurfsCallback_Trampoline);
        HookManager::CreateHook(GetGameAddresses().R_SetQuadMesh(), (std::uintptr_t)R_SetQuadMesh_Hook,
                                &R_SetQuadMesh_Trampoline);
    }

    void Unload()
    {
        unloaded = true;
        BeginFrame();
        followed.clear();
        steadies.clear();
        // (as the game had it)
        *reinterpret_cast<void**>(GetGameAddresses().spotLightDef()) = nullptr;
    }
}  // namespace IWXMVM::T5::Lights
