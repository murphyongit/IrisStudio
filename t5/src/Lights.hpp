#pragma once
#include "Types/Actor.hpp"

// The scene's lights in the renderer. The game's own way has four lights, shared with its
// flashes, one of them a spot that lights only the ground; its additive pass, one draw of the lit surfaces per light,
// has no such limit and lights everything, but nothing feeds it in this game. The mod feeds it with these. One spot
// also goes through the scene, for the shadow map the game makes for its first spot.
namespace IWXMVM::T5::Lights
{
    // Before the frame adds anything: on the main thread, as the rest
    void BeginFrame();
    // The scene's lights on at the tick: once its actors are posed, as a light can follow their bones
    void AddToScene(const std::vector<Types::Actor>& scene, std::uint32_t tick);
    // Once the game posed the gun on screen (CG_AddPlayerWeapon for the view): the lights on the viewed player's bones
    // that go by it in first person are placed again from it
    void OnViewGunAdded();

    // (the harness's) what the renderer did with the frame drawn last, and those of the scene on at its tick
    struct FrameStats
    {
        std::int32_t added = 0;        // the game's own, and the shadowed spot
        std::int32_t visible = 0;      // of those, in the view
        std::int32_t pass = 0;         // lights given to the additive pass
        std::int32_t batches = 0;      // draws of the pass (it takes four lights at a time)
        std::int32_t litSurfaces = 0;  // surfaces each light of the pass draws
        bool shadowMap = false;        // the shadowed spot has its shadow map
        float exposure = 0.0f;         // the map's
        std::uint32_t shadowed = 0;    // the light that casts shadows, its id; 0 for none
    };
    FrameStats GetFrameStats();
#ifdef IWXMVM_T5_HARNESS
    // (the harness's) where the lights were placed in the latest frames, the latest last
    struct PlacedLight
    {
        std::uint32_t id = 0;
        std::uint32_t tick = 0;
        glm::vec3 position{0.0f};
        glm::vec3 direction{0.0f};
        bool capturing = false;
        bool viewGun = false;  // placed by the gun on screen
    };
    std::vector<PlacedLight> GetPlacedLights();
    // off for comparing: a light on a bone that's gone goes back where it was put
    void SetKeepsFollowed(bool on);
    // how often the gun on screen's skeleton was the game's already when a light read it, and how often it was made
    // then
    std::pair<std::uint32_t, std::uint32_t> GetViewGunSkeletons();
#endif
    // the view gun's DObj and pose (CG_GetPose's handle from 0x600 on): the gun the film's player holds in first person
    inline constexpr int VIEW_GUN_HANDLE = 0x600;

    void Install();
    void Unload();
}  // namespace IWXMVM::T5::Lights
