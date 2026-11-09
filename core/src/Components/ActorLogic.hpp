#pragma once
#include "Types/Actor.hpp"
#include "Types/AnimationCatalog.hpp"
#include "Components/PoseLogic.hpp"

#include <deque>
#include <span>

#include "nlohmann/json.hpp"

namespace IWXMVM::ActorLogic
{
    // What actors do at a tick of the timeline, worked out from the actors alone: no game, UI or global state, so the
    // tests in tests/actors-tests build it on its own. Components::Actors keeps the scene and calls into this.

    // keeps clips by start, keys and events by tick, a Hit where its death starts and the stretches one per point of
    // the path (see FitStretches), and gives the new clips and events ids
    void Normalize(Types::Actor& actor);

    // What an actor shows at a tick, into at most two samples; returns how many. The samples point into the actor's
    // clips. Before its first clip an actor holds that clip's first frame; while a clip blends in, the clip before it
    // goes on under it.
    std::size_t Sample(const Types::Actor& actor, std::uint32_t tick, std::array<Types::ActorAnimationSample, 2>& out);
    std::vector<Types::ActorAnimationSample> Evaluate(const Types::Actor& actor, std::uint32_t tick);

    // How an animation moves the root of what plays it (its delta, see RootMotion below): where the root is, from where
    // it started, at evenly spaced points of a play
    struct RootMotion
    {
        // x forward and y to the left of where the root started facing (units), and how far it has turned to the left
        // since (degrees, not wrapped): the first all 0, the last at the end of a play
        std::vector<glm::vec3> samples;
    };
    // where the root is a point into a play (0 its start, 1 its end), between the samples
    glm::vec3 GetRootMotion(const RootMotion& motion, float time);
    // how it moves from one point of a play to another, as the game's delta between them has it: x forward and y to
    // the left of the way it faced at the first, and how far it turned to the left (degrees)
    glm::vec3 GetRootMotionBetween(const RootMotion& motion, float from, float to);
    // what is guessed of how an animation the game hasn't measured moves the root, from where a play ends (x forward,
    // y left) and how far it turns (degrees left): straight there at a steady speed, turning evenly
    RootMotion GuessRootMotion(glm::vec2 motion, float turn);

    // What is known of an animation that walks an actor along its path
    struct AnimationInfo
    {
        float duration = 0.0f;  // seconds
        // root motion in one play, once the game has measured it: x forward, y to the left
        std::optional<glm::vec2> motionPerPlay;
        std::span<const Types::ActorNotetrack> notetracks;  // sorted by time
        // what it moves and the rig it was made for (see PoseLogic::ClassifyAnimation)
        Types::AnimationKind kind = Types::AnimationKind::Other;
        Types::AnimationFamily family = Types::AnimationFamily::Other;
        // as the catalog of every animation has them, worked out once (see AnimationCatalogLogic): whether it is a
        // death (IsDeathAnimation) and aims or shoots (IsAlertAnimation), and the stance it starts in; without them
        // the rules work them out from the name and notetracks
        std::optional<bool> death;
        std::optional<bool> alert;
        std::optional<Types::ActorStance> stance;
        // how it moves the root through a play, once the game has measured it; none for an animation without a delta
        std::shared_ptr<const RootMotion> rootMotion;
        bool delta = false;  // it moves the root, as the catalog of every animation says (before it is measured)
        // as the catalog has them: the posture and movement it starts and ends in, and how far it turns in a play
        // (degrees to the left: its root's, or for MP's turns, which the game turns in code, its name's)
        std::optional<Types::AnimationState> from;
        std::optional<Types::AnimationState> to;
        std::optional<float> turn;
        bool loop = false;  // made to loop, such as idles and walks
        // how far a play moves the root (x forward, y to the left) as the catalog decoded it, for how fast a look goes
        // before the game has measured it (see GetAnimationSpeed)
        std::optional<glm::vec2> catalogMotion;
        // where its feet come down, as the catalog worked it out from its legs where it has no footstep notetracks
        // (see GetFootfalls)
        std::span<const Types::AnimationFootfall> footfalls;
    };
    using AnimationLookup = std::function<std::optional<AnimationInfo>(const std::string& animation)>;

    // Legs under upper body clips. The game plays the players' upper body animations
    // (pt_) over a full body one for the legs (pb_: the stance's idle, a walk), weighed a hundredth as much: as it
    // blends each bone by the weights of the animations that move it, the legs, which only the full body one moves,
    // are wholly its own, and the upper body is 99% the upper body animation's. An actor's clips of upper body
    // animations get legs the same way: the idle of their stance, as the actor's gun stands in it. Where such a clip
    // blends in over a full body clip, the legs go on with that clip until the blend ends, then blend to the idle as
    // the game blends its legs, and a change of stance blends to the other stance's idle.

    // how much the legs under upper body clips weigh, relative to those clips (the game's)
    inline constexpr float LEGS_WEIGHT = 0.01f;
    // the ticks the legs take to blend to another animation (the game's least, see BG_SetNewAnimation): from one that
    // stands, from one that walks, and to the idle of another stance
    inline constexpr std::uint32_t LEGS_BLEND = 170;
    inline constexpr std::uint32_t LEGS_BLEND_AFTER_MOVING = 250;
    inline constexpr std::uint32_t LEGS_BLEND_STANCE = 400;

    // the stance an upper body animation is made for, from its name: crouching, prone, else standing; GetStance gives
    // the catalog's (the stance it starts in, as the game's tables, notetracks and name say) where it has one
    Types::ActorStance GetNameStance(std::string_view animation);
    Types::ActorStance GetStance(std::string_view animation, const std::optional<AnimationInfo>& info);
    // the stance an anim_pose notetrack (anim_pose = "crouch") has the body in from there; none for other notetracks
    std::optional<Types::ActorStance> GetNotetrackStance(std::string_view notetrack);
    // The idle the legs stand (crouch, lie) in under upper body clips, by stance (not Auto) and gun, as the game's
    // player animations have it; FindStanceIdle gives the first of the gun's and the rifle's the game has, empty for
    // neither
    std::string_view GetStanceIdle(Types::GunClass gun, Types::ActorStance stance);
    std::string FindStanceIdle(Types::GunClass gun, Types::ActorStance stance,
                               const std::function<bool(std::string_view)>& has);

    // Locomotion. With Locomotion Auto, an actor's clips of the players' own animations
    // of standing, walking, running and sprinting (those of any gun, see FindLocomotion) stand for its locomotion: its
    // body plays there what the game has a player play, picked each tick: the animation of its gun for its speed (on a
    // path, the actor's pathSpeed while such a clip walks it; with keys, how fast it goes between them), its stance and
    // the way it goes against the way it faces, played as fast as the speed asks of it. Upper body clips play over it
    // as over the legs above, walking on with a walk they interrupt; other clips (deaths, the campaign's animations)
    // show in its place, as with Locomotion Off. The legs blend to each animation they change to over the game's times.
    // An actor with nowhere to go has no speed to pick a gait by: its walks, runs and sprints play as they are, in
    // place (see HasWayToGo), and only its idles stand for the locomotion's.

    // the players' gaits, slowest first; prone, every gait but standing crawls
    enum class Gait
    {
        Idle,
        Walk,
        Run,
        Sprint,
    };
    // the way an actor goes, against the way it faces
    enum class Heading
    {
        Forward,
        Back,
        Left,
        Right,
    };
    // the heading of an angle (as ClipWalk::angle: 0 forward, 90 left; within 45 degrees of one), and its angle
    Heading GetHeading(float angle);
    float GetHeadingAngle(Heading heading);

    // Dogs (step 7). A dog stands, walks and runs with the dogs' own animations, those
    // of its breed: multiplayer's german shepherd's (its killstreak's, which the campaign's dogs play too) or Zombies'
    // hellhound's. It has one stance and one way to go (forward). With Locomotion Auto its clips of either breed's
    // idle, walk and run stand for its own breed's of its speed, as a person's clips of the players' do for theirs, and
    // they play as fast as the speed asks; it has no legs under upper body clips (those are a person's).
    enum class DogBreed
    {
        Shepherd,   // german_shepherd_*: an idle, a walk and a run
        Hellhound,  // zombie_dog_*: an idle, a walk, a trot (its run) and a run (its sprint)
    };
    // the animations of a breed: its idle and the walk, run and sprint (its run when it has none), and its death
    std::string_view GetDogAnimation(DogBreed breed, Gait gait);
    std::string_view GetDogDeath(DogBreed breed);

    // What an actor stands, walks and runs as (see GetLayers): a person holding a gun of a class (the players'
    // animations have variants for some) with a movement set, a dog of a breed, or a killstreak (whose clips are of its
    // motions, see KillstreakLogic)
    struct Body
    {
        Types::ActorKind kind = Types::ActorKind::Human;
        Types::GunClass gun = Types::GunClass::Rifle;  // a person's
        DogBreed breed = DogBreed::Shepherd;           // a dog's
        // a person's, never Auto (see Components::Actors::GetBody): whose walks, runs, turns, starts and stops
        Types::ActorMovementSet set = Types::ActorMovementSet::Players;
        Types::Killstreak killstreak = Types::Killstreak::RcXd;  // a vehicle's

        Body() = default;
        // a person with a gun of the class
        Body(Types::GunClass gun) : gun(gun)
        {
        }
        static Body Dog(DogBreed breed)
        {
            Body body;
            body.kind = Types::ActorKind::Dog;
            body.breed = breed;
            return body;
        }
        static Body Vehicle(Types::Killstreak killstreak)
        {
            Body body;
            body.kind = Types::ActorKind::Vehicle;
            body.killstreak = killstreak;
            return body;
        }

        bool operator==(const Body&) const = default;
    };

    // below this speed (units a second) the legs stand
    inline constexpr float IDLE_SPEED = 5.0f;
    // how fast the movement animations play, against their own speed, at least and at most (BG_RunLerpFrameRate's)
    inline constexpr float MIN_LOCOMOTION_RATE = 0.1f;
    inline constexpr float MAX_LOCOMOTION_RATE = 2.0f;
    // the ticks the legs take to blend to an animation that moves (the game's least, see LEGS_BLEND)
    inline constexpr std::uint32_t LEGS_BLEND_TO_MOVING = 120;

    // The players' animation of a gait (Idle: the stance's idle, as GetStanceIdle), heading and stance (not Auto) with
    // a gun, as mp/playeranim.script has them; where they have none, the nearest (a crouching sprint runs, a sprint
    // that isn't forward runs, prone every gait crawls); a dog's its breed's of the gait, whatever the stance and
    // heading. FindLocomotionAnimation gives the first of the gun's and the rifle's the game has (a dog's breed's and
    // the shepherd's), empty for neither.
    std::string_view GetLocomotionAnimation(const Body& body, Types::ActorStance stance, Gait gait, Heading heading);
    std::string FindLocomotionAnimation(const Body& body, Types::ActorStance stance, Gait gait, Heading heading,
                                        const std::function<bool(std::string_view)>& has);
    // which of those an animation is (of any gun, any movement set, or any breed), none for another animation
    struct LocomotionAnimation
    {
        Types::GunClass gun = Types::GunClass::Rifle;
        Types::ActorStance stance = Types::ActorStance::Stand;
        Gait gait = Gait::Idle;
        Heading heading = Heading::Forward;
        std::optional<DogBreed> dog;  // a dog's, of that breed
        // the players' (the movement set of every gun's), or the AI's of a set
        Types::ActorMovementSet set = Types::ActorMovementSet::Players;
        bool vehicle = false;  // a killstreak's motion (see KillstreakLogic): its moving ones go, the others stay
    };
    std::optional<LocomotionAnimation> FindLocomotion(std::string_view animation);
    // The same of one of an actor's clips: the AI's stand for its locomotion only with a movement set other than the
    // players' (scenes from before movement sets walk their AI's walks as their own animations), and walks, runs and
    // sprints only while the actor has a way to go (see HasWayToGo): standing still they play as picked, in place
    std::optional<LocomotionAnimation> FindLocomotion(const Types::Actor& actor, std::string_view animation);
    // whether an actor goes anywhere its locomotion could walk it: along a path of two points or more, or between two
    // keys or more (not standing still, attached, or before its path's second point is placed)
    bool HasWayToGo(const Types::Actor& actor);

    // How fast an animation goes, units a second: as far as it moves in a play (as the game measured it) over its
    // duration; before it is measured, about as fast as the rifle's animations of its gait go, or a dog's own (as the
    // catalog of every animation has them), and any other as far as the catalog has a play move it (0 without)
    float GetAnimationSpeed(std::string_view animation, const AnimationLookup& animations);

    // the presets of an actor's speed on the Movement page, units a second: as fast as the rifle's animations go, and
    // the stance and gait of each
    struct SpeedPreset
    {
        std::string_view name;
        float speed = 0.0f;
        Types::ActorStance stance = Types::ActorStance::Stand;
        Gait gait = Gait::Walk;
    };
    inline constexpr std::array<SpeedPreset, 5> SPEED_PRESETS{{
        {"Walk", 94.0f, Types::ActorStance::Stand, Gait::Walk},
        {"Run", 211.0f, Types::ActorStance::Stand, Gait::Run},
        {"Sprint", 281.0f, Types::ActorStance::Stand, Gait::Sprint},
        {"Crouch walk", 83.0f, Types::ActorStance::Crouch, Gait::Walk},
        {"Crouch run", 120.0f, Types::ActorStance::Crouch, Gait::Run},
    }};
    // a body's: a person's of the players' those above, of the AI's as fast as its set's walk, run, sprint and crouched
    // walk go; a dog's as fast as its breed's walk, run (and sprint) go
    std::span<const SpeedPreset> GetSpeedPresets(const Body& body);

    // Gaits. With Locomotion Auto, a person or dog goes along each stretch of its path
    // at a gait (Types::ActorStretch): as fast as the gait's own animation goes for its body and movement set (so its
    // feet don't slide, and a run stays a run when the body changes), times the actor's pace; its locomotion plays that
    // gait there, at the rate the pace asks. It stands at a stretch's first point for the stretch's wait before going
    // on. A path without stretches (from before gaits) walks at pathSpeed, its gait picked by the speed, as before.

    // the pace Arrive at and the Pace slider keep to; paces from before gaits (a speed between two gaits') may be
    // outside it, within the locomotion's rates
    inline constexpr float MIN_PACE = 0.8f;
    inline constexpr float MAX_PACE = 1.2f;
    inline constexpr float MIN_PACE_FROM_BEFORE = MIN_LOCOMOTION_RATE;
    inline constexpr float MAX_PACE_FROM_BEFORE = MAX_LOCOMOTION_RATE;
    Gait ToGait(Types::ActorGait gait);
    // a stance of a stretch: crouched, or standing
    Types::ActorStance GetStretchStance(const Types::ActorStretch& stretch);
    // the gaits a body goes at, as its speed presets have them (a person's standing and crouched ones, a dog's), none
    // for a killstreak; and the animation a gait plays going forward for the body, of those the game has (empty for
    // none)
    std::vector<SpeedPreset> GetGaits(const Body& body);
    std::string GetGaitAnimation(const Body& body, Types::ActorGait gait, bool crouched,
                                 const std::function<bool(std::string_view)>& has);
    // the stretch from a point of an actor's path (one past its last stretch goes as that one); an actor without
    // stretches has none
    const Types::ActorStretch* GetStretch(const Types::Actor& actor, std::size_t point);
    // how fast an actor goes along a stretch: as fast as its gait's animation goes (measured, else guessed, see
    // GetAnimationSpeed), at the actor's pace and the stretch's own
    float GetStretchSpeed(const Types::Actor& actor, const Types::ActorStretch& stretch,
                          const AnimationLookup& animations);
    // Gives an actor that walks its path at pathSpeed stretches that go as fast: the gait its locomotion picks at that
    // speed (standing, or crouched in a crouched stance) for its body, and the pace that keeps the speed; returns
    // whether it did (an actor with Locomotion Auto on a path, of a body with gaits)
    bool MakeStretches(Types::Actor& actor, const Body& body, const AnimationLookup& animations,
                       const std::function<bool(std::string_view)>& has);
    // Sets the gait of the stretches from one point to another (excluded), giving an actor from before gaits its
    // stretches first (see MakeStretches), and resolves the animations of all of them for the body (see
    // GetGaitAnimation); a stretch going at another gait loses its own look (one of the gait it had)
    void SetGait(Types::Actor& actor, std::size_t from, std::size_t to, Types::ActorGait gait, bool crouched,
                 const Body& body, const AnimationLookup& animations, const std::function<bool(std::string_view)>& has);
    // the animations of an actor's stretches for its body as it is now (after its body or set changed): their looks,
    // else the body's of their gaits
    void ResolveStretches(Types::Actor& actor, const Body& body, const std::function<bool(std::string_view)>& has);

    // Looks: the path owns movement. Each stretch plays its gait's look going forward
    // (whatever way the animation itself goes), as fast as the look goes, at the pace: its own look, else the actor's
    // of its gait, else the body's own animation of the gait. Swapping a moving animation sets a look, for the whole
    // path or one stretch, instead of picking a clip that would walk the path at its own speed whatever the stretches
    // say.

    // a stretch's look: its own, else the actor's of its gait; empty for none
    std::string_view GetLook(const Types::Actor& actor, const Types::ActorStretch& stretch);
    // the gait (and crouched) an animation is a look of: its locomotion's, else the body's whose speed is nearest its
    // own; none for one that doesn't go
    std::optional<std::pair<Types::ActorGait, bool>> GetLookGait(const std::string& animation, const Body& body,
                                                                 const AnimationLookup& animations);
    // Sets a look: a stretch's own (that stretch going at its gait), or the actor's for the whole path (its stretches
    // of the gait, and those of the first stretch's gait, going at it, their own looks taken off); returns the gait it
    // is a look of, none for an actor that has no gaits
    std::optional<std::pair<Types::ActorGait, bool>> SetLook(Types::Actor& actor, std::optional<std::size_t> stretch,
                                                             const std::string& animation, const Body& body,
                                                             const AnimationLookup& animations,
                                                             const std::function<bool(std::string_view)>& has);

    // Joins. Where a stretch plays another animation than the stretch before (another
    // look or gait), goes crouched where that didn't (or the other way) or at another pace of its own, the walk changes
    // over the stretch's join: the legs blend from the one look to the other and the speed eases (in and out) from the
    // one stretch's to the other's over the join's blend, centred on the point, ending or starting at it
    // (Types::ActorJoinAt). Where the path's plan puts a turn or a wait's stop and start at the point, the walk after
    // them takes over from those over the join's blend instead (that clip's blend in, which re-plans keep), and from
    // their start on goes as the stretch from the point. A join from before (Types::ActorStretch::joinAsBefore) blends
    // the legs over LEGS_BLEND_TO_MOVING from the point, and the speed changes there.

    // the shortest and longest Auto blend of a join, ticks
    inline constexpr std::uint32_t MIN_JOIN_BLEND = 120;
    inline constexpr std::uint32_t MAX_JOIN_BLEND = 600;
    // whether a stretch has a join with the one before: one the walk goes on from at a point but the first, which
    // differs from it in its animation, gait, stance or pace
    bool HasJoin(const Types::Actor& actor, std::size_t stretch);
    // Where an animation's feet come down in a play (0 to 1), in order: as its footstep notetracks have them, else as
    // the catalog worked it out from its legs (AnimationInfo::footfalls); none for one that doesn't step
    using Footfall = Types::AnimationFootfall;
    std::vector<Footfall> GetFootfalls(const AnimationInfo& info);
    // a join's Auto blend: one step of the slower look as it goes there (its footsteps' spacing at the rate it plays
    // at, else a quarter of a play), within MIN_JOIN_BLEND..MAX_JOIN_BLEND in steps of 10 ms; for a change of stance
    // at least LEGS_BLEND_STANCE
    std::uint32_t GetAutoJoinBlend(const Types::Actor& actor, std::size_t stretch, const AnimationLookup& animations);
    // a join's blend: its own, else Auto; a join from before's LEGS_BLEND_TO_MOVING
    std::uint32_t GetJoinBlend(const Types::Actor& actor, std::size_t stretch, const AnimationLookup& animations);
    // A join from before made one of now (centred on the point, the feet matched) with its blend as it was (0.12 s),
    // for a change to it
    void UpdateJoin(Types::ActorStretch& stretch);

    // When an actor passes the points of its path as its clips walk it (see GetWalk): the tick it reaches each point
    // and leaves it (later by the point's wait), none for a point it doesn't get to; and when it sets off and arrives
    // (its first walking clip's start, the tick it reaches the path's end), for the timeline's path bar
    struct PathTiming
    {
        std::vector<std::optional<std::uint32_t>> reached;
        std::vector<std::optional<std::uint32_t>> left;
        std::optional<std::uint32_t> start;
        std::optional<std::uint32_t> arrival;
        // each point's join (see HasJoin) as the walk passes it: the ticks its blend starts and ends; none for a point
        // without one, one it doesn't get to, or one a turn or wait of the path's plan is at (the walk after those
        // blends in over its own)
        std::vector<std::optional<std::pair<std::uint32_t, std::uint32_t>>> joins;
    };
    PathTiming GetPathTiming(const Types::Actor& actor, const AnimationLookup& animations);
    // Arrive at: the pace that gets an actor with stretches to its path's end at a tick
    // (within the locomotion's rates), its waits as long whatever the pace, the walk fitted at each pace tried by `fit`
    // (with its transitions planned); none when it doesn't get there, or sets off too late to
    std::optional<float> SolvePace(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations,
                                   const std::function<void(Types::Actor&)>& fit);

    // The walk shown as its looks: each clip that walks the path at its stretches'
    // speeds is drawn in pieces, one per run of stretches it goes through with no join between them, named by its look.
    // A piece after a join starts as the join's blend starts (ramping up over it); a clip's first piece starts with the
    // clip, its ramp the clip's blend in. In order along the timeline.
    struct WalkPiece
    {
        std::uint32_t clip = 0;  // the walk clip it is of (its id)
        std::size_t first = 0;   // its stretches, from the first to the last
        std::size_t last = 0;
        std::uint32_t start = 0;  // ticks: the next piece's start, else where its clip stops showing
        std::uint32_t end = 0;
        std::uint32_t blend = 0;  // ticks its ramp goes on from its start
        bool joined = false;      // it starts at its first stretch's join (else as its clip does)
        std::string animation;    // its look
    };
    std::vector<WalkPiece> GetWalkPieces(const Types::Actor& actor, const PathTiming& timing,
                                         const AnimationLookup& animations);

    // Slides an actor's walk along the timeline: everything of the actor from
    // the clip shown as it sets off at a tick (its walk, or the walk's start) on, its clips, Face row's clips, events
    // and keys, moves that many ticks, and the clip before lasts until the new set-off; not to before 0, nor to before
    // the clip before has played MIN_WALK_LENGTH. Returns how far it moved.
    std::int64_t SlideWalk(Types::Actor& actor, std::uint32_t setOff, std::int64_t delta);

    // What the locomotion plays at a speed (units a second), an angle (as ClipWalk::angle) and a stance (Auto stands)
    // with a gun: idle below IDLE_SPEED; then walking; running above halfway between the speeds of the walk and the run
    // of that heading; sprinting above halfway between the run's and the sprint's, standing and forward only; prone,
    // crawling. As fast as the speed asks of its animation, within the rates.
    struct LocomotionChoice
    {
        std::string animation;  // empty when the game has none
        Gait gait = Gait::Idle;
        Heading heading = Heading::Forward;
        float rate = 1.0f;
    };
    LocomotionChoice ChooseLocomotion(const Body& body, Types::ActorStance stance, float speed, float angle,
                                      const AnimationLookup& animations);

    // The animations an actor's legs may play with a gun, which the game's tree of its animations has as entries of
    // their own, so it isn't made again as they change: with Locomotion Off, the stance idles when it has clips of
    // upper body animations; with Auto, those of its locomotion (the idles, and for an actor that moves the walks, runs
    // and sprint) for each stance it may take (its own, else standing and those its clips' names and notetracks say),
    // when it has upper body or locomotion clips. A dog's: with Auto, its breed's idle (and walk, run and sprint).
    std::vector<std::string> GetLegsLibrary(const Types::Actor& actor, const Body& body,
                                            const AnimationLookup& animations);

    // Faces. The game's players have a face of their own that their client scripts
    // (_clientfaceanim_mp.csc) animate with animations of the face alone over the body's: an idle, the alert one, and
    // for a while after the player shoots, is hit or hits with a melee the face of it, and from its death the face of
    // a death, each change over a tenth of a second. With Face Auto an actor's face does the same from its clips and
    // events: the alert idle while a clip aims or shoots (see IsAlertAnimation), else the casual one; the firing face
    // for FIRING_FACE from each shot (of its Fire events, and the fire notetracks its clips shoot), the face of pain
    // once through from each Hit and that of a melee from a clip's melee notetrack, and the face of a death from the
    // start of a clip of a death (see IsDeathAnimation) while that shows, holding its last frame. A death goes over
    // pain and melees, they over firing, and that over the idle. The Face row's clips play over all of it while they
    // show, blending in over their blend in and out over FACE_BLEND, with Face Off too.

    // the game's animations of players' faces
    inline constexpr std::string_view FACE_CASUAL = "pf_casual_idle";
    inline constexpr std::string_view FACE_ALERT = "pf_alert_idle";
    inline constexpr std::string_view FACE_FIRING = "pf_firing";
    inline constexpr std::string_view FACE_MELEE = "pf_melee";
    inline constexpr std::string_view FACE_PAIN = "pf_pain";
    inline constexpr std::string_view FACE_DEATH = "pf_death";
    inline constexpr std::array<std::string_view, 6> FACE_ANIMATIONS{FACE_CASUAL, FACE_ALERT, FACE_FIRING,
                                                                     FACE_MELEE,  FACE_PAIN,  FACE_DEATH};
    // the ticks the face takes to change (the game's: SetAnimKnob's 0.1 s), and fires for after each shot
    inline constexpr std::uint32_t FACE_BLEND = 100;
    inline constexpr std::uint32_t FIRING_FACE = 500;
    // how far back from a tick the shots, hits and melees the face reacts to are looked for: longer than the firing
    // face plays (4.7 s) before it holds its last frame
    inline constexpr std::uint32_t FACE_LOOKBACK = 6000;

    // Whether an animation aims or shoots, which the face is alert for: one of the players' (their faces are always
    // alert in the game), one named so (a part of its name is aim, ads, alert, shoot..., fire..., cqb, combat or
    // exposed) or one that shoots (a fire notetrack)
    bool IsAlertAnimation(std::string_view animation, const std::optional<AnimationInfo>& info);
    // whether a notetrack is a melee's hit
    bool IsMeleeNotetrack(std::string_view name);

    // What an actor's face plays at a tick: a function of the actor and the tick, like its clips' samples
    struct FaceSample
    {
        const Types::ActorClip* clip = nullptr;  // a clip of the Face row (pointing into the actor's faceClips), if so
        std::string animation;
        float time = 0.0f;  // seconds into the animation, before it loops or holds its last frame
        bool loop = true;
        float weight = 0.0f;  // the face's add up to 1, or less while a face clip blends in or out over no face
    };
    std::vector<FaceSample> GetFace(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations);
    // The animations an actor's face may play, which the game's tree of its animations has as entries of their own,
    // so it isn't made again as they change: with Face Auto those of the game's faces it has, then its face clips',
    // each once
    std::vector<std::string> GetFaceLibrary(const Types::Actor& actor, const AnimationLookup& animations);

    // What the legs play under upper body clips (and as the body of locomotion clips): an idle, a movement animation of
    // the locomotion, or the full body clip they go on with
    struct LegsSample
    {
        const Types::ActorClip* clip = nullptr;  // that clip (pointing into the actor's clips), null for the others
        std::string animation;                   // the others'
        float time = 0.0f;                       // seconds into the animation, before it loops
        float weight = 0.0f;                     // of the legs: the legs' samples add up to 1
        float rate = 1.0f;                       // how fast it plays now
    };

    // What an actor shows at a tick, as the game plays it: the clips' samples (see Sample), which of them are of upper
    // body animations and which are locomotion clips, the legs under (or of) those, if any shows, and the face (see
    // GetFace). The legs are a function of the actor and the tick like the clips (a movement animation is as far into
    // it as its rates over time have played it), so scrubbing and captures show the same.
    struct Layers
    {
        std::array<Types::ActorAnimationSample, 2> clips{};
        std::size_t clipCount = 0;
        std::array<bool, 2> upperBody{};
        std::array<bool, 2> locomotion{};  // the legs are its body
        std::vector<LegsSample> legs;      // empty when no such clip shows (or the game has no idle for it)
        std::vector<FaceSample> face;      // empty without a face (Face Off and no face clip showing)
    };
    Layers GetLayers(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations,
                     const Body& body);

    // How much a game's tree of the actor's animations weighs each animation of the layers, as it blends them per bone:
    // the legs under upper body clips a hundredth of those clips (LEGS_WEIGHT), the full body clips shown with them
    // as part of the legs, so the legs blend evenly between those and the idle; locomotion clips are the legs, as much
    // as their own weight. Each animation is there once: a clip the legs go on with weighs its share of both.
    struct TreeWeight
    {
        const Types::ActorClip* clip = nullptr;  // a clip's animation, else the legs' idle
        std::string animation;                   // the idle
        float time = 0.0f;                       // seconds, as the samples have it
        float weight = 0.0f;
    };
    // an animation of the face, by name (a face clip's and the automatic face's of the same animation are one entry, as
    // the heavier has it)
    struct FaceWeight
    {
        std::string animation;
        float time = 0.0f;  // seconds, as the face's samples have it
        bool loop = true;
        float weight = 0.0f;
    };
    // The weights of the animations, and for an MP body (or a spine rig) of those of torso_stabilizer alone that line
    // it up for the clips made for SP bodies and keep it at rest for those that move neither it nor the body (see
    // PoseLogic::SpineKind): those of its legs move it under upper body clips, as on players; legs that don't (made for
    // SP bodies, the AI's sets' idles) have it aimed ahead as the players' legs hold it (PoseLogic::AimSpine), each
    // their own, by their animation. The face's animations move the face alone, which other animations leave alone
    // (but for a few of the players' deaths and flinches, which share it with them), so they are weighed as much as the
    // clips' samples together, each by its share of the face.
    struct TreeWeights
    {
        std::vector<TreeWeight> animations;
        float lined = 0.0f;
        float resting = 0.0f;
        std::vector<std::pair<std::string, float>> aimed;
        std::vector<FaceWeight> faces;
    };
    // what an animation does with an MP body's torso_stabilizer
    using SpineLookup = std::function<PoseLogic::SpineKind(const std::string& animation)>;
    TreeWeights WeighLayers(const Layers& layers, const SpineLookup& spines);

    // Dual wield guns. The left gun hangs from tag_weapon_left, which only the players' dual
    // wield animations (pb_ and pt_ ones named with _dw_) hold in the left hand, all within about 1.5 units of one
    // grip; the others leave it at rest or move it with a gun that isn't there (17 units off the hand in
    // pb_stand_alert, 30 to 45 in deaths). So it is held at the grip, as the left gun of a corpse is, but while dual
    // wield animations weigh all but LEFT_GUN_FREE_SHARE of what the actor shows (their melees swing the gun about).
    inline constexpr glm::vec3 LEFT_GUN_GRIP_POSITION{13.6f, 0.7f, -3.9f};  // from j_wrist_le, its parent
    inline constexpr glm::vec3 LEFT_GUN_GRIP_ANGLES{-2.86f, 2.86f, 175.3f};
    inline constexpr float LEFT_GUN_FREE_SHARE = 0.05f;
    bool IsDualWieldAnimation(std::string_view animation);
    bool GripsLeftGun(const TreeWeights& weights);
    // whether animations of lying down are most of what the actor shows, which the left hand's grip goes by (see
    // GripLogic::PickGrip)
    bool IsProneShown(const TreeWeights& weights);

    // Paths. An actor walks its path through rounded corners: each corner is an arc
    // meeting the stretches on either side of it at a tangent, so the actor turns steadily through it instead of on the
    // spot, passing a few units inside the point. Its radius is as far as the actor walks the path in CORNER_TIME
    // (about a step at a walk, more at a run), at most what keeps it within half of each stretch beside it. A point
    // given twice in a row is a stop (as walking on from where the actor stands puts one, see WalkTo): no arc cuts it
    // short, and the path leaves it curving from the way the actor came onto the way to the next point. The rounded
    // path is the path everywhere: where the actor is along it, how long it is, so the walks fitted to it, and the line
    // the view draws.
    inline constexpr float CORNER_TIME = 0.25f;  // seconds
    inline constexpr float MIN_CORNER_RADIUS = 12.0f;
    inline constexpr float MAX_CORNER_RADIUS = 96.0f;
    // the radius of the corners of a path walked at a speed (units a second)
    float GetCornerRadius(float speed);
    // the radius of the corners of the actor's path at a speed: a killstreak's is as wide as it turns at it (see
    // VehicleLogic::GetTurnRadius), at most half of each stretch as for everyone
    float GetCornerRadius(const Types::Actor& actor, float speed);

    struct RoundedPath
    {
        // a stretch of it: straight, or an arc turning from its direction toward its normal
        struct Piece
        {
            glm::vec3 start{0.0f};
            glm::vec3 direction{1.0f, 0.0f, 0.0f};  // the way it goes from its start (a unit vector)
            glm::vec3 normal{0.0f};                 // an arc's: the way it turns (a unit vector across the direction)
            float radius = 0.0f;                    // an arc's; 0 for a straight piece
            float length = 0.0f;
            float distance = 0.0f;  // along the path to its start
            // the path's point it is of: the first of those it goes to for a straight piece, the last copy of the point
            // whose corner it rounds (or the stop it leaves) for an arc
            std::size_t point = 0;
            bool stop = false;  // an arc leaving a stop
        };
        glm::vec3 origin{0.0f};  // its first point
        std::vector<Piece> pieces;
        float length = 0.0f;
    };
    // (the corners at the points of `sharp` (the last copy of a point given twice) aren't rounded: a turn of the
    // actor's own turns it there, see TransitionLogic::PlanPath; a corner's arc reaches at most half of each stretch,
    // all of one whose other end is a stop or a point the path goes straight on through)
    RoundedPath RoundPath(const std::vector<glm::vec3>& path, float radius, std::span<const std::size_t> sharp = {});
    // an actor's path as it walks it: its corners rounded for the speed it walks it at (its fastest walking clip's,
    // else its locomotion's speed)
    RoundedPath GetRoundedPath(const Types::Actor& actor, const AnimationLookup& animations);
    float GetPathLength(const Types::Actor& actor, const AnimationLookup& animations);
    // points along a rounded path, close enough on its arcs for a line through them to look round
    std::vector<glm::vec3> GetPathLine(const RoundedPath& path);

    // An animation that moves less than this in a play (an idle) doesn't walk
    constexpr float MIN_MOTION_PER_PLAY = 2.0f;

    // How a clip walks an actor along its path, from its animation's motion and the clip's overrides
    struct ClipWalk
    {
        float speed = 0.0f;           // units a second of animation, along the path
        std::optional<float> angle;   // degrees from where the actor faces: 0 forward, 90 left, 180 back; none if it
                                      // doesn't move
    };
    ClipWalk GetClipWalk(const Types::ActorClip& clip, const AnimationLookup& animations);
    // How one of an actor's clips walks it, with its locomotion: Off as above; with Auto, a locomotion clip that walks
    // (one of a walk, run or sprint; or a walk speed given to it) goes at the actor's pathSpeed (or that), the way its
    // animation goes, and an upper body clip walks on as the locomotion clip before its run of upper body clips does
    ClipWalk GetClipWalk(const Types::Actor& actor, const Types::ActorClip& clip, const AnimationLookup& animations);

    // How far the actor's moving clips have walked it along its path by a tick (the motion of the animations they
    // show, weighed as they are shown, while their animations play; a clip holding its last frame doesn't move), and
    // the angle they walk at, which the actor faces away from the path's direction: a strafe left walks along the
    // path facing to its right. The angle is the walking clip's shown, turning from the angle before it over its blend
    // in (the short way round; straight back turns to the left); a clip that doesn't walk keeps the angle it starts
    // at. With Locomotion Auto, its clips walk as GetClipWalk(actor, ...) says, the locomotion ones and those walking
    // on with them for as long as the clips are.
    struct Walk
    {
        float distance = 0.0f;
        float angle = 0.0f;  // degrees, as ClipWalk::angle
    };
    Walk GetWalk(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations);
    float GetWalkedDistance(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations);

    // a point and the direction there on a path at a distance along it (within it)
    Types::ActorPlacement PlaceOnPath(const RoundedPath& path, float distance);
    // how far along a path the point of it nearest a point is, of those between two distances along it (on the ground:
    // heights don't count)
    float ProjectOnPath(const RoundedPath& path, glm::vec2 point, float from, float to);
    // how far along a path a point of it is: where the stretch from it starts (the corner's point, unrounded; where its
    // arc ends, rounded); none for a point it doesn't go on from
    std::optional<float> GetPointDistance(const RoundedPath& path, std::size_t point);
    // How far along its path an actor has got by a tick: as far as its clips walked it, and as far along it as the
    // root motion of its clips took it once they stopped moving it (see RootMotion below); where it is on its path,
    // which walks fitted to the path get to its end by
    float GetPathProgress(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations);

    // Root motion. An animation that moves the root (one with a delta: deaths,
    // stumbles, turns, walks) has the root's travel taken out of its bones' tracks, and the game moves whatever plays
    // it by as much. An actor's clips that don't walk its path (see GetClipWalk; on an actor standing still, which
    // has no path, all of them but the looping walks and runs, which walk in place) move it the same way unless set to
    // stay in place (ActorClip::rootMotion), but those that stand for its locomotion or move the upper body alone: by
    // the root's motion from where the clip starts in
    // its animation, turned the way the actor faced as the clip started, its turn added to the way the actor faces;
    // a looping clip by a play's each play. What follows starts where a clip left the actor: a death lies where it
    // fell, an idle after it stands there, and walking its path goes on from there: as much further along the path as
    // the clip moved it along it, back onto the path from where it moved it off over the first MERGE_DISTANCE units it
    // walks (or more for a longer way back), facing the way it goes; moving between its keys, it goes back onto their
    // way over as much of it.
    // The motion is weighed as the clip is shown while it blends in and while the clip after it blends in over it, as
    // the game sums the motion of a tree's animations by their weights: a clip blending in over a walk doesn't add all
    // of its own to the walk's. Like the clips' samples it is a function of the tick, so scrubbing and captures show
    // the same.
    inline constexpr float MERGE_DISTANCE = 64.0f;
    // whether one of an actor's clips moves it by its animation's root motion: not at all (it walks the path, stands
    // for the locomotion, moves the upper body alone, or its animation doesn't move the root), or as its setting says
    enum class RootMotionUse
    {
        None,
        Moves,
        Stays,
    };
    RootMotionUse GetRootMotionUse(const Types::Actor& actor, const Types::ActorClip& clip,
                                   const AnimationLookup& animations);

    // Where an actor stands at a tick: its position, between its keys, or as far along its path as it has walked;
    // moved from there by its clips' root motion
    Types::ActorPlacement EvaluatePlacement(const Types::Actor& actor, std::uint32_t tick,
                                            const AnimationLookup& animations);

    // Moves an actor to stand there at a tick: its position, its key at that tick (added if need be), or its whole
    // path; where its clips' root motion has moved it from there by then, as EvaluatePlacement has it
    void ApplyPlacement(Types::Actor& actor, std::uint32_t tick, glm::vec3 position, float yaw,
                        const AnimationLookup& animations);

    // How far along a ray (from its origin, along its unit direction) it first touches an actor standing there: an
    // upright capsule about a person's size; none if it misses
    std::optional<float> RayHitsActor(glm::vec3 origin, glm::vec3 direction, glm::vec3 feet);
    // the same of a sphere (a killstreak's body, see KillstreakLogic::GetPickSphere)
    std::optional<float> RayHitsSphere(glm::vec3 origin, glm::vec3 direction, glm::vec3 center, float radius);

    // Whether an actor is drawn at a tick: unless it is hidden in the editor, as its last Show or Hide event before
    // then says, and before the first of those the opposite of what it does
    bool IsVisible(const Types::Actor& actor, std::uint32_t tick);
    // Whether an event's effect stays where it goes off, in the world, rather than going with its bone: when the actor
    // is hidden within EFFECT_STAYS after it, as nothing hidden carries an effect (the blast that ends a killstreak,
    // which the game plays where the vehicle is with playfx before it deletes it)
    inline constexpr std::uint32_t EFFECT_STAYS = 500;
    bool EffectStays(const Types::Actor& actor, const Types::ActorEvent& event);

    // Effects in the scene on their own: an actor of kind Effect (see Types::ActorEffect)
    using EffectInfo = Types::ActorCatalog::EffectInfo;
    // Whether an effect loops as it is made: some of its elements loop for a second or more (an explosion's sparks
    // loop for a few hundred ms: it plays once)
    inline constexpr std::uint32_t MIN_LOOPING_LIFE = 1000;
    bool EffectLoops(const EffectInfo& info);
    // How long a play of it lasts by itself, ms: its looping life, EFFECT_STOPPED_LENGTH for one that loops till it is
    // stopped (the length its bar starts with), EFFECT_ONCE_LENGTH for one that plays once (how long its elements live
    // isn't known)
    inline constexpr std::uint32_t EFFECT_STOPPED_LENGTH = 8000;
    inline constexpr std::uint32_t EFFECT_ONCE_LENGTH = 2000;
    std::uint32_t GetEffectLength(const EffectInfo& info);
    // Whether it plays as a loop: Loop, or Auto for one that loops as made (played again as its looping life ends, till
    // its bar's end); else it plays once
    bool PlaysLooping(const Types::ActorEffect& effect, const EffectInfo& info);
    // Where it plays till: its bar's end while it loops, else where its one play ends (by itself, or cut at the bar's
    // end); the rest of the bar is drawn as unplayed
    std::uint32_t GetEffectPlayedEnd(const Types::ActorEffect& effect, const EffectInfo& info);

    // A model of the map on its own: standing at a point, turned by a yaw, its bar
    // from a tick to an end (at least PROP_LENGTH); as big as it was made, up to these
    inline constexpr std::uint32_t PROP_LENGTH = 8000;
    inline constexpr float MIN_PROP_SCALE = 0.1f, MAX_PROP_SCALE = 10.0f;
    Types::Actor MakeProp(std::string_view model, glm::vec3 point, float yaw, std::uint32_t tick, std::uint32_t end);
    // what a prop is called in the list: its model's name, without its prefixes ("Chair metal")
    std::string GetPropBaseName(std::string_view model);

    // An effect's, a light's or a prop's bar on the timeline: its start and end, none for other actors
    std::optional<std::pair<std::uint32_t, std::uint32_t>> GetBar(const Types::Actor& actor);
    // Puts a bar's start at a tick, as long as it was
    void MoveBar(Types::Actor& actor, std::uint32_t start);

    // The game's pitch and yaw (degrees) that face along a direction
    glm::vec2 GetFacing(glm::vec3 direction);
    // The effect at a point of a surface, facing out of it along its normal (on the ground it fires up, on a wall out
    // of the wall), its bar from a tick as long as it plays by itself; named by the
    // caller
    Types::Actor MakeEffect(std::string_view effect, const EffectInfo& info, glm::vec3 point, glm::vec3 normal,
                            std::uint32_t tick);
    // what a scene effect is called in the list: its effect's name, without its folders and fx_ ("Fire lg")
    std::string GetEffectBaseName(std::string_view effect);
    // The categories the effect browser sorts effects into by their first folder: the
    // map's own (maps/), environment (env/), destructibles, explosions, impacts, vehicles, weapons, bodies (bio/),
    // trails, then the rest as Other
    const std::vector<std::string_view>& GetEffectCategories();
    std::string_view GetEffectCategory(std::string_view effect);
    // how an effect plays by itself, in words: "Loops for 12.0 s, then ends by itself", "Loops till it is stopped",
    // "Plays once"
    std::string DescribeEffect(const EffectInfo& info);

    // A spawn of a scene effect: from where on the timeline, till where (kept and ended there: one that loops; let go
    // to end by itself: one that plays once, whose end is when it should be over), and whether it is cut there at once
    // (its bar's end, Ends Cut) or its particles let finish
    struct EffectPlay
    {
        std::uint32_t begin = 0;
        std::uint32_t end = 0;
        bool kept = false;
        bool cut = false;

        bool operator==(const EffectPlay&) const = default;
    };
    // its spawns over its bar, in order (a one-shot that loops every so often; one that loops, again as its looping
    // life ends; else one)
    std::vector<EffectPlay> GetEffectPlays(const Types::Actor& actor, const EffectInfo& info);
    // those on at a tick: begun by then and not yet ended
    std::vector<EffectPlay> GetEffectPlays(const Types::Actor& actor, const EffectInfo& info, std::uint32_t tick);
    // How far back (ms) a spawn is started when the playhead lands inside it, so it shows as if it had played since its
    // begin, at most this much, as the game works the time it skips out frame by frame
    inline constexpr std::uint32_t MAX_EFFECT_CATCH_UP = 2000;

    // What an actor looks at at a tick: the Look event it turns to (null for none: it looks ahead), the one before it,
    // which it turns from, and how far it has turned, from 0 (still at the one before) to 1, over the event's turn
    struct LookState
    {
        const Types::ActorEvent* from = nullptr;  // point into the actor's events
        const Types::ActorEvent* to = nullptr;
        float turned = 1.0f;
    };
    LookState GetLook(const Types::Actor& actor, std::uint32_t tick);

    // How Look events turn the head (each copy of it, relative to the bone it hangs from), from the rotations that
    // face what the look turns from and what it turns to (none for nothing to look at, which is the clip's own head,
    // as it is animated): the rotation to turn it to, and how much of the clip's own it
    // takes over, from 0 (the clip's own) to 1 (all of it). None while it is the clip's own.
    struct HeadTurn
    {
        glm::quat rotation{1, 0, 0, 0};
        float weight = 1.0f;
    };
    std::optional<HeadTurn> GetHeadTurn(std::optional<glm::quat> from, std::optional<glm::quat> to, float turned);
    // the head as drawn: the rotation the clip animates it to, turned by that much toward the turn's (of the same
    // length, as the game's animations don't keep theirs at 1)
    glm::quat ApplyHeadTurn(const glm::quat& animated, const HeadTurn& turn);

    // The events (and shots of Fire events) that go off as the timeline plays from one tick (included) to another
    // (excluded): an event at the tick the timeline was paused at goes off as it plays on (added at the playhead,
    // it went off only after a rewind to before it)
    struct FiredEvent
    {
        const Types::ActorEvent* event;  // points into the actor's events
        std::uint32_t shot;
    };
    std::vector<FiredEvent> GetFiredEvents(const Types::Actor& actor, std::uint32_t from, std::uint32_t to);

    // The notetracks the actor's clips pass as the timeline plays from one tick (excluded) to another, in order: those
    // of the clip playing at each tick, as far as it has played its animation (a clip holding its last frame passes
    // none; a looping clip passes them again each play)
    struct PassedNotetrack
    {
        const Types::ActorClip* clip;  // points into the actor's clips
        std::string name;
        std::uint32_t shot = 0;  // a shot notetrack's place among those the clip passes from its start, plays included
    };
    std::vector<PassedNotetrack> GetPassedNotetracks(const Types::Actor& actor, std::uint32_t from, std::uint32_t to,
                                                     const AnimationLookup& animations);

    // what a notetrack does
    bool IsShotNotetrack(std::string_view name);

    // A Fire event's shot's place among the shots of the actor's Fire events, in the order of their ticks (an event's
    // own in turn)
    std::uint32_t GetShotPlace(const Types::Actor& actor, const Types::ActorEvent& event, std::uint32_t shot);
    // Which gun of a dual wield gun fires a shot of that place: they take turns, right then left (the way a
    // player fires both), so a shot fires from the same gun however the timeline gets there
    bool IsLeftGunShot(std::uint32_t place);
    // the sound alias a notetrack plays (sndnt#<alias>), empty for none
    std::string_view GetNotetrackSound(std::string_view name);

    // A reload of the actor's gun in a play of a clip's animation, as the animation's reload profile says (see
    // ReloadLogic): the magazine leaves the gun at `out` (into the hand that pulls it, or falling from the gun), the
    // hand has a new one from the swap (and drops the old one it pulled, at a pouch) and that is on the gun at `in`. A
    // reload stops where its clip stops playing it (the next clip starts or the clip ends), with the magazine on the
    // gun again.
    struct Reload
    {
        const Types::ActorClip* clip = nullptr;  // points into the actor's clips
        std::uint32_t out = 0;                   // ticks
        std::uint32_t in = 0;
        std::uint32_t end = 0;              // where the clip stops playing it
        bool dropped = false;               // the old magazine falls from the gun at out, else the hand pulls it
        std::optional<std::uint32_t> swap;  // a new magazine is in the hand from here; none when the hand never shows
                                            // one (until in)
    };
    // What a reload does in a play of its clip's animation, from 0 to 1, as Reload has it in ticks
    struct ReloadMoments
    {
        float out = 0.0f;
        float in = 0.0f;
        std::optional<float> swap;
        bool dropped = false;
    };
    // the reload a clip's animation does, none for none
    using ReloadMomentsLookup = std::function<std::optional<ReloadMoments>(const Types::ActorClip& clip)>;

    // The plays of a clip's animation of a duration (in seconds): 0 is the first, counting those its offset skips; a
    // clip that doesn't loop plays it once (play 0), then holds its last frame. GetReloads maps a reload's moments to
    // ticks with these, and the UI a moment dragged along a clip back.
    // the play a clip is in at a tick while it plays it, else its first
    std::int64_t GetPlayAt(const Types::ActorClip& clip, float duration, std::uint32_t tick);
    // where a play is at a tick, from 0 to 1 within it (less before it, more after it)
    float GetTimeInPlay(const Types::ActorClip& clip, float duration, std::int64_t play, std::uint32_t tick);
    // the first tick at or after a point of a play (from 0 to 1); none before the clip starts, or for a clip that
    // doesn't play (speed 0)
    std::optional<std::uint32_t> GetTickInPlay(const Types::ActorClip& clip, float duration, std::int64_t play,
                                               float time);

    // The reloads of an actor's clips whose magazine leaves the gun at most at a tick, in order, as a lookup says what
    // each clip's animation does (see Components::ReloadProfiles). Of a looping clip only those of its last
    // RECENT_RELOAD_PLAYS plays by then: the ones before are long over, and it can loop for hours.
    inline constexpr std::int64_t RECENT_RELOAD_PLAYS = 8;
    std::vector<Reload> GetReloads(const Types::Actor& actor, std::uint32_t until, const AnimationLookup& animations,
                                   const ReloadMomentsLookup& moments);

    // Where a reload's magazines are at a tick: off the gun from out to in, the old one in the hand until the swap if
    // the hand pulled it out, and the new one in it from the swap on (from in when the reload has no swap)
    enum class HeldMagazine
    {
        None,
        Old,
        New,
    };
    struct ReloadState
    {
        bool offGun = false;
        HeldMagazine held = HeldMagazine::None;
    };
    ReloadState GetReloadState(const Reload& reload, std::uint32_t tick);
    // when the old magazine starts to fall: at out from the gun, at the swap from the hand; none if the reload stops
    // first and the hand puts it back
    std::optional<std::uint32_t> GetFallStart(const Reload& reload);

    // A magazine falling from a gun or a hand, which comes to rest on its flat side: its model is a gun's (x forward, y
    // to the left, z up), so it lies flat with the model's y axis upright. It turns about that axis as it falls, and
    // settles flat over a moment after it lands.
    struct MagazineFall
    {
        glm::mat4 start{1.0f};     // the model's transform as it starts to fall
        glm::vec3 middle{0.0f};    // the magazine's middle, in the model
        glm::vec3 velocity{0.0f};  // of its middle, units a second
        float spin = 0.0f;         // degrees a second
        float ground = 0.0f;       // the height of the ground it lands on
    };
    inline constexpr float MAGAZINE_GRAVITY = 800.0f;        // units a second squared, as the game's g_gravity
    inline constexpr float MAGAZINE_HALF_THICKNESS = 0.75f;  // how high its middle lies above the ground
    inline constexpr float MAGAZINE_SETTLE_TIME = 0.15f;     // seconds from landing to lying flat
    // where its middle is a time after it started falling, as if there were no ground
    glm::vec3 GetFallingMiddle(const MagazineFall& fall, float seconds);
    // seconds from the start until it lands
    float GetLandingTime(const MagazineFall& fall);
    // its model's transform a time after it started falling
    glm::mat4 GetMagazineTransform(const MagazineFall& fall, float seconds);

    // Which of an actor's models a game can draw together, given how many bones each has, in the order they matter
    // (the body first). The game builds an actor's body, head, what it wears and the gun in its hand into one object
    // of at most maxBones bones, counting each model's own bones, even those named like the body's (a full body's face
    // counts again with another head). Each is kept in order unless it would take the total over, so a head or hat too
    // many is left out rather than the actor; none when the first alone is over. With no limit (0) all are kept.
    std::vector<bool> FitBones(std::span<const std::uint32_t> bones, std::uint32_t maxBones);

    // Adds a clip and returns its id; a clip playing where it starts is cut there. AddFaceClip does the same on the
    // Face row.
    std::uint32_t AddClip(Types::Actor& actor, Types::ActorClip clip);
    std::uint32_t AddFaceClip(Types::Actor& actor, Types::ActorClip clip);
    // Adds an event and returns its id
    std::uint32_t AddEvent(Types::Actor& actor, Types::ActorEvent event);
    // A person's new Fire: a short burst at the gun's own speed (fireTime 0 for one the map doesn't know), and one rocket
    // of a launcher
    Types::ActorEvent MakeGunFire(std::uint32_t fireTime, Types::GunClass gun, std::uint32_t tick);

    // Copies a clip to start where it ends, and an event to half a second after its last shot; returns the copy's id,
    // 0 when there is no such clip or event
    std::uint32_t DuplicateClip(Types::Actor& actor, std::uint32_t clipId);
    std::uint32_t DuplicateEvent(Types::Actor& actor, std::uint32_t eventId);

    // A Hit's death: the clip it dies with (ActorEvent::death), which starts with it; null for none
    const Types::ActorClip* FindDeathClip(const Types::Actor& actor, const Types::ActorEvent& hit);
    // Gives the Hits without a death the one they died with before deaths were linked: a clip of the user's that
    // starts with the Hit and doesn't loop (and no other Hit's)
    void LinkDeaths(Types::Actor& actor);
    // Moves an event to a tick, a Hit with its death clip
    void MoveEvent(Types::Actor& actor, std::uint32_t eventId, std::uint32_t tick);

    // A clip with its start moved to a tick and its end where it was, the animation staying where it was on the
    // timeline (its offset follows), or with its end moved to a tick; no shorter than minLength, not before 0
    Types::ActorClip TrimClipStart(const Types::ActorClip& clip, std::int64_t start, std::uint32_t minLength);
    Types::ActorClip TrimClipEnd(const Types::ActorClip& clip, std::int64_t end, std::uint32_t minLength);

    // Adds a point to the path after another: halfway to the next one, or past the last one in the direction of the
    // last stretch, going on as the stretch it is put in (without a wait); with a gait given (as placing points picks
    // it), the stretch to it goes at that gait. Returns its index.
    std::size_t InsertWaypoint(Types::Actor& actor, std::size_t after,
                               std::optional<std::pair<Types::ActorGait, bool>> gait = std::nullopt);
    // Takes a point out of the path, with its stretch; the stretch before it goes on to the next point
    void RemoveWaypoint(Types::Actor& actor, std::size_t index);
    // Adds a point at the end of the path, going on as the last stretch did (a stretch of its own with a gait given)
    void AppendWaypoint(Types::Actor& actor, glm::vec3 point);
    // Moves a point of the path somewhere else, a stop (its point given twice) as one
    void MoveWaypoint(Types::Actor& actor, std::size_t index, glm::vec3 to);
    // keeps an actor's stretches one per point of its path (when it has any): the last point's (which nothing walks)
    // goes as the last stretch walked, and those added go on as it does
    void FitStretches(Types::Actor& actor);

    // the blood of a hit that kills (see AddDeath)
    inline constexpr std::string_view FATAL_BLOOD = "impacts/fx_flesh_hit_body_fatal_exit_mp";
    // the blend a death starts with, until its Auto one is worked out from the poses (Components::Actors::BlendCleanly)
    inline constexpr std::uint32_t DEATH_BLEND = 100;

    // Adds a Hit at a tick and a clip of a death animation (its duration in seconds) that starts with it, as the Hit's
    // "Dies" makes it; a clip shown there is cut (or replaced, starting there), and the clips after it go, so it stays
    // dead. Returns the hit's id.
    std::uint32_t AddDeath(Types::Actor& actor, std::uint32_t tick, const std::string& animation, float duration);
    // One rule for deaths, however they are made (the clip's Animation, a Hit's Dies,
    // Die here, Play, Then... and Add): a clip of a death (its animation's duration in seconds, unknown at 0) plays
    // once, one play long (holding its last frame after), where the actor falls: not along its path, its root motion
    // moving it from the way it faces; the clips after it go and those before it end there (it lies there for good);
    // and a Hit of FATAL_BLOOD at its start dies with it (a Hit dying with it, or one there dying with nothing, stays).
    // Its blend is the Auto one, which the caller asks for once it is in (Components::Actors::BlendCleanly). Returns
    // the Hit's id, 0 for no such clip.
    std::uint32_t MakeDeath(Types::Actor& actor, std::uint32_t clipId, float duration);
    // whether a clip is of a death (it plays once: see IsDeath)
    bool IsDeathClip(const Types::ActorClip& clip, const AnimationLookup& animations);
    // The clip's Animation: a clip's animation swapped for another, as picked, with
    // what suits the new one instead of what suited the old: a death as MakeDeath has it; a one-off once, one play
    // long, not along the path (its root motion moves it); a loop that moves along the path. A death swapped for what
    // isn't one loses the Hits that died with it. Returns the Hit's id for a death, else 0.
    std::uint32_t SwapAnimation(Types::Actor& actor, std::uint32_t clipId, const std::string& animation,
                                const AnimationLookup& animations);
    // Scenes from before deaths had one rule: a death that follows the path (as a swap left one) falls where it is
    // instead; returns whether any did
    bool TakeDeathsOffPath(Types::Actor& actor, const AnimationLookup& animations);

    // Anchors. The one-off clips of an actor walking its path (deaths, reloads,
    // gestures, waves while it stands) hang on a time (a death's Hit goes with it), the path's end or a point of it
    // (ActorClip::anchor), keeping their offset from it. Its walk is fitted and planned with them taken out, then they
    // are put back where their anchors have them: a one-off as PlayAt puts one in (what was shown goes on after it), a
    // death cutting what is shown there (the run goes on until it, or the actor arrives as it starts, or arrives and
    // holds its last frame until it) with the clips after it gone. Nothing is put in
    // that the user didn't add.

    // the tick an anchored clip starts at by the path's timing, none for one on a time (or an anchor not reached)
    std::optional<std::uint32_t> GetAnchoredStart(const Types::ActorClip& clip, const PathTiming& timing);
    // a one-off taken out of an actor's clips: the clip, the id of the clip that went on after it (none for a death),
    // the Hits that die with it (a death's), and whether Auto transitions joined it to the clips around it
    struct OneOff
    {
        Types::ActorClip clip;
        std::optional<std::uint32_t> resumedId;
        std::vector<std::uint32_t> hits;
        bool death = false;
        bool joined = false;
    };
    // Takes out an actor's one-offs from its walk's first clip on: the user's clips (not the walk's own, nor a loop
    // after its last walking clip, which the walk stops into) in what was shown before them, cut there, with it going
    // on after them (as PlayAt leaves it) or what comes next starting as they end, what was shown put back together
    // and their joins (Auto transitions' Join) taken out with them; and the death after its walk (the first of the
    // user's clips after its last walking clip that is one) with the clips after that, nothing put in its place (the
    // walk then gets to the path's end with nothing after it, unless the user put something there). In the order they
    // start; none for an actor not walking a path.
    std::vector<OneOff> TakeOutOneOffs(Types::Actor& actor, const AnimationLookup& animations);
    // Puts one-offs taken out back where their anchors have them now, by the actor's path timing; one-offs after a
    // death put back before them go. Returns the ids of those that were joined, to join again.
    std::vector<std::uint32_t> PutBackOneOffs(Types::Actor& actor, std::vector<OneOff> oneOffs,
                                              const AnimationLookup& animations);
    // how long before the path's end a one-off starting hangs on the end (see Anchor), ticks
    inline constexpr std::uint32_t ANCHOR_REACH = 300;
    // Anchors a one-off where it starts, by the path's timing (its offset as it is: a drag snaps it on the anchor, see
    // the timeline): on the path's end when it starts within reach before the arrival or after it; a one-off that
    // isn't a death starting while the actor walks, on the point it reaches nearest then; else on its time. A death
    // made where the user didn't time it (a clip swapped to one, Then...) takes over from the run as the actor arrives
    // when it starts within reach before then (it dies as it runs).
    void Anchor(Types::ActorClip& clip, const PathTiming& timing, bool death, std::uint32_t reach = ANCHOR_REACH,
                bool takeOver = false);
    // Anchors a death made where the user didn't time it (a clip swapped to one, Then...) by the path's timing as the
    // re-plan will have it (its one-offs out): right after the walk (the clip before it walks the path or is one of
    // its transitions, ending as it starts) it takes over from the run as the actor arrives; else as Anchor's takeOver
    void AnchorMadeDeath(Types::Actor& actor, std::uint32_t clipId, const PathTiming& timing,
                         const AnimationLookup& animations);

    // Duplicate beside: a path moved that far to its left (right, negative),
    // each point off the way through it, at a corner off both ways (mitred: as far from each stretch as from the other)
    std::vector<glm::vec3> OffsetPath(const std::vector<glm::vec3>& path, float left);

    // Scenes from before looks: a moving clip the user picked over a path with
    // stretches (which walked it at its own speed whatever the stretches said) becomes the look of the stretches it
    // covered (the actor's look of a gait when it covered them all), at the gait it is a look of, and the walk the
    // body's own animation of that gait, which stands for its locomotion; returns whether any did
    bool ConvertPickedWalks(Types::Actor& actor, const Body& body, const AnimationLookup& animations,
                            const std::function<bool(std::string_view)>& has);

    // Whether an animation is a death: one that plays once and lets the body go (a start_ragdoll notetrack), or one
    // that plays once named so (a part of its name between underscores and digits is death..., die, dead or dying). A
    // bodyfall notetrack alone isn't one: jumps land with it too.
    bool IsDeathAnimation(std::string_view animation, bool loop, std::span<const Types::ActorNotetrack> notetracks);
    // the same as the catalog of every animation has it (AnimationInfo::death), else as above
    bool IsDeath(std::string_view animation, const std::optional<AnimationInfo>& info);

    // What an actor is doing at a tick, for the deaths that suit it: the clip shown then
    // (the last to start by then that isn't a death: at a Hit, the clip before its death) in the state the catalog says
    // it ends in (a transition's end, a loop's own). While a clip of its path walks it there, before it gets to the
    // end, the stretch it is on (crouched or not) at the speed it goes (units a second); a path's clip before it sets
    // off or after it arrives, the stretch it starts or ends on, still. A clip the catalog doesn't know, or none: its
    // stance at that speed.
    Types::AnimationState GetSuitedState(const Types::Actor& actor, std::uint32_t tick, float speed,
                                         const PathTiming& timing, const AnimationLookup& animations);
    // what that is in words, for a heading ("Suit it crouched"): standing, crouched or prone, walking or running
    std::string DescribeSuitedState(const Types::AnimationState& state);

    // The animation + Clip adds one play of: the selected clip's, else the last's, never a death's (the idle instead,
    // as after one the actor would only die again); the idle for an actor without clips
    std::string GetAddedClipAnimation(const Types::Actor& actor, std::optional<std::uint32_t> selectedClip,
                                      std::string_view idle, const AnimationLookup& animations);

    // Has the actor play an animation (its duration in seconds, its notetracks) from a tick, put in among its clips,
    // as the user picked it (ActorClip::picked): one play of it (up to the next clip), looping if it loops (so a
    // longer clip plays it again), with the clip shown there going on after it; a death as MakeDeath has it, with its
    // Hit, as AddDeath's. A clip shown there is cut (or
    // replaced, starting there). It blends in over the time given (see Components::Actors::GetCleanBlend), else the
    // clip's default (a death's 100 ms). Returns the new clip's id.
    std::uint32_t PlayAt(Types::Actor& actor, std::uint32_t tick, const std::string& animation, float duration,
                         bool loop, std::span<const Types::ActorNotetrack> notetracks = {},
                         std::optional<std::uint32_t> blendIn = std::nullopt);

    // What an actor does once one of its clips ends: holds its last frame (until the next clip starts), goes on with
    // what it played before the clip (the clip after it starts as it ends and plays the animation of the clip before
    // it, as PlayAt puts a one-off in), or plays the next clip, which starts as it ends
    enum class ClipAfter
    {
        Hold,
        Resume,
        Next,
    };
    ClipAfter GetClipAfter(const Types::Actor& actor, std::uint32_t clipId);
    // Has a clip that ends with the actor holding it go on with what the actor played before it: a clip after it of
    // the clip before it, its animation where it would have been, to the next clip (with none, to the end tick, or for
    // 5 s from a clip ending after it); or one that resumes hold its last frame instead, taking that clip out. A clip
    // with no clip before it can't resume, nor one the next clip starts as it ends. Returns whether the clips changed.
    bool SetClipAfter(Types::Actor& actor, std::uint32_t clipId, ClipAfter after, std::uint32_t end);

    // A path cut where it has been walked to, a distance along it as rounded with a radius (see RoundPath): its points
    // before there and one to stop at exactly there, which keeps the way there as it was. In the arc of a corner, the
    // corner's point moves back along the stretch it came on to where the arc's tangent there meets it; a corner
    // before that would round less (the stretch to it now shorter, or the stop taking all of it) gets a point where its
    // arc ends, which it goes straight on through. A stop there (the point again) and more points after it change
    // nothing of the way up to the distance.
    std::vector<glm::vec3> CutPath(const std::vector<glm::vec3>& path, float distance, float radius);

    // Has the actor walk from where it stands at a tick to a point and stand there, with a clip of the walk animation
    // from the tick (see FitWalk) and one of rest after it, as long as the clip shown at the tick would have gone on
    // (that clip again, unless it walks: then the rest animation). Along a path: one from where it stands for an actor
    // standing still (its clips before the tick don't walk), or its path cut where it has walked to by then (see
    // CutPath) with a stop there; an actor with keys gets one where it stands at the tick and one where it gets to.
    // Returns the walk clip's id, 0 for an attached actor, which moves with what it is on.
    std::uint32_t WalkTo(Types::Actor& actor, std::uint32_t tick, glm::vec3 target, const std::string& walk,
                         const std::string& rest, const AnimationLookup& animations, float guessedSpeed);

    // Makes a clip that walks the actor along its path as long as it takes to get to the path's end, at its
    // animation's speed (at guessedSpeed units a second while that isn't measured), and the clip after it start
    // there, keeping its end; with keys, the key after the clip's start moves to when it gets there.
    void FitWalk(Types::Actor& actor, std::uint32_t walkClip, const AnimationLookup& animations, float guessedSpeed);
    // the shortest walk clip FitWalk makes, for a path with nowhere left to go
    inline constexpr std::uint32_t MIN_WALK_LENGTH = 100;
    // Fits the clip that walks an actor along its path to the path as FitWalk does, if that one alone walks it (as a
    // starting point or walking to a point makes it): after the actor's speed or locomotion changed, it still gets to
    // the path's end as the clip ends
    void FitOnlyWalk(Types::Actor& actor, const AnimationLookup& animations);

    // How an actor added from a starting point starts (see MakeStart)
    enum class Start
    {
        Standing,  // stands, idling
        WalkPath,  // walks along the path it is given, then stands
        RunPath,   // runs along it
        Shoot,     // shoots its gun from where it stands
        Die,       // stands, and is hit and dies a moment later
    };
    // the animations a starting point uses, empty for those the game doesn't have; with the players' animations
    // (locomotion), the actor gets Locomotion Auto, and the walk and run are the players' (as their clips stand for
    // it), walking and running the path at the speeds given; with the players' faces, Face Auto
    struct StartAnimations
    {
        std::string idle, walk, run, shoot, death;
        float deathDuration = 1.0f;  // seconds
        bool locomotion = false;
        bool face = false;
        float walkSpeed = SPEED_PRESETS[0].speed;
        float runSpeed = SPEED_PRESETS[1].speed;
    };
    // A dog's of a breed: its breed's idle, walk, run and death (else the shepherd's, which every map has), Locomotion
    // Auto at its breed's speeds, no face and nothing to shoot
    StartAnimations GetDogStartAnimations(DogBreed breed, const AnimationLookup& animations);
    // ticks from the start to the Die starting point's hit, so it is seen standing first
    inline constexpr std::uint32_t DIE_AFTER = 1000;
    // the first walk of the path starting points, until it is fitted to the path given
    inline constexpr std::uint32_t START_WALK_LENGTH = 1000;
    // An actor as a starting point starts it at a tick, to the end of the timeline: its clips, movement and events
    // (the shooting one aims and fires a burst, a Fire event, rather than as its animation's notetracks would), and
    // the weapon to shoot; it stands at the origin (a path of one point there) for the UI to place. With locomotion,
    // the path ones walk and run at the speeds of the Walk and Run presets (the animations').
    Types::Actor MakeStart(Start start, const StartAnimations& animations, std::uint32_t tick, std::uint32_t end,
                           const std::string& weapon);

    // An actor whose body becomes another (a person a dog or back, a dog of another breed: see
    // Components::Actors::ApplyCharacter) does what it did as the new body does it: its clips of the other's idles,
    // walks and runs those of the new one's of the same gait (with locomotion they stand for them anyway), its deaths
    // the new one's death (as long as a play, up to the next clip), and walking a path at a speed preset, the new
    // one's preset of that gait (its one walking clip fitted to the path again). A dog stands as the clips say and has
    // no face of its own; a person gets the face the new actor's defaults give. Clips of anything else stay as they
    // were chosen.
    void ChangeBody(Types::Actor& actor, const Body& from, const Body& to, const StartAnimations& defaults,
                    const AnimationLookup& animations);
    // A person whose body becomes another person's (the Look page's Character, its parts, a look preset) goes on as it
    // did, the user's request of 2026-10-28: its clips, path and transitions stay as they are (no ChangeBody), and with
    // Moves as Auto, which picks by the body (the AI's low ready for a campaign one, the players' for another), it
    // keeps the set Auto stood for before (was), where the new body would pick another (now), so the same walks, runs,
    // starts and stops go on. An actor still as it was added (IsAsAdded) has nothing of its own to keep yet: Auto goes
    // by the new body, so a person added and made a campaign character moves as the AI does.
    void KeepMovementSet(Types::Actor& actor, Types::ActorMovementSet was, Types::ActorMovementSet now);
    // whether an actor does nothing yet but stand as it was added: one clip, no way to walk (no path of two points, no
    // keys), no events
    bool IsAsAdded(const Types::Actor& actor);

    // The animations the starting points look for, the first one a game has first: campaign ones (loaded with a
    // pack) and multiplayer ones (the map's own; multiplayer has no rifle walk but the aiming one)
    inline constexpr std::array<std::string_view, 2> IDLE_ANIMATIONS{"casual_stand_idle", "pb_stand_alert"};
    inline constexpr std::array<std::string_view, 2> WALK_ANIMATIONS{"walk_lowready_f", "pb_stand_shoot_walk_forward"};
    inline constexpr std::array<std::string_view, 2> RUN_ANIMATIONS{"run_lowready_f", "pb_combatrun_forward_loop"};
    inline constexpr std::array<std::string_view, 2> SHOOT_ANIMATIONS{"exposed_aim_5", "pt_stand_shoot_auto"};
    inline constexpr std::array<std::string_view, 2> DEATH_ANIMATIONS{"ai_death_fallforward",
                                                                      "pb_stand_death_headchest_topple"};
    // speeds to walk and run at while a walk's animation isn't measured yet, units a second
    inline constexpr float WALK_SPEED_GUESS = 90.0f;
    inline constexpr float RUN_SPEED_GUESS = 200.0f;
    // the first of the names the game has, empty for none
    std::string FindFirst(std::span<const std::string_view> names, const std::function<bool(std::string_view)>& has);

    // a list of what was used last, the latest first, without repeats and at most so long
    void AddRecent(std::vector<std::string>& recent, std::string_view name, std::size_t most = 8);

    // Undo and redo over copies of a scene's actors. Each change records the actors as they were before it; the
    // changes of one step (a drag, typing a name) undo together, until the step ends.
    class History
    {
       public:
        // before a change; with sameStep, part of the last step even if it ended (the game placing what the UI asked
        // for). Returns whether the change starts a step.
        bool Record(const std::vector<Types::Actor>& before, bool sameStep = false);
        // the next change starts a new step
        void EndStep();

        // the actors to go back (or forward) to from the current ones, if there is a step to undo (redo)
        std::optional<std::vector<Types::Actor>> Undo(std::vector<Types::Actor> current);
        std::optional<std::vector<Types::Actor>> Redo(std::vector<Types::Actor> current);
        bool CanUndo() const;
        bool CanRedo() const;
        void Clear();

       private:
        static constexpr std::size_t MAX_STEPS = 200;
        std::deque<std::vector<Types::Actor>> undoSteps;
        std::vector<std::vector<Types::Actor>> redoSteps;
        bool stepOpen = false;
    };

    // An actor's look (its character, weapon and props) and its clips and Face row's clips (from the first one's start),
    // as presets keep them. Applying clips adds them from a tick on; a clip playing there is cut.
    nlohmann::json LookToJson(const Types::Actor& actor);
    void ApplyLook(Types::Actor& actor, const nlohmann::json& look);
    nlohmann::json ClipsToJson(const Types::Actor& actor);
    void ApplyClips(Types::Actor& actor, const nlohmann::json& clips, std::uint32_t tick);

    // Copies of an actor in a formation of count actors, the actor itself first: rows of columns to its right, each
    // row behind the one before, facing as it does (it faces yaw now); each copy's clips, events and keys are the
    // stagger later than the one before's. The copies have no ids.
    struct Formation
    {
        std::uint32_t count = 4;
        std::uint32_t columns = 2;
        glm::vec2 spacing{48.0f, 48.0f};  // between columns, between rows
        std::uint32_t stagger = 0;        // ticks
    };
    std::vector<Types::Actor> MakeFormation(const Types::Actor& actor, const Formation& formation, float yaw);

    // A demo's scene as saved: its actors and the content pack they need. Read back, the actors have the ids 1, 2, ...
    // in order, which the actors that Look events look at are referred to by.
    struct Scene
    {
        std::vector<Types::Actor> actors;
        std::string pack;
        // what is chosen for the demo's players (PlayerLookLogic::ChoicesToJson), which this logic only keeps
        nlohmann::json players;
    };
    nlohmann::json SceneToJson(const Scene& scene, const std::string& demoName);
    // Throws on a file that isn't a scene. Scenes from before clips had one animation that looped from a tick on, to
    // the demo's end.
    Scene SceneFromJson(const nlohmann::json& root, std::uint32_t demoEndTick);
}  // namespace IWXMVM::ActorLogic
