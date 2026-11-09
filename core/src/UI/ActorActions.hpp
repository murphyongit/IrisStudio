#pragma once
#include "Components/ActorLogic.hpp"

namespace IWXMVM::UI::ActorActions
{
    // What the Actors page and the view's menu do to actors, each one step to undo, at the timeline's playhead

    // the starting points in the order the menus list them, with the names they show them by and what each does
    inline constexpr std::array STARTS{ActorLogic::Start::Standing, ActorLogic::Start::WalkPath,
                                       ActorLogic::Start::RunPath, ActorLogic::Start::Shoot, ActorLogic::Start::Die};
    // a dog's: all but shooting
    inline constexpr std::array DOG_STARTS{ActorLogic::Start::Standing, ActorLogic::Start::WalkPath,
                                           ActorLogic::Start::RunPath, ActorLogic::Start::Die};
    const char* GetStartLabel(ActorLogic::Start start);
    const char* GetStartTooltip(ActorLogic::Start start);
    // the name of a starting point for the harness: standing, walk, run, shoot, die
    const char* GetStartName(ActorLogic::Start start);
    // why a starting point can't be used now (e.g. the game has no walk, or a dog to add), none when it can
    std::optional<std::string> GetStartUnavailableReason(ActorLogic::Start start,
                                                         Types::ActorKind kind = Types::ActorKind::Human);

    // Adds an actor of the default character (a dog: the map's german shepherd) from a starting point (see
    // ActorLogic::MakeStart) on the ground where the ray hits, facing the camera, selected and shown on the Scene tab;
    // those that walk a path go on to placing its points (Click to place), the walk kept as long as the path
    void Add(ActorLogic::Start start, glm::vec3 origin, glm::vec3 direction,
             Types::ActorKind kind = Types::ActorKind::Human);
    // Add Actor, Add Dog: an actor standing on the ground where the ray hits, idling,
    // facing the camera, selected: a person of the map's own multiplayer character (never a character zone's or the
    // campaign's), a dog the map's german shepherd
    void AddActor(glm::vec3 origin, glm::vec3 direction, Types::ActorKind kind = Types::ActorKind::Human);
    // why it can't be added now, none when it can
    std::optional<std::string> GetAddUnavailableReason(Types::ActorKind kind = Types::ActorKind::Human);

    // Add Effect: one of the map's effects on the surface the ray hits,
    // facing out of it, from the playhead for as long as it plays by itself, selected; it becomes the latest of the
    // recent effects
    void AddEffect(const std::string& effect, glm::vec3 origin, glm::vec3 direction);
    // why it can't be added now (the game hasn't it), none when it can
    std::optional<std::string> GetEffectUnavailableReason(std::string_view effect);

    // Add a prop: a model of the map standing upright on the surface the ray hits,
    // facing the camera, its bar from the playhead to the end of the demo (DG1), selected; it becomes the
    // latest of the recent props
    void AddProp(const std::string& model, glm::vec3 origin, glm::vec3 direction);
    // A prop's place keyed at the playhead, as a change of its row keys it: its position, its turn (each
    // angle the one nearest the key before it), the edit one step named step
    void KeyPropPlace(std::uint32_t id, std::optional<glm::vec3> position, std::optional<glm::vec3> turn,
                      const std::string& step);
    // A prop moved by its gizmo to where placed is: what has keys is keyed at the playhead, the rest is set on actor
    void MoveProp(Types::Actor& actor, const Types::Actor& placed, const std::string& step);
    // The yaw that faces the camera, as a prop or killstreak added where it looks has
    float GetYawFacingCamera();

    // Add Light: a point off the surface the ray hits, or a spot where the camera is, aimed
    // at it, from the playhead, selected
    void AddLight(Types::LightKind kind, glm::vec3 origin, glm::vec3 direction);
    // A light's place keyed at the playhead, as a change of its row keys it:
    // its position, its aim (its yaw the one nearest the key before it), the edit one step named step
    void KeyLightPlace(std::uint32_t id, std::optional<glm::vec3> position, std::optional<glm::vec3> aim,
                       const std::string& step);
    // A light moved by its gizmo to where placed is: what has keys is keyed at the playhead, the rest is set on actor
    void MoveLight(Types::Actor& actor, const Types::Actor& placed, const std::string& step);

    // Adds a killstreak from its starting point (KillstreakLogic::MakeStart) on the ground where the ray hits (a flying
    // one as high above it as it flies, a plane's pass over it halfway), facing the camera, selected; it drives or
    // flies a path once given one (Fly here, Drive here, its Movement)
    void AddKillstreak(Types::Killstreak killstreak, glm::vec3 origin, glm::vec3 direction);
    // why it can't be added now (the map hasn't its model), none when it can
    std::optional<std::string> GetKillstreakUnavailableReason(Types::Killstreak killstreak);
    // Adds the Mortar Team: its three points about the ground where the
    // ray hits, its shells coming in the way the camera looks, its Fire at the playhead (the first lands then),
    // selected
    void AddMortar(glm::vec3 origin, glm::vec3 direction);
    // why it can't be added now (the map hasn't its shell), none when it can
    std::optional<std::string> GetMortarUnavailableReason();
    // what its starting point does, for the menus' tooltips
    std::string GetKillstreakTooltip(Types::Killstreak killstreak);
    // its name for the harness: its name lower case without spaces or dashes (rcxd, gunship, carepackagechopper...)
    std::string GetKillstreakKey(Types::Killstreak killstreak);

    // A killstreak's Fire from the playhead: the event its guns or what it drops have
    // (KillstreakLogic::MakeFire), selected, or for one that goes off its Detonate death
    void AddKillstreakFire(std::uint32_t id);
    // why an actor can't fire, none when it can (a killstreak's, a dog's, a person's without a gun)
    std::optional<std::string> GetFireUnavailableReason(const Types::Actor& actor);

    // plays an animation from the playhead, or a tick (ActorLogic::PlayAt), which becomes one of the recent ones, joined
    // to the clips around it with the actor's Auto transitions; one of the face alone on the actor's Face row
    // (PlayFace: once, or looping up to the next face clip; not a dog's)
    void Play(std::uint32_t id, const std::string& animation, std::optional<std::uint32_t> tick = std::nullopt);
    // Then...: plays an animation after one of the actor's clips (Components::Actors::PlayAfter)
    void PlayAfter(std::uint32_t id, std::uint32_t clip, const std::string& animation);
    // a death moved on by what was put in before it (PlayAfter) back where it was, as part of the same step: what was
    // put in ends there, and its Hit goes with it
    void KeepDeathAt(std::uint32_t id, std::uint32_t death, std::uint32_t start);
    // the clip of an actor shown at the playhead (the last to start by then), which the view's Then... follows; none
    // before its first
    std::optional<std::uint32_t> GetClipAtPlayhead(const Types::Actor& actor);
    void PlayFace(std::uint32_t id, const std::string& animation, std::optional<std::uint32_t> tick = std::nullopt);
    // walks (or runs, with Locomotion Auto) from where it stands at the playhead to the ground the ray hits, and stands
    // there; with Auto transitions it sets off and stops as the game's do (Components::Actors::WalkTo); at a gait, the
    // stretch there goes at it
    void WalkTo(std::uint32_t id, glm::vec3 origin, glm::vec3 direction, bool run = false,
                std::optional<Types::ActorGait> gait = std::nullopt, bool crouched = false);
    // why it can't run there: as walking, and without Locomotion Auto
    std::optional<std::string> GetRunUnavailableReason(std::uint32_t id);
    // walks on from where it stands at the playhead along the points clicked in the view next (Click to place), at a
    // gait if given
    void WalkOn(std::uint32_t id, std::optional<Types::ActorGait> gait = std::nullopt, bool crouched = false);
    // why it can't walk (attached; by clicked points, with keys), none when it can
    std::optional<std::string> GetWalkUnavailableReason(std::uint32_t id, bool clickedPoints);
    // looks at the camera from the playhead (a Look event there)
    void LookAtCamera(std::uint32_t id);
    // is hit and dies at the playhead, with the death given, else the one Components::Actors::GetDeathAnimation finds
    void DieHere(std::uint32_t id, const std::string& animation = {});
    // the deaths the view's Die menu offers an actor: the one it would die with first, then the favourite and recent
    // deaths that fit it, each once
    std::vector<std::string> GetDeaths(const Types::Actor& actor);
    // a point of its path after the selected one (or the last) on the ground where the ray hits; with a gait given (as
    // placing points picks it with 1, 2, 3), the stretch to it goes at that gait (see ActorLogic::InsertWaypoint)
    void AddPathPoint(std::uint32_t id, glm::vec3 origin, glm::vec3 direction,
                      std::optional<std::pair<Types::ActorGait, bool>> gait = std::nullopt);
    // a point of its path after another, on the ground where the ray hits (the path line's Insert point here)
    void InsertPathPoint(std::uint32_t id, std::size_t after, glm::vec3 origin, glm::vec3 direction);
    // The point menu's: takes a point out of the path (it keeps one); ends the path at a
    // point (its points after it out); has it stand at a point a while before going on; moves a point to the ground
    // where the camera looks
    void DeletePathPoint(std::uint32_t id, std::size_t point);
    void EndPathAt(std::uint32_t id, std::size_t point);
    void SetWait(std::uint32_t id, std::size_t point, std::uint32_t wait);
    void MovePointToCrosshair(std::uint32_t id, std::size_t point);
    // the gait of the stretches from one point to another (excluded) of its path (see Components::Actors::SetGait)
    void SetGait(std::uint32_t id, std::size_t from, std::size_t to, Types::ActorGait gait, bool crouched);
    // puts it standing where the ray hits the ground (its path's first point, for one that walks one)
    void MoveHere(std::uint32_t id, glm::vec3 origin, glm::vec3 direction);
    void Duplicate(std::uint32_t id);
    // Duplicate beside, Run with: a copy BESIDE units to its left or right (its
    // path that far to the side, planned along it to arrive when it does), selected; or the actor going along another's
    // path beside it (on the side of it it stands on), at the gait and pace that get it to the end with it
    inline constexpr float BESIDE = 48.0f;
    void DuplicateBeside(std::uint32_t id, bool left);
    void RunWith(std::uint32_t id, std::uint32_t leaderId);
    void Delete(std::uint32_t id);
    // the name of a gait for the menus and buttons, as the actor's body has it (a dog's Run may be its trot)
    std::string GetGaitName(const Types::Actor& actor, Types::ActorGait gait, bool crouched);

    // once a frame: a walk is fitted to its path only while its points are placed
    void Update();
}  // namespace IWXMVM::UI::ActorActions
