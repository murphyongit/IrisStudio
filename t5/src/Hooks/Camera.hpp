#pragma once
#include "Types/BoneCamera.hpp"
#include "Types/DrawnView.hpp"

namespace IWXMVM::T5::Hooks::Camera
{
    void Install();
    void OnCameraChanged();

    // whether this is the thread the game builds its frames on
    bool IsMainThread();

    // The game's view as the render thread draws a frame with it, read from the start of the frame's view (GfxViewInfo
    // starts with GfxViewParms), so it is the frame's own: the camera has moved on by then
    struct DrawnView
    {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        glm::mat4 viewProjection{1.0f};
        glm::vec3 origin{0.0f};
        double time = 0.0;        // when, in ms
        std::uint32_t views = 0;  // how many views the frame had so far
    };
    // render thread, for each view of a frame
    void OnViewDrawn(const std::uint8_t* viewInfo);
    // render thread: the view of the frame drawn last, as the game had it and as the mod's drawing takes it
    std::optional<DrawnView> GetDrawnMatrices();
    std::optional<Types::DrawnView> GetDrawnView();

    // main thread: where the dolly is at a timeline tick, from the campath the frame uses; none without one
    std::optional<glm::vec3> GetDollyPosition(std::uint32_t timelineTick);

    // Whether the frame shows the demo player's own screen (their vision and screen filters): only in first person.
    // Kept leaves the game's own vision or filters in every view, for the harness to compare.
    bool ShowsPlayerScreen();
    void KeepPlayerScreen(bool vision, bool filters);
    // main thread: the map's vision kept from before a shellshock's took the naked one over (empty before any)
    std::vector<std::uint8_t> GetMapVision();
    // cg_drawShellshock (the shellshocks' white flash and blur) as the Look tab has it in first person, off in every
    // other view
    void ApplyShellshock();

    // What the bone camera was placed from in one of the latest frames and where the view was rendered from, for the
    // harness to check that the camera is on the bone as drawn
    struct BoneCameraFrame
    {
        std::uint32_t tick = 0;  // of what it follows
        Types::BoneCameraSettings settings;  // with its keys at the frame's timeline tick
        std::optional<Types::TargetBone> placedFrom;  // the bone as drawn
        glm::vec3 position{0.0f};                     // where the camera was put
        glm::vec3 angles{0.0f};
        std::optional<glm::vec3> rendered;  // where the frame's view was as the next frame started
        std::uint32_t timelineTick = 0;
        bool capturing = false;
        Types::BoneCameraSettings fromUi;  // the settings as the UI's copy had them (its keys at the UI's tick)
        double time = 0.0;                 // real time, in ms
        std::uint32_t views = 0;           // the views built in demos when it was placed (the next one takes it)
    };
    // the latest frames', the latest last
    std::vector<BoneCameraFrame> GetBoneCameraFrames();
    // A view built with the bone camera (CG_CalcViewValues), the views built in demos counting it
    struct BuiltView
    {
        std::uint32_t views = 0;
        glm::vec3 position{0.0f};
        std::uint32_t timelineTick = 0;
        double time = 0.0;
    };
    std::vector<BuiltView> GetBuiltViews();
    // the views the render thread drew lately (each frame's first), with their times in ms
    std::vector<std::pair<double, glm::vec3>> GetDrawnViews();
    // off for the harness's comparisons: the camera exactly on the bone when slowed down too
    void SetBoneCameraGlide(bool on);
    // off for the harness's comparisons: the view built from the render thread's camera, as before the hand-over
    void SetBoneCameraHandOver(bool on);
    // main thread, once the frame's players and actors were posed and added to the scene: places the bone camera
    void OnEntitiesAdded();
}  // namespace IWXMVM::T5::Hooks::Camera
