#pragma once
#include "Components/ActorLogic.hpp"
#include "Types/AnimationPose.hpp"

namespace IWXMVM::TransitionLogic
{
    // Chaining the short turns and transitions: the states clips start and end in, the
    // game's transitions between them (its AI's tables, animscripts/anims_table.gsc; its players' turns and changes of
    // stance, mp/playeranim.script; its dogs'), the chain that joins two clips, what can follow a clip (Then...), and
    // how a transition turns the actor as the game turns its AI. No game state; tested in tests/actors-tests.

    using State = Types::AnimationState;

    // The state one of an actor's clips is in as it starts or ends (posture and movement, Unknown where nothing says):
    // as the catalog has its animation (a looping one ends as it starts); a locomotion clip's (Locomotion Auto) as the
    // locomotion plays it, standing at its idle or walking, running or sprinting at the speed it walks; a dog's
    // stands
    State GetClipState(const Types::Actor& actor, std::size_t index, bool end, const ActorLogic::Body& body,
                       const ActorLogic::AnimationLookup& animations);
    // whether a state says both what the posture and the movement are
    bool IsKnown(const State& state);

    // A transition of the game: an animation that goes from one state to another, turning so far as it goes, as the
    // game's tables have it for a movement set (a person's) or breed (a dog's)
    struct Transition
    {
        std::string_view animation;
        State from;
        State to;
        float turn = 0.0f;  // degrees to the left
    };
    // the transitions of a body's set (or breed), the first of each kind the one to use: for a person with a gun of a
    // class, the players' of the gun's class first, then the rifle's
    std::vector<Transition> GetTransitions(const ActorLogic::Body& body);

    // the game's blend times of a transition: how long it takes over from the clip before it, and how long before its
    // end the clip after it starts to take over from it (blending in as long)
    struct Blends
    {
        std::uint32_t in = 0;
        std::uint32_t handOver = 0;
    };
    Blends GetBlends(const ActorLogic::Body& body, const State& from, const State& to);

    // A transition put in between two clips: the animation, how far it turns the actor by its end (see
    // ActorClip::turn; none for as far as its animation turns), the game's blend times and what it does in words
    struct Link
    {
        std::string animation;
        std::optional<float> turn;
        Blends blends;
        std::string label;
    };
    // turns of at most this much (degrees) need no turn of their own: the actor steers through them
    inline constexpr float TURN_THRESHOLD = 45.0f;
    // The chain that joins the end of a clip to the start of another (none, one or two transitions), as the game
    // chains its AI's, players' and dogs': none when the states are alike (and the turn wanted is within
    // TURN_THRESHOLD), one isn't known, or the clip after is a death; else the transition of the body's set from the
    // state one ends in to the state the other starts in whose turn is nearest the turn wanted (degrees to the left),
    // or two through a state between them (prone to running: getting up to a crouch, then setting off from it). The
    // transition turns the actor as far as wanted when it is asked to turn, else as its animation does.
    std::vector<Link> JoinClips(const Types::Actor& actor, std::size_t from, std::size_t to,
                                const ActorLogic::Body& body, const ActorLogic::AnimationLookup& animations,
                                float turn = 0.0f);
    // the same between states
    std::vector<Link> JoinStates(const State& from, const State& to, const ActorLogic::Body& body,
                                 const ActorLogic::AnimationLookup& animations, float turn = 0.0f);
    // What a transition does in words, for the menus: "Stop, turning left 90°", "Set off running", "Crouch"
    std::string Describe(const State& from, const State& to, float turn);

    // Poses (Q18): where some bones of the actor's body are as an animation starts and as it
    // ends, which say how cleanly one animation goes on from another where no state does
    inline constexpr std::array<std::string_view, 12> SIGNATURE_BONES{
        "j_mainroot", "j_spinelower", "j_spine4", "j_head",   "j_knee_le", "j_knee_ri",
        "j_ankle_le", "j_ankle_ri",   "j_wrist_le", "j_wrist_ri", "j_pelvis", "j_tail1"};
    // where the bones are (by SIGNATURE_BONES' order; none for those the body hasn't), in the animation's space: x
    // forward, y to the left, z up from where it stands
    using Pose = std::array<std::optional<glm::vec3>, SIGNATURE_BONES.size()>;
    struct PoseSignature
    {
        Pose first;
        Pose last;
    };
    Pose MakePose(const Types::AnimationPose& pose);
    // how far apart two poses are: the root of the mean of the squared distances of the bones both have; none without
    // any
    std::optional<float> GetPoseDistance(const Pose& a, const Pose& b);
    // a join of poses this close (units) doesn't show
    inline constexpr float CLEAN_JOIN = 6.0f;
    // whether a clip goes on cleanly from the one before, its poses that far apart: its blend in moves the bones at most
    // CLEAN_JOIN in 50 ms (a blend under 50 ms as one of 50)
    bool IsCleanJoin(float distance, std::uint32_t blendIn);
    // an animation's signature on the actor's body, none until it is known
    using PoseLookup = std::function<std::shared_ptr<const PoseSignature>(const std::string& animation)>;

    // What can follow a clip (Then...), each with what it does: the transitions of the actor's set from the state the
    // clip ends in, then the catalog's other animations that start in that state and change it (the other sets', the
    // cover arrivals and exits, which are never put in on their own), each once; with a fit test, only those that fit
    // the actor. For a clip whose state isn't known, the catalog's animations that change a state, the nearest its
    // last pose first as far as their poses are known (see PoseLookup). At most `most`.
    struct Candidate
    {
        std::string animation;
        std::string label;
        bool table = false;             // the body's own set's
        std::optional<float> distance;  // from the clip's last pose to its first, where both are known
    };
    std::vector<Candidate> ChainCandidates(const Types::Actor& actor, std::size_t index, const ActorLogic::Body& body,
                                           const Types::AnimationCatalog& catalog,
                                           const ActorLogic::AnimationLookup& animations,
                                           const std::function<bool(const Types::AnimationEntry&)>& fits,
                                           const PoseLookup& poses, std::size_t most = 40);

    // Auto transitions: the chain JoinClips gives between a clip and the clips before
    // and after it, put in as clips of its own (marked Join): the clip before ends where the chain starts, each
    // transition plays once from where the one before hands over to it, as fast as the game blends them, and the clip
    // after starts where the last hands over to it; a looping one gives up as much of its start, else it and the clips
    // after it move on as much. The transitions put in before are taken out first (the clip moved back to where they
    // started), so joining again replaces them. Returns whether the clips changed. A path's own chain (see PlanPath)
    // isn't joined to.
    bool JoinAround(Types::Actor& actor, std::uint32_t clipId, const ActorLogic::Body& body,
                    const ActorLogic::AnimationLookup& animations);
    // Takes out the Join transitions next to a clip (as JoinAround does first); returns whether there were any
    bool Unjoin(Types::Actor& actor, std::uint32_t clipId);

    // Has a clip of a transition that turns (as the catalog has it) turn the actor as the game turns its own (see
    // GetTurnMotion), a user's clip of another animation not; returns whether it changed. Clips of transitions are
    // marked so with Auto transitions (their own turns are code's in the players')
    bool MarkTurn(Types::ActorClip& clip, const ActorLogic::AnimationLookup& animations);

    // Then...: plays an animation after a clip (one play, as picked: looping if it loops), the clips after
    // it moved on to make room as JoinAround moves them; with Auto transitions joined to its neighbours. A death is
    // made as ActorLogic::MakeDeath makes every death, the clips after it gone. Returns the new clip's id, 0 for none.
    std::uint32_t PlayAfter(Types::Actor& actor, std::uint32_t clipId, const std::string& animation,
                            const ActorLogic::Body& body, const ActorLogic::AnimationLookup& animations);

    // Paths: with Locomotion Auto and Auto transitions, an actor walking its path forward
    // sets off with its movement set's start for the turn from the way it faced to the first stretch (where the set has
    // none, the players', turning on the spot first), running turns at each corner of more than TURN_THRESHOLD with its
    // running turn for the corner's angle and side (the players', which have none, stop at it, turn on the spot and run
    // on; walking, it walks round the corner, as the game's walks turn as they go), and
    // stops with its stop (turning as its stop is to turn, ActorClip::turn): clips of the path's own (Start, Turn,
    // Stop), the walk cut around the turns (Walk), timed so each turn lands the actor on the stretch after its corner
    // and the stop at the walk's end. The corners of its turns aren't rounded (see ActorLogic::RoundPath).
    // Takes out a path's transitions, putting its walks back together; returns whether there were any
    bool UnplanPath(Types::Actor& actor);
    // Takes them out and puts them in again for each walk of the path (the turn to stop with given for a walk clip, by
    // its id); returns whether the clips changed. Without Auto transitions or Locomotion Auto, it only takes them out.
    bool PlanPath(Types::Actor& actor, const ActorLogic::Body& body, const ActorLogic::AnimationLookup& animations,
                  std::optional<std::pair<std::uint32_t, float>> stopTurn = std::nullopt);
    // The same for one walk of the path (its clip's id): the transitions of its own (those next to it: its start, its
    // turns and the stretches after them, its stop) taken out, the walk put back together; and put in again (the turn
    // to stop with as given, else as its stop turned)
    bool UnplanWalk(Types::Actor& actor, std::uint32_t walkId);
    bool PlanWalk(Types::Actor& actor, std::uint32_t walkId, const ActorLogic::Body& body,
                  const ActorLogic::AnimationLookup& animations, std::optional<float> stopTurn = std::nullopt);
    // how far the stop a walk was planned with turns it (as PlanWalk keeps it), none for a stop that doesn't turn
    std::optional<float> GetStopTurn(const Types::Actor& actor, std::uint32_t walkId);
    // the walks of a path that its transitions are planned for: the clips of its locomotion walking it forward, within
    // MAX_WALK_ANGLE of straight ahead (not those the transitions cut from them)
    inline constexpr float MAX_WALK_ANGLE = 15.0f;
    std::vector<std::uint32_t> GetPathWalks(const Types::Actor& actor, const ActorLogic::AnimationLookup& animations);
    // the ticks a walk takes as planned, from its start's start to its stop's end (its own clip's without them), and
    // whether it is planned (it has a start, turns or a stop of its own)
    std::pair<std::uint32_t, std::uint32_t> GetWalkSpan(const Types::Actor& actor, std::uint32_t walkId);
    bool IsPlanned(const Types::Actor& actor, std::uint32_t walkId);

    // How a transition that turns the actor (ActorClip::turn) moves it through a play (see ActorLogic::RootMotion), as
    // the game moves its AI through a turn: a running turn with an exit_align notetrack holds the way it faces until
    // then and turns at a steady rate to the turn by its code_move notetrack (or its end), the root's travel going the
    // way the actor faces (turn.gsc moves its AI by the animation's position deltas in its own facing); one without,
    // and a start (setting off from a stop: exit_node.gsc moves the AI by the whole of the animation's deltas, its
    // rotation too, from its first frame), turns as its animation does, and the rest of the turn at a steady rate over
    // the part of it that turns; one whose animation doesn't turn (MP's turns, which the game turns in code) at a
    // steady rate up to its loop_begin notetrack (or its end). Without a root motion the animation stays where it is.
    ActorLogic::RootMotion GetTurnMotion(const ActorLogic::RootMotion* motion,
                                         std::span<const Types::ActorNotetrack> notetracks, float turn,
                                         bool start = false);
}  // namespace IWXMVM::TransitionLogic
