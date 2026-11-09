#pragma once
#include "Types/Actor.hpp"
#include "Types/Loadout.hpp"
#include "Components/ActorLogic.hpp"
#include "Components/CharacterLogic.hpp"
#include "Components/AnimationCatalogLogic.hpp"
#include "Components/TransitionLogic.hpp"
#include "Components/VehicleLogic.hpp"

#include <set>

namespace IWXMVM::Components::Actors
{
    // The actors of the demo that is playing (see Types::Actor) and the content pack they come from. The game draws
    // them every frame from what is here, evaluated at the timeline's tick, so scrubbing, rewinding and capturing
    // show the same thing. A demo's actors are saved as it changes and come back when it loads again.

    void Initialize();
    // saves a change made in the last moment, which is saved 500 ms after the last one otherwise (as the mod unloads)
    void SaveIfChanged();

    // The game sets what it offers whenever it changes (another map or pack), on its own thread
    void SetCatalog(std::shared_ptr<const Types::ActorCatalog> catalog);
    std::shared_ptr<const Types::ActorCatalog> GetCatalog();

    std::vector<Types::Actor> GetActors();
    std::optional<Types::Actor> GetActor(std::uint32_t id);

    // Adds an actor (its id and, if empty, its name are given here) and returns its id
    std::uint32_t Add(Types::Actor actor);
    // a name no actor has: the base and the first number free ("Dog 2")
    std::string MakeName(std::string base);
    std::uint32_t Duplicate(std::uint32_t id);
    // with sameStep, the change is part of the last undo step (the game placing what the UI asked for); the step is
    // named by the label ("delete clip"), else "change" and the actor's name
    void Update(const Types::Actor& actor, bool sameStep = false, std::string_view label = {});
    void Remove(std::uint32_t id);
    void Clear();
    // puts the actors in that order of their ids, which all of them are in, as a step to undo
    void Reorder(std::span<const std::uint32_t> order);

    // Undoes or redoes the last step of changes to the actors; returns whether there was one. The changes made until
    // EndEditStep are one step: the UI ends it whenever nothing is being dragged or typed. Each step is one of the
    // editor's undo history (UndoHistory), which calls these.
    bool Undo();
    bool Redo();
    bool CanUndo();
    bool CanRedo();
    void EndEditStep();

    // changes whenever the actors do, so the game can keep what it built for them until then
    std::uint32_t GetGeneration();
    // whether the demo's scene is still to be read from its file (it is once the demo plays, a few seconds after its
    // load)
    bool IsSceneLoadPending();

    // the actor the UI edits and the gizmo moves, and the clip of it the UI edits
    std::optional<std::uint32_t> GetSelected();
    void Select(std::optional<std::uint32_t> id);
    std::optional<std::uint32_t> GetSelectedClip();
    void SelectClip(std::optional<std::uint32_t> clip);
    // or the clip of its Face row the UI edits (one or the other is selected)
    std::optional<std::uint32_t> GetSelectedFaceClip();
    void SelectFaceClip(std::optional<std::uint32_t> clip);

    // Adds a clip to an actor and returns its id, 0 when there is no such actor. A clip playing where it starts is cut
    // there; with the actor's Auto transitions, it is joined to the clips around it (see JoinAround).
    std::uint32_t AddClip(std::uint32_t actorId, Types::ActorClip clip);
    // With the actor's Auto transitions, joins one of its clips to the clips before and after it with the game's
    // transitions for its body (see TransitionLogic::JoinAround); returns whether its clips changed
    bool JoinAround(Types::Actor& actor, std::uint32_t clipId);
    // Turns an actor's Auto transitions on, joining each of its clips to the next, or off, taking out the transitions
    // it put in
    void SetAutoTransitions(Types::Actor& actor, bool on);
    // the movement set an actor's Auto stands for (its own otherwise): the AI's low ready on a campaign body, else the
    // players'
    Types::ActorMovementSet GetMovementSet(const Types::Actor& actor);
    // Then...: what can follow one of an actor's clips (see TransitionLogic::ChainCandidates), of those that fit its
    // models as the fit filter has it; and playing one after it (TransitionLogic::PlayAfter), returning the new clip's
    // id (0 for none)
    std::vector<TransitionLogic::Candidate> GetChainCandidates(const Types::Actor& actor, std::uint32_t clipId,
                                                              std::size_t most = 40);
    std::uint32_t PlayAfter(Types::Actor& actor, std::uint32_t clipId, const std::string& animation);
    // the same on its Face row
    std::uint32_t AddFaceClip(std::uint32_t actorId, Types::ActorClip clip);

    // What an actor shows at a timeline tick; empty for its rest pose. The samples point into the actor's clips.
    std::vector<Types::ActorAnimationSample> Evaluate(const Types::Actor& actor, std::uint32_t tick);
    // the same with the legs under its clips of upper body animations and of its locomotion, as the game stands and
    // walks them for its body (a person with a gun of a class, a dog; see ActorLogic::GetLayers)
    ActorLogic::Layers GetLayers(const Types::Actor& actor, std::uint32_t tick, const ActorLogic::Body& body);
    // the animations the actor's legs may play for its body (see ActorLogic::GetLegsLibrary), and its face (see
    // ActorLogic::GetFaceLibrary; none for a dog)
    std::vector<std::string> GetLegsLibrary(const Types::Actor& actor, const ActorLogic::Body& body);
    std::vector<std::string> GetFaceLibrary(const Types::Actor& actor);

    // What an actor is (see Types::ActorKind): a killstreak's model makes it a vehicle (see KillstreakLogic); else as its
    // body's bones say once the game has had the model loaded, else as its character's kind says (see
    // CharacterLogic::GetActorKind), else a person
    Types::ActorKind GetActorKind(const Types::Actor& actor);
    // how high above the ground a flying killstreak's points are put (Types::Actor::flyHeight), 0 for other actors
    float GetFlyHeight(const Types::Actor& actor);
    // a killstreak's body at a tick: its tilt, drift, wheels and steering as it moves then (see VehicleLogic::GetBody),
    // none for other actors
    std::optional<VehicleLogic::Body> GetVehicleBody(const Types::Actor& actor, std::uint32_t tick);
    // What its locomotion and legs play (see ActorLogic::Body): a person's with the class of the gun the game built it
    // with (see SetGunClass), or that given; a dog's of its breed: Zombies' hellhound for a dog of a Zombies map, else
    // the shepherd
    ActorLogic::Body GetBody(const Types::Actor& actor);
    ActorLogic::Body GetBody(const Types::Actor& actor, Types::GunClass gun);
    // the body of a character of the catalog
    ActorLogic::Body GetBody(const Types::Character& character, const Types::ActorCatalog& catalog);
    // The class of the gun in an actor's hand as the game built it, which its legs stand and walk with; the UI shows
    // what they play
    void SetGunClass(std::uint32_t id, Types::GunClass gun);
    Types::GunClass GetGunClass(std::uint32_t id);

    // Where an actor stands at a timeline tick: its position, between its keys, or as far along its path as its
    // moving clips have walked it by then (which the game may still put on the ground), moved from there by its clips'
    // root motion (see ActorLogic::EvaluatePlacement); for an attached actor, where the game last drew it
    Types::ActorPlacement EvaluatePlacement(const Types::Actor& actor, std::uint32_t tick);
    // the game tells where it drew an attached actor, which only it can work out from the bone it is attached to
    void SetDrawnPlacement(std::uint32_t id, const Types::ActorPlacement& placement);

    // What the game left out of an actor as it built it, as its models and gun together have more bones than the game
    // can draw (see ActorLogic::FitBones), and how many they have; the UI says so
    struct LeftOut
    {
        std::vector<std::string> models;  // of the actor's
        bool gun = false;
        bool stowed = false;      // the gun on its back
        std::uint32_t bones = 0;  // of all of them together
    };
    void SetLeftOut(std::uint32_t id, LeftOut leftOut);
    // none when nothing was left out
    std::optional<LeftOut> GetLeftOut(std::uint32_t id);

    // Moves an actor to stand there at a tick: its position, its key at that tick (added if need be), or its whole
    // path
    void ApplyPlacement(Types::Actor& actor, std::uint32_t tick, glm::vec3 position, float yaw);

    // How far an animation moves in one play (its root motion: x forward, y to the left), which the game measures
    // once it has built a tree with the animation. Paths are walked at these speeds and angles.
    void SetAnimationMotion(const std::string& animation, glm::vec2 motionPerPlay);
    std::optional<glm::vec2> GetAnimationMotion(const std::string& animation);
    // what is known of the animations the game has loaded, as ActorLogic needs it (the catalog's, the motion measured)
    ActorLogic::AnimationLookup GetAnimationLookup();
    // How an animation moves the root through a play (see ActorLogic::RootMotion), which the game measures once it has
    // built a tree with the animation, for the clips that move their actor by it
    void SetAnimationRootMotion(const std::string& animation, std::shared_ptr<const ActorLogic::RootMotion> motion);
    std::shared_ptr<const ActorLogic::RootMotion> GetAnimationRootMotion(const std::string& animation);
    // how one of an actor's clips walks it along a path (see ActorLogic::GetClipWalk), and whether it moves it by its
    // root motion (ActorLogic::GetRootMotionUse)
    ActorLogic::ClipWalk GetClipWalk(const Types::Actor& actor, const Types::ActorClip& clip);
    ActorLogic::RootMotionUse GetRootMotionUse(const Types::Actor& actor, const Types::ActorClip& clip);
    // an actor's path as it walks it, with its corners rounded (see ActorLogic::RoundPath): its length, and points
    // along it for drawing it
    float GetPathLength(const Types::Actor& actor);
    std::vector<glm::vec3> GetPathLine(const Types::Actor& actor);

    // Whether an actor is drawn at a tick: unless it is hidden in the editor, as its last Show or Hide event before
    // then says, and before the first of those the opposite of what it does
    bool IsVisible(const Types::Actor& actor, std::uint32_t tick);

    // what an actor looks at at a tick (see ActorLogic::GetLook)
    ActorLogic::LookState GetLook(const Types::Actor& actor, std::uint32_t tick);

    // The events (and shots of Fire events) that go off as the timeline plays from one tick (excluded) to another
    using FiredEvent = ActorLogic::FiredEvent;
    std::vector<FiredEvent> GetFiredEvents(const Types::Actor& actor, std::uint32_t from, std::uint32_t to);

    // The notetracks of an actor's clips the timeline passes playing from one tick (excluded) to another, and the
    // reloads of its gun whose magazine leaves the gun at most at a tick, with what each clip's animation does as a
    // lookup says (see ActorLogic, and Components::ReloadProfiles for the lookup)
    using PassedNotetrack = ActorLogic::PassedNotetrack;
    std::vector<PassedNotetrack> GetPassedNotetracks(const Types::Actor& actor, std::uint32_t from, std::uint32_t to);
    std::vector<ActorLogic::Reload> GetReloads(const Types::Actor& actor, std::uint32_t until,
                                               const ActorLogic::ReloadMomentsLookup& moments);

    // Adds an event to an actor and returns its id, 0 when there is no such actor
    std::uint32_t AddEvent(std::uint32_t actorId, Types::ActorEvent event);
    std::optional<std::uint32_t> GetSelectedEvent();
    void SelectEvent(std::optional<std::uint32_t> event);

    // The weapon variant an actor holds (e.g. ak47_acog_mp): its weapon with the attachments of them it can have
    // together; empty for none
    std::string ResolveWeapon(const Types::Actor& actor);
    // the variant of the gun on its back, empty for none
    std::string ResolveStowedWeapon(const Types::Actor& actor);
    // a Fire for a person, at a tick, as its gun fires (see ActorLogic::MakeGunFire)
    Types::ActorEvent MakeGunFire(const Types::Actor& actor, std::uint32_t tick);

    // the point of the selected actor's path the UI edits and the gizmo moves
    std::optional<std::size_t> GetSelectedWaypoint();
    void SelectWaypoint(std::optional<std::size_t> waypoint);

    // what an animation is (see AnimationCatalogLogic::GetCategory: "Movement" for ai_walk_forward, "Faces" for those
    // that move the face alone), and the categories in the order to list them
    std::string_view GetAnimationCategory(const Types::ActorCatalog::Animation& animation);
    const std::vector<std::string_view>& GetAnimationCategories();

    // The catalog of every animation the game has, which comes with the mod (resources/animations.json, written by
    // tools/t5-characters/animations.py); empty when it doesn't read. Of its animations the game has those of
    // multiplayer loaded, and the campaign's common ones once a campaign character is.
    const Types::AnimationCatalog& GetAnimationCatalog();
    // The animations the browser lists (see AnimationCatalogLogic::List) with the game's catalog as it is now, which
    // the list keeps; none before the game has one
    struct AnimationList : AnimationCatalogLogic::AnimationList
    {
        std::shared_ptr<const Types::ActorCatalog> game;
    };
    std::shared_ptr<const AnimationList> GetAnimationList();
    // Whether each of the catalog's animations fits models (see AnimationCatalogLogic::Fits), in the catalog's order;
    // none when the game hasn't loaded one of the models (so it can't tell their bones). A model's bones are kept once
    // the game has had it loaded.
    std::shared_ptr<const std::vector<bool>> GetFits(const std::vector<std::string>& models);
    // Whether an animation fits models: none when that isn't known (see GetFits); one the catalog doesn't have fits
    std::optional<bool> Fits(const std::vector<std::string>& models, std::string_view animation);
    // why it doesn't (see AnimationCatalogLogic::DescribeMisfit), empty when it fits or that isn't known
    std::string DescribeMisfit(const std::vector<std::string>& models, std::string_view animation);
    // forgets the models' bones and what fits them, as if the game had never had them loaded (for the harness)
    void ForgetModels();
    // whether models the game hasn't loaded yet are on their way (from their character zones, or a pack loading), so
    // which animations fit them is known in a moment
    bool AreModelsLoading(const std::vector<std::string>& models);
    // Whether the lists of animations for an actor (the browser, the view's menu) show only those that fit its models
    // (F3); on as the mod starts
    bool IsFitFilterOn();
    void SetFitFilter(bool on);
    // The animations of the animation pack the store (T5's AnimationStore) has loaded now, which the game sets as they
    // change: those the browser shows as loaded (the others load as an actor plays them)
    using StoreLoaded = std::set<std::string, std::less<>>;
    void SetStoreLoaded(std::shared_ptr<const StoreLoaded> loaded);
    std::shared_ptr<const StoreLoaded> GetStoreLoaded();
    bool IsStoreLoaded(std::string_view animation);
    // whether an animation of the game's catalog is loaded now: the game's own, or the store's once it has loaded it
    // (as the store had loaded them, if given)
    bool IsAnimationLoaded(const Types::ActorCatalog::Animation& animation, const StoreLoaded* storeLoaded = nullptr);
    bool IsFavoriteAnimation(std::string_view name);
    void SetFavoriteAnimation(std::string_view name, bool favorite);

    // Where the UI wants an actor: the ground where a ray from the origin hits the world. The game works it out on
    // its own thread (TakePlacementRequests) and gives it to Place, which moves the actor there.
    struct PlacementRequest
    {
        std::uint32_t id;
        glm::vec3 origin;
        glm::vec3 direction;
        std::uint32_t tick;                   // where it stands then (see ApplyPlacement)
        std::optional<std::size_t> waypoint;  // a point of its path instead
        std::optional<float> yaw;             // which way it faces there, else as it did (walking, as it stops)
        bool walk = false;                    // it walks there from where it stands at the tick instead (WalkTo)
        bool run = false;                     // runs there instead
        std::optional<Types::ActorGait> gait;  // walking there at a gait (see WalkOptions)
        bool crouched = false;
        // an effect in the scene's: put on the first surface the ray hits, facing out of it,
        // not on the ground below it
        bool surface = false;
        // a spot's: it stays where it is and points at the surface the ray hits
        bool aim = false;
    };
    // (where it stands at the timeline's tick, or the tick given)
    void RequestPlacement(std::uint32_t id, glm::vec3 origin, glm::vec3 direction,
                          std::optional<std::size_t> waypoint = std::nullopt, std::optional<float> yaw = std::nullopt,
                          std::optional<std::uint32_t> tick = std::nullopt);
    // a spot points at the surface the ray hits
    void RequestAim(std::uint32_t id, glm::vec3 origin, glm::vec3 direction);
    // the actor walks (or runs) from where it stands at the timeline's tick to the ground the ray hits, and stands there
    // (see WalkTo; facing a way as it stops, if given), as one step to undo
    void RequestWalkTo(std::uint32_t id, glm::vec3 origin, glm::vec3 direction, bool run = false,
                       std::optional<float> face = std::nullopt, std::optional<Types::ActorGait> gait = std::nullopt,
                       bool crouched = false);
    std::vector<PlacementRequest> TakePlacementRequests();
    // the game found the ground a request's ray hits (or the surface, with its normal, for an effect's): does what it
    // asked, as part of the step that asked for it
    void Place(const PlacementRequest& request, glm::vec3 ground, glm::vec3 normal = {0.0f, 0.0f, 1.0f});

    // Has an actor walk from where it stands at a tick to a point (see ActorLogic::WalkTo) with the first walk the
    // game has (else a run), then stand with its idle; returns the walk clip's id, 0 when it can't, and the speed
    // the walk is guessed to go at until it is measured. With Locomotion Auto it may run instead (at its movement
    // set's run), and with Auto transitions it sets off and stops with the game's transitions, turning at the corners
    // of its path (see TransitionLogic::PlanWalk), stopping facing a way if given (degrees), else the way it went.
    struct WalkOptions
    {
        bool run = false;
        std::optional<float> face;
        // at a gait (a person's, a dog's): the stretch to the point goes at it (see ActorLogic::SetGait)
        std::optional<Types::ActorGait> gait;
        bool crouched = false;
    };
    std::uint32_t WalkTo(Types::Actor& actor, std::uint32_t tick, glm::vec3 target, float* guessedSpeed = nullptr,
                         const WalkOptions& options = {});

    // While the path of an actor that walks it from a starting point (or Walk to...) is placed point by point, its
    // walk clip is kept as long as the path takes (ActorLogic::FitWalk): as each point is placed, and once the walk's
    // animation is measured (a walk to a point is fitted again then too). Placing stops it.
    void FitWalkWhilePlacing(std::uint32_t id, std::uint32_t clip, float guessedSpeed);
    void StopFittingWalk();
    // fits the one clip that walks an actor's path to it, after its speed or locomotion changed (see
    // ActorLogic::FitOnlyWalk), with its path's transitions planned again (see TransitionLogic::PlanWalk), or taken
    // out without Auto transitions or Locomotion Auto; its one-offs taken out meanwhile and put back where their
    // anchors have them (ActorLogic::TakeOutOneOffs), those of `anchored` anchored first where they are now (moved by
    // the user, see ActorLogic::Anchor)
    void FitOnlyWalk(Types::Actor& actor, const std::set<std::uint32_t>& anchored = {});
    // A death made where the user didn't time it (a clip swapped to one, Then...) anchored as the re-plan will have the
    // path's timing (ActorLogic::AnchorMadeDeath): right after the walk or within reach before the arrival, it takes
    // over from the run as the actor arrives (it dies as it runs); later, it keeps its
    // time from the arrival; earlier, it cuts the run there
    void AnchorDeath(Types::Actor& actor, std::uint32_t clipId);
    // fits a walk along an actor's path to it (ActorLogic::FitWalk), its transitions planned again
    void FitWalk(Types::Actor& actor, std::uint32_t walkClip, float guessedSpeed);
    // has an actor whose path's transitions were planned before the game measured how they move it planned again once
    // it has (fitted again and planned, as one step to undo with the change that planned it)
    void ReplanOnceMeasured(const Types::Actor& actor);
    // changes an actor's movement set: a path walked at a speed preset is walked at the new set's preset of that gait,
    // with the new set's transitions
    void SetMovementSet(Types::Actor& actor, Types::ActorMovementSet set);
    // changes an actor (its body, its set) with its Auto transitions taken out, and puts in those of what it is then
    void Rechain(Types::Actor& actor, const std::function<void()>& change);
    // the actor whose walk is fitted as its path is placed, if any
    std::optional<std::uint32_t> GetWalkFittedActor();

    // Gaits (see ActorLogic). The gaits an actor's body goes at (none for a killstreak)
    std::vector<ActorLogic::SpeedPreset> GetGaits(const Types::Actor& actor);
    // the gait an actor goes along the stretch from a point of its path at, and its pace: its stretches', else (a path
    // from before gaits) the one its speed picks, at the pace that keeps that speed; none off a path, without
    // Locomotion Auto or for a killstreak
    struct GaitState
    {
        Types::ActorGait gait = Types::ActorGait::Walk;
        bool crouched = false;
        float pace = 1.0f;
        bool fromBefore = false;  // a path from before gaits
        float speed = 0.0f;       // units a second, at the pace
    };
    std::optional<GaitState> GetGait(const Types::Actor& actor, std::size_t point);
    // sets the gait of the stretches from one point to another (excluded) as ActorLogic::SetGait does, and the pace
    // of them all; the walk is fitted and planned again once the edit ends (see RunPendingReplans)
    void SetGait(Types::Actor& actor, std::size_t from, std::size_t to, Types::ActorGait gait, bool crouched);
    void SetPace(Types::Actor& actor, float pace);
    // gives an actor from before gaits its stretches (see ActorLogic::MakeStretches); returns whether it has them
    bool MakeStretches(Types::Actor& actor);
    // Arrive at: the pace that has an actor get to its path's end at a tick, with its waits and transitions as they
    // are; beyond MIN_PACE..MAX_PACE, the gait that would get it there instead (a faster or slower one), if it has one
    struct Arrival
    {
        float pace = 1.0f;
        std::optional<Types::ActorGait> instead;
    };
    std::optional<Arrival> SolveArrival(const Types::Actor& actor, std::uint32_t tick);
    // has it arrive then, at a pace within MIN_PACE..MAX_PACE; returns whether it gets there then
    bool ArriveAt(Types::Actor& actor, std::uint32_t tick);
    // when an actor passes the points of its path and arrives (see ActorLogic::GetPathTiming), worked out once a change
    ActorLogic::PathTiming GetPathTiming(const Types::Actor& actor);
    // the walk along an actor's path in pieces, one per look (see ActorLogic::GetWalkPieces), worked out once a change
    std::vector<ActorLogic::WalkPiece> GetWalkPieces(const Types::Actor& actor);
    // how far apart a stretch's join's two looks are as its blend starts (see TransitionLogic::GetPoseDistance), each
    // as far into its play as it is then, as the game poses the actor's body in them; none without a join there, or
    // while their poses can't be worked out
    std::optional<float> GetJoinDistance(const Types::Actor& actor, std::size_t stretch);

    // The blend in that joins an animation played on an actor at a tick cleanly to the clip it shows there (see
    // TransitionLogic::IsCleanJoin): the shortest, in steps of 10 ms, over which the bones move at most CLEAN_JOIN in
    // 50 ms from that clip's last pose to the animation's first, from MIN_CLEAN_BLEND to MAX_CLEAN_BLEND; none while
    // their poses aren't known (or with no clip there)
    inline constexpr std::uint32_t MIN_CLEAN_BLEND = 50;
    inline constexpr std::uint32_t MAX_CLEAN_BLEND = 500;
    std::optional<std::uint32_t> GetCleanBlend(const Types::Actor& actor, std::uint32_t tick,
                                               const std::string& animation);
    // Gives a clip just added the blend in that joins it cleanly to the clip before it (GetCleanBlend), as part of the
    // step that added it: on the next RunPendingReplans, or once their poses are worked out (PoseSignatures asks the
    // game for them the first time), for up to 6 s; not once its blend in was changed meanwhile
    void BlendCleanly(std::uint32_t actorId, std::uint32_t clipId);

    // The paths with gaits changed since (their points, gaits, looks, pace, stance, set or clips) get their walks
    // fitted to them and their transitions planned again (see FitOnlyWalk), as part of the change's step: the UI calls
    // this when nothing is dragged or typed. One-offs the user moved or added are anchored where they are. Timing is
    // owned: after a change of look or body the actor keeps its arrival if a pace within
    // MIN_PACE..MAX_PACE can, else it moves; what moved is said once in a note (see GetNotes). What the re-plans put in
    // or moved is kept a moment for the UI to show (see GetFlashes). The blends BlendCleanly waits for are given first.
    void RunPendingReplans();
    struct ReplanFlash
    {
        std::uint32_t actor = 0;
        std::vector<std::uint32_t> clips;  // transitions put in or moved, one-offs moved with their anchors
        std::chrono::steady_clock::time_point at;
    };
    // those of the last second
    std::vector<ReplanFlash> GetFlashes();

    // Never silently (proposal 8): what a re-plan changed that the user didn't ask for
    // (the arrival, a death that hangs on the end, a gait a look took), said once with the other choice
    struct Note
    {
        std::uint32_t actor = 0;
        std::string kind;  // one of a kind at a time: "arrival", "death", "gait"
        std::string text;
        // Keep time: arrive as it did before; Let it move: the pace it had before it kept its arrival
        std::optional<std::uint32_t> keepArrival;
        std::optional<float> letMovePace;
        std::chrono::steady_clock::time_point at;
    };
    // the notes of an actor of the last 12 s (the latest first), a note (the one of its kind before it goes), and
    // taking them away (answered, or the actor changed since)
    std::vector<Note> GetNotes(std::uint32_t actor);
    void AddNote(Note note);
    void DismissNotes(std::uint32_t actor);

    // Looks (ActorLogic::SetLook): the look of a gait for the whole path, or a stretch's
    // own; an animation of another gait's speed changes the gait, which a note says once the change is planned (not
    // for a preview, said false); returns the gait it is a look of
    std::optional<std::pair<Types::ActorGait, bool>> SetLook(Types::Actor& actor, std::optional<std::size_t> stretch,
                                                             const std::string& animation, bool said = true);
    // the animation a clip shows: a walk of a path with gaits, the look of the stretch it sets off on (the walk plays
    // its stretches' looks), else its own
    std::string GetShownAnimation(const Types::Actor& actor, const Types::ActorClip& clip);
    // the stretch of an actor's path it is on at a tick (by its timing: the last it has left by then), if any
    std::optional<std::size_t> GetStretchAt(const Types::Actor& actor, std::uint32_t tick);
    // where a death cuts an actor's walk before it gets to its path's end (the rest of
    // the path isn't reached), as the timeline's tick of the death's start; none when it arrives
    std::optional<std::uint32_t> GetCutAt(const Types::Actor& actor);

    // Try another. Shows an actor as given without it being a change to undo (a
    // look previewed from a list), or back as it was.
    void Preview(const Types::Actor& actor);
    // while a look is previewed, an actor shown going through a span of its timeline again and again (from when it was
    // asked, whatever the playhead does), for the view to draw it at (GetPreviewTick); none to stop
    struct PreviewLoop
    {
        std::uint32_t actor = 0;
        std::uint32_t from = 0, to = 0;
    };
    void SetPreviewLoop(std::optional<PreviewLoop> loop);
    std::optional<std::uint32_t> GetPreviewTick(std::uint32_t actor);
    // the looks of a gait for an actor: those of the catalog that loop going forward (within MAX_WALK_ANGLE) as that
    // gait does and fit its models, the body's own first, then the nearest in speed to it; at most `most`
    std::vector<std::string> GetLookCandidates(const Types::Actor& actor, Types::ActorGait gait, bool crouched,
                                               std::size_t most = 16);
    // the looks tried on an actor's path, the latest first (A/B goes between the first two)
    std::vector<std::string> GetTriedLooks(std::uint32_t actor);
    void AddTriedLook(std::uint32_t actor, const std::string& animation);
    // Deaths that suit the motion first (proposal 11): the deaths of the catalog that fit the actor and its rig family
    // (a person's: not an animal's, a zombie's or a vehicle's), those that start as it moves at the tick first (running
    // deaths while it runs or sprints, walking or running ones while it walks, standing ones while it stands; crouched
    // ones while it crouches), its own first among them, then the others; special weapons', cinematic and level deaths
    // after the plain ones; with what it does then, in words ("running"); at most `most` of each
    struct DeathCandidates
    {
        std::string motion;
        std::vector<std::string> suited;
        std::vector<std::string> others;
    };
    DeathCandidates GetDeathCandidates(const Types::Actor& actor, std::uint32_t tick, std::size_t most = 12);
    // how fast an actor goes at a tick, units a second (over the 100 ms around it)
    float GetSpeedAt(const Types::Actor& actor, std::uint32_t tick);

    // Duplicate beside, Run with. A copy of an actor beside it: its path (or
    // where it stands) that far to its left (right, negative, see ActorLogic::OffsetPath), its keys the same, its clips
    // and times as they are; not added.
    Types::Actor MakeBeside(const Types::Actor& actor, float left);
    // An actor going along another's path that far to its side, from when the other sets off: of the gaits its body
    // standing has, the one whose pace for getting to the end with the other is nearest 100% (within MIN_PACE..
    // MAX_PACE), standing at its start a while if even its slowest pace gets it there sooner (worked out again as its
    // plan changes); an actor of another body may never quite arrive with it
    void RunWith(Types::Actor& actor, const Types::Actor& leader, float left);

    // What actors are added with (see ActorLogic::MakeStart): the starting points' animations of those the game has,
    // the gun to shoot (an assault rifle of the map's), the death to die with (a favourite or recent death the game
    // has that fits, else the first of ActorLogic::DEATH_ANIMATIONS); empty for none. A dog's are its breed's (see
    // ActorLogic::GetDogStartAnimations), its death a favourite or recent dog's death, else its breed's.
    ActorLogic::StartAnimations GetStartAnimations(const ActorLogic::Body& body = {});
    std::string GetStartWeapon();
    // the death of an actor (a person's, without one): as GetStartAnimations has it, of those that fit its models
    std::string GetDeathAnimation(const Types::Actor* actor = nullptr);
    // what a new clip of an actor that has none plays: its body's idle (a person's casual_stand_idle)
    std::string GetIdleAnimation(const Types::Actor& actor);

    // the animations the user chose last for clips, the latest first (kept in config.json)
    std::vector<std::string> GetRecentAnimations();
    void AddRecentAnimation(std::string_view name);

    // Effects in the scene: how one of the game's plays by itself (none for one it hasn't
    // loaded), the effects starred in the browser and those added last, the latest first (kept in config.json)
    std::optional<Types::ActorCatalog::EffectInfo> GetEffectInfo(std::string_view effect);
    bool IsFavoriteEffect(std::string_view name);
    void SetFavoriteEffect(std::string_view name, bool favorite);
    std::vector<std::string> GetRecentEffects();
    void AddRecentEffect(std::string_view name);
    // Props in the scene: those added last, the latest first (kept in config.json),
    // for the view's right-click menu
    std::vector<std::string> GetRecentProps();
    void AddRecentProp(std::string_view name);

    // The content pack the scene needs (empty for none); setting it asks the game to load it
    std::string GetPack();
    void SetPack(const std::string& pack);

    // Presets of actors' looks (character, weapon, props) and clips, kept in config.json by name. Applying a look
    // preset changes the actor's look; a clips preset adds its clips from a tick on.
    enum class PresetKind
    {
        Look,
        Clips,
    };
    std::vector<std::string> GetPresetNames(PresetKind kind);
    // the animations a clips preset adds (of its clips, then of its Face row's)
    std::vector<std::string> GetPresetAnimations(const std::string& name);
    void SavePreset(PresetKind kind, const std::string& name, const Types::Actor& actor);
    bool ApplyPreset(PresetKind kind, const std::string& name, Types::Actor& actor, std::uint32_t tick);
    void DeletePreset(PresetKind kind, const std::string& name);

    // Adds copies of an actor in a formation around it (see ActorLogic::MakeFormation), as one step to undo; returns
    // their ids
    std::vector<std::uint32_t> AddFormation(std::uint32_t id, const ActorLogic::Formation& formation,
                                            std::uint32_t tick);

    // Makes a player of the demo look like an actor (see Components::Loadouts): its body and head, if the map has
    // them for players, and its weapon in place of the player's primary or secondary. Returns why it couldn't, empty
    // when it did.
    std::string CopyLookToPlayer(const Types::Actor& actor, std::int32_t client);
    // the models of actors and players with face paint on, for those of a character zone (see ContentPacks)
    std::vector<std::string> GetFacePaintModels();
    // Gives a loadout a character's models, as the Look page's Character gives an actor; false for one it doesn't know
    bool SetLoadoutCharacter(Types::Loadout& loadout, std::string_view id);

    // the name a model is shown with, e.g. "Rus Prison Guard: Head 1" for c_rus_prison_guard_head1
    std::string DescribeModel(std::string_view model);

    // The characters of the campaign and Zombies that come with the mod (resources/characters.json) and what their
    // models are (see CharacterLogic::GetModelKinds); empty when the file doesn't read
    const CharacterLogic::CampaignCatalog& GetCampaignCharacters();
    const std::unordered_map<std::string, Types::ActorCatalog::ModelKind>& GetCampaignModelKinds();
    bool IsFavoriteCharacter(std::string_view id);
    void SetFavoriteCharacter(std::string_view id, bool favorite);
    // The character an actor is: its own, else the one its models make up (actors saved before they had one); none
    // when the catalog has neither
    const Types::Character* GetCharacter(const Types::Actor& actor, const Types::ActorCatalog& catalog);
    // Makes an actor a character, with a look of it (see CharacterLogic::GetModels). Its models come from the
    // character zones when they are set up (see GetWantedCharacterZones), else from the pack of the character's level,
    // which the scene is given when they aren't loaded.
    void ApplyCharacter(Types::Actor& actor, const Types::Character& character, const CharacterLogic::Look& look,
                        const Types::ActorCatalog& catalog);

    // Character zones: a small zone per character of the campaign and Zombies, which tools/t5-characters/setup.ps1
    // builds from the game's own zones into storage/t5/iris/zones. The game loads those
    // the scene's actors need and unloads those no actor needs when there are too many.
    std::filesystem::path GetCharacterZonesFolder();
    // whether the setup has built them (the folder has character zones), as last looked at (RescanCharacterZones, and
    // as each demo loads)
    bool HasCharacterZones();
    bool IsCharacterZoneAvailable(std::string_view zone);
    // a zone that is built but can't load yet, as Iris Studio's textures file hasn't its images (they'd end the game)
    bool IsCharacterZoneMissingTextures(std::string_view zone);
    // (the harness's) reads that file in place of the mod's next time the zones are looked for, empty for the mod's
    void SetTexturesFile(std::filesystem::path file);
    // (the harness's: a folder of zones the setup hasn't built)
    void SetExtraZonesFolder(std::filesystem::path folder);
    void RescanCharacterZones();
    // as if the setup hadn't built them, for the harness's scenarios of level packs
    void IgnoreCharacterZones(bool ignore);
    // the zones an actor's models need (see CharacterLogic::GetZonesFor), of those built
    std::vector<std::string> GetActorZones(const Types::Actor& actor, const Types::ActorCatalog& catalog);
    // the zones the scene's actors need: the selected actor's first, then the others' in their order
    std::vector<std::string> GetWantedCharacterZones();

    // reads the demo's scene from its file again in the next frame, as when the demo loads (for the harness)
    void ReloadScene();
}  // namespace IWXMVM::Components::Actors
