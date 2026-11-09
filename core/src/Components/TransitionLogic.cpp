#include "StdInclude.hpp"
#include "TransitionLogic.hpp"

namespace IWXMVM::TransitionLogic
{
    using Posture = Types::AnimationPosture;
    using Movement = Types::AnimationMovement;
    using Set = Types::ActorMovementSet;

    namespace
    {
        bool IsMoving(Movement movement)
        {
            return movement == Movement::Walk || movement == Movement::Run || movement == Movement::Sprint;
        }

        Posture ToPosture(Types::ActorStance stance)
        {
            switch (stance)
            {
                case Types::ActorStance::Crouch:
                    return Posture::Crouch;
                case Types::ActorStance::Prone:
                    return Posture::Prone;
                default:
                    return Posture::Stand;
            }
        }

        Movement ToMovement(ActorLogic::Gait gait)
        {
            switch (gait)
            {
                case ActorLogic::Gait::Walk:
                    return Movement::Walk;
                case ActorLogic::Gait::Run:
                    return Movement::Run;
                case ActorLogic::Gait::Sprint:
                    return Movement::Sprint;
                default:
                    return Movement::Stop;
            }
        }

        std::uint32_t ToTicks(float seconds)
        {
            return std::max(1u, static_cast<std::uint32_t>(std::lround(seconds * 1000.0f)));
        }

        bool IsDog(const ActorLogic::Body& body)
        {
            return body.kind == Types::ActorKind::Dog;
        }

        bool IsVehicle(const ActorLogic::Body& body)
        {
            return body.kind == Types::ActorKind::Vehicle;
        }
    }  // namespace

    bool IsKnown(const State& state)
    {
        return state.posture != Posture::Unknown && state.movement != Movement::Unknown;
    }

    namespace
    {
        // the stretch of an actor's path it is on at a tick (see ActorLogic::GetStretch), as far along it as it has
        // walked by then; none without stretches
        const Types::ActorStretch* GetStretchAt(const Types::Actor& actor, std::uint32_t tick,
                                                const ActorLogic::AnimationLookup& animations)
        {
            if (actor.stretches.empty() || actor.path.size() < 2)
                return nullptr;
            const auto path = ActorLogic::GetRoundedPath(actor, animations);
            const auto progress = ActorLogic::GetPathProgress(actor, tick, animations);
            std::size_t found = 0;
            for (std::size_t i = 0; i + 1 < actor.path.size(); ++i)
                if (const auto distance = ActorLogic::GetPointDistance(path, i);
                    distance && *distance <= progress + 1e-3f)
                    found = i;
            return ActorLogic::GetStretch(actor, found);
        }
    }  // namespace

    State GetClipState(const Types::Actor& actor, std::size_t index, bool end, const ActorLogic::Body& body,
                       const ActorLogic::AnimationLookup& animations)
    {
        State state;
        if (index >= actor.clips.size())
            return state;
        const auto& clip = actor.clips[index];
        const auto info = animations(clip.animation);
        // (a killstreak, like a dog, stands)
        const bool dog = IsDog(body) || IsVehicle(body);
        const bool torso = info && info->kind == Types::AnimationKind::Torso;
        // (a clip the user picked plays as picked)
        const auto locomotion = actor.locomotion == Types::ActorLocomotion::Auto && !torso && !clip.picked
                                    ? ActorLogic::FindLocomotion(actor, clip.animation)
                                    : std::nullopt;
        // a clip of the locomotion, or of the upper body over the legs: as the legs stand or go, at the speed it walks
        if (locomotion || torso)
        {
            auto stance = actor.stance;
            if (dog)
                stance = Types::ActorStance::Stand;
            else if (stance == Types::ActorStance::Auto)
                stance = locomotion ? locomotion->stance : ActorLogic::GetStance(clip.animation, info);
            state.posture = ToPosture(stance);
            state.movement = Movement::Stop;
            const auto walk = ActorLogic::GetClipWalk(actor, clip, animations);
            if (walk.angle && walk.speed * std::max(clip.speed, 0.0f) >= ActorLogic::IDLE_SPEED)
            {
                // on a path with gaits: the gait (and stance) of the stretch it starts (ends) on
                const auto* stretch =
                    locomotion && clip.walkSpeed <= 0.0f
                        ? GetStretchAt(actor, end ? std::max(clip.End(), 1u) - 1 : clip.start, animations)
                        : nullptr;
                if (stretch)
                {
                    if (!dog && actor.stance == Types::ActorStance::Auto)
                        stance = ActorLogic::GetStretchStance(*stretch);
                    state.posture = ToPosture(stance);
                    auto gait = ActorLogic::ToGait(stretch->gait);
                    if (gait == ActorLogic::Gait::Sprint && !dog && stance != Types::ActorStance::Stand)
                        gait = ActorLogic::Gait::Run;
                    state.movement = ToMovement(gait);
                }
                else
                    state.movement = ToMovement(
                        ActorLogic::ChooseLocomotion(body, stance, walk.speed * clip.speed, *walk.angle, animations)
                            .gait);
            }
            return state;
        }
        if (info)
        {
            // (a looping one ends as it starts)
            const auto& known = end && !clip.loop && info->to ? info->to : info->from;
            if (known)
                state = *known;
        }
        // (a dog stands, as far as the catalog's dog animations say anything)
        if (dog && state.movement != Movement::Unknown)
            state.posture = Posture::Stand;
        return state;
    }

    namespace
    {
        constexpr State STAND_STOP{Posture::Stand, Movement::Stop};
        constexpr State STAND_WALK{Posture::Stand, Movement::Walk};
        constexpr State STAND_RUN{Posture::Stand, Movement::Run};
        constexpr State STAND_SPRINT{Posture::Stand, Movement::Sprint};
        constexpr State CROUCH_STOP{Posture::Crouch, Movement::Stop};
        constexpr State CROUCH_WALK{Posture::Crouch, Movement::Walk};
        constexpr State CROUCH_RUN{Posture::Crouch, Movement::Run};
        constexpr State PRONE_STOP{Posture::Prone, Movement::Stop};

        // The game's transitions, as its tables have them; the first of a kind in a list is the one to use
        // The campaign AI's, rifle (animscripts/anims_table.gsc): exits ("exit_exposed": setting off from standing, by
        // the way to go), arrivals ("arrive_exposed": stopping, by the way to face), running turns (turn.gsc's
        // turn_f_*), standing turns (combat turn_left/right_*), changes of stance
        constexpr std::array<Transition, 48> LOW_READY{{
            {"stand_2_run_f_2", STAND_STOP, STAND_RUN, 0.0f},
            {"stand_2_run_l", STAND_STOP, STAND_RUN, 90.0f},
            {"stand_2_run_r", STAND_STOP, STAND_RUN, -90.0f},
            {"stand_2_run_180_med", STAND_STOP, STAND_RUN, 180.0f},
            {"crouch_2run_f", CROUCH_STOP, STAND_RUN, 0.0f},
            {"crouch_2run_l", CROUCH_STOP, STAND_RUN, 90.0f},
            {"crouch_2run_r", CROUCH_STOP, STAND_RUN, -90.0f},
            {"crouch_2run_180", CROUCH_STOP, STAND_RUN, 180.0f},
            {"run_2_stand_f_6", STAND_RUN, STAND_STOP, 0.0f},
            {"run_2_stand_90l", STAND_RUN, STAND_STOP, 90.0f},
            {"run_2_stand_90r", STAND_RUN, STAND_STOP, -90.0f},
            {"run_2_stand_180l", STAND_RUN, STAND_STOP, 180.0f},
            {"run_2_crouch_f", STAND_RUN, CROUCH_STOP, 0.0f},
            {"run_2_crouch_90l", STAND_RUN, CROUCH_STOP, 90.0f},
            {"run_2_crouch_90r", STAND_RUN, CROUCH_STOP, -90.0f},
            {"run_2_crouch_180l", STAND_RUN, CROUCH_STOP, 180.0f},
            {"ai_run_lowready_f_turn_45_l", STAND_RUN, STAND_RUN, 45.0f},
            {"ai_run_lowready_f_turn_90_l", STAND_RUN, STAND_RUN, 90.0f},
            {"ai_run_lowready_f_turn_180_l", STAND_RUN, STAND_RUN, 180.0f},
            {"ai_run_lowready_f_turn_45_r", STAND_RUN, STAND_RUN, -45.0f},
            {"ai_run_lowready_f_turn_90_r", STAND_RUN, STAND_RUN, -90.0f},
            {"ai_run_lowready_f_turn_180_r", STAND_RUN, STAND_RUN, -180.0f},
            {"ai_sprint_turn_45_l", STAND_SPRINT, STAND_SPRINT, 45.0f},
            {"ai_sprint_turn_90_l", STAND_SPRINT, STAND_SPRINT, 90.0f},
            {"ai_sprint_turn_180_l", STAND_SPRINT, STAND_SPRINT, 180.0f},
            {"ai_sprint_turn_45_r", STAND_SPRINT, STAND_SPRINT, -45.0f},
            {"ai_sprint_turn_90_r", STAND_SPRINT, STAND_SPRINT, -90.0f},
            {"ai_sprint_turn_180_r", STAND_SPRINT, STAND_SPRINT, -180.0f},
            {"exposed_tracking_turn45l", STAND_STOP, STAND_STOP, 45.0f},
            {"exposed_tracking_turn90l", STAND_STOP, STAND_STOP, 90.0f},
            {"exposed_tracking_turn135l", STAND_STOP, STAND_STOP, 135.0f},
            {"exposed_tracking_turn180l", STAND_STOP, STAND_STOP, 180.0f},
            {"exposed_tracking_turn45r", STAND_STOP, STAND_STOP, -45.0f},
            {"exposed_tracking_turn90r", STAND_STOP, STAND_STOP, -90.0f},
            {"exposed_tracking_turn135r", STAND_STOP, STAND_STOP, -135.0f},
            {"exposed_tracking_turn180l", STAND_STOP, STAND_STOP, -180.0f},
            {"exposed_crouch_turn_left", CROUCH_STOP, CROUCH_STOP, 90.0f},
            {"exposed_crouch_turn_right", CROUCH_STOP, CROUCH_STOP, -90.0f},
            {"exposed_stand_2_crouch", STAND_STOP, CROUCH_STOP, 0.0f},
            {"exposed_crouch_2_stand", CROUCH_STOP, STAND_STOP, 0.0f},
            {"crouch_2_prone", CROUCH_STOP, PRONE_STOP, 0.0f},
            {"stand_2_prone", STAND_STOP, PRONE_STOP, 0.0f},
            {"prone2crouch_straight", PRONE_STOP, CROUCH_STOP, 0.0f},
            {"prone_2_crouch", PRONE_STOP, CROUCH_STOP, 0.0f},
            {"prone_2_stand", PRONE_STOP, STAND_STOP, 0.0f},
            {"prone2crouchrun_straight", PRONE_STOP, CROUCH_RUN, 0.0f},
            {"crouch2stand", CROUCH_STOP, STAND_STOP, 0.0f},
            {"stand_2_prone_nodelta", STAND_STOP, PRONE_STOP, 0.0f},
        }};

        // The AI's close quarters set: starts (cqb_start_<numpad>, by the way to go: the direction of the numpad's keys
        // from 8 ahead) and stops (cqb_stop_<numpad>, by the way to face), from and to crouching too (its crouched stops
        // from a crouched walk), and the walk's turns (turn_f_*_cqb); its runs are fast walks, so the same go from and
        // to them
        constexpr std::array<Transition, 62> CQB{{
            {"cqb_start_8", STAND_STOP, STAND_WALK, 0.0f},
            {"cqb_start_7", STAND_STOP, STAND_WALK, 45.0f},
            {"cqb_start_4", STAND_STOP, STAND_WALK, 90.0f},
            {"cqb_start_1", STAND_STOP, STAND_WALK, 135.0f},
            {"cqb_start_2", STAND_STOP, STAND_WALK, 180.0f},
            {"cqb_start_9", STAND_STOP, STAND_WALK, -45.0f},
            {"cqb_start_6", STAND_STOP, STAND_WALK, -90.0f},
            {"cqb_start_3", STAND_STOP, STAND_WALK, -135.0f},
            {"cqb_stop_2", STAND_WALK, STAND_STOP, 0.0f},
            {"cqb_stop_7", STAND_WALK, STAND_STOP, 45.0f},
            {"cqb_stop_4", STAND_WALK, STAND_STOP, 90.0f},
            {"cqb_stop_1", STAND_WALK, STAND_STOP, 135.0f},
            {"cqb_stop_8", STAND_WALK, STAND_STOP, 180.0f},
            {"cqb_stop_9", STAND_WALK, STAND_STOP, -45.0f},
            {"cqb_stop_6", STAND_WALK, STAND_STOP, -90.0f},
            {"cqb_stop_3", STAND_WALK, STAND_STOP, -135.0f},
            {"cqb_crouch_start_8", CROUCH_STOP, STAND_WALK, 0.0f},
            {"cqb_crouch_start_7", CROUCH_STOP, STAND_WALK, 45.0f},
            {"cqb_crouch_start_4", CROUCH_STOP, STAND_WALK, 90.0f},
            {"cqb_crouch_start_1", CROUCH_STOP, STAND_WALK, 135.0f},
            {"cqb_crouch_start_2", CROUCH_STOP, STAND_WALK, 180.0f},
            {"cqb_crouch_start_9", CROUCH_STOP, STAND_WALK, -45.0f},
            {"cqb_crouch_start_6", CROUCH_STOP, STAND_WALK, -90.0f},
            {"cqb_crouch_start_3", CROUCH_STOP, STAND_WALK, -135.0f},
            {"cqb_crouch_stop_2", CROUCH_WALK, CROUCH_STOP, 0.0f},
            {"cqb_crouch_stop_1", CROUCH_WALK, CROUCH_STOP, 45.0f},
            {"cqb_crouch_stop_4", CROUCH_WALK, CROUCH_STOP, 90.0f},
            {"cqb_crouch_stop_7", CROUCH_WALK, CROUCH_STOP, 135.0f},
            {"cqb_crouch_stop_8", CROUCH_WALK, CROUCH_STOP, 180.0f},
            {"cqb_crouch_stop_3", CROUCH_WALK, CROUCH_STOP, -45.0f},
            {"cqb_crouch_stop_6", CROUCH_WALK, CROUCH_STOP, -90.0f},
            {"cqb_crouch_stop_9", CROUCH_WALK, CROUCH_STOP, -135.0f},
            {"cqb_walk_turn_7", STAND_WALK, STAND_WALK, 45.0f},
            {"cqb_walk_turn_4", STAND_WALK, STAND_WALK, 90.0f},
            {"cqb_walk_turn_2", STAND_WALK, STAND_WALK, 180.0f},
            {"cqb_walk_turn_9", STAND_WALK, STAND_WALK, -45.0f},
            {"cqb_walk_turn_6", STAND_WALK, STAND_WALK, -90.0f},
            {"cqb_walk_turn_2", STAND_WALK, STAND_WALK, -180.0f},
            // and from and to its run
            {"cqb_start_8", STAND_STOP, STAND_RUN, 0.0f},
            {"cqb_start_7", STAND_STOP, STAND_RUN, 45.0f},
            {"cqb_start_4", STAND_STOP, STAND_RUN, 90.0f},
            {"cqb_start_1", STAND_STOP, STAND_RUN, 135.0f},
            {"cqb_start_2", STAND_STOP, STAND_RUN, 180.0f},
            {"cqb_start_9", STAND_STOP, STAND_RUN, -45.0f},
            {"cqb_start_6", STAND_STOP, STAND_RUN, -90.0f},
            {"cqb_start_3", STAND_STOP, STAND_RUN, -135.0f},
            {"cqb_stop_2", STAND_RUN, STAND_STOP, 0.0f},
            {"cqb_stop_7", STAND_RUN, STAND_STOP, 45.0f},
            {"cqb_stop_4", STAND_RUN, STAND_STOP, 90.0f},
            {"cqb_stop_1", STAND_RUN, STAND_STOP, 135.0f},
            {"cqb_stop_8", STAND_RUN, STAND_STOP, 180.0f},
            {"cqb_stop_9", STAND_RUN, STAND_STOP, -45.0f},
            {"cqb_stop_6", STAND_RUN, STAND_STOP, -90.0f},
            {"cqb_stop_3", STAND_RUN, STAND_STOP, -135.0f},
            {"cqb_crouch_start_8", CROUCH_STOP, STAND_RUN, 0.0f},
            {"cqb_crouch_start_4", CROUCH_STOP, STAND_RUN, 90.0f},
            {"cqb_crouch_start_2", CROUCH_STOP, STAND_RUN, 180.0f},
            {"cqb_crouch_start_6", CROUCH_STOP, STAND_RUN, -90.0f},
            {"cqb_walk_turn_7", STAND_RUN, STAND_RUN, 45.0f},
            {"cqb_walk_turn_4", STAND_RUN, STAND_RUN, 90.0f},
            {"cqb_walk_turn_9", STAND_RUN, STAND_RUN, -45.0f},
            {"cqb_walk_turn_6", STAND_RUN, STAND_RUN, -90.0f},
        }};

        // The players' (mp/playeranim.script): turns on the spot (the game turns the legs in code as they play, up
        // to loop_begin) and changes of stance, by the gun's class; no starts or stops (the game blends its runs in)
        constexpr std::array<Transition, 13> PLAYERS_RIFLE{{
            {"pb_turn_90left", STAND_STOP, STAND_STOP, 90.0f},
            {"pb_turn_90right", STAND_STOP, STAND_STOP, -90.0f},
            {"pb_crouchturn_90left", CROUCH_STOP, CROUCH_STOP, 90.0f},
            {"pb_crouchturn_90right", CROUCH_STOP, CROUCH_STOP, -90.0f},
            {"pb_prone_turn_90left", PRONE_STOP, PRONE_STOP, 90.0f},
            {"pb_prone_turn_90right", PRONE_STOP, PRONE_STOP, -90.0f},
            {"pb_rifle_stand2crouch", STAND_STOP, CROUCH_STOP, 0.0f},
            {"pb_rifle_crouch2stand", CROUCH_STOP, STAND_STOP, 0.0f},
            {"pb_crouch2prone", CROUCH_STOP, PRONE_STOP, 0.0f},
            {"pb_prone2crouch", PRONE_STOP, CROUCH_STOP, 0.0f},
            {"pb_prone2stand", PRONE_STOP, STAND_STOP, 0.0f},
            {"pb_prone2sprint", PRONE_STOP, STAND_SPRINT, 0.0f},
            {"pb_prone2sprint", PRONE_STOP, STAND_RUN, 0.0f},
        }};
        constexpr std::array<Transition, 12> PLAYERS_PISTOL{{
            {"pb_pistol_turn_90left", STAND_STOP, STAND_STOP, 90.0f},
            {"pb_pistol_turn_90right", STAND_STOP, STAND_STOP, -90.0f},
            {"pb_pistol_crouchturn_90left", CROUCH_STOP, CROUCH_STOP, 90.0f},
            {"pb_pistol_crouchturn_90right", CROUCH_STOP, CROUCH_STOP, -90.0f},
            {"pb_pistol_prone_turn_90left", PRONE_STOP, PRONE_STOP, 90.0f},
            {"pb_pistol_prone_turn_90right", PRONE_STOP, PRONE_STOP, -90.0f},
            {"pb_pistol_stand2crouch", STAND_STOP, CROUCH_STOP, 0.0f},
            {"pb_pistol_crouch2stand", CROUCH_STOP, STAND_STOP, 0.0f},
            {"pb_crouch2prone_pistol", CROUCH_STOP, PRONE_STOP, 0.0f},
            {"pb_prone2crouch_pistol", PRONE_STOP, CROUCH_STOP, 0.0f},
            {"pb_prone2stand_pistol", PRONE_STOP, STAND_STOP, 0.0f},
            {"pb_prone2sprint_pistol", PRONE_STOP, STAND_SPRINT, 0.0f},
        }};
        constexpr std::array<Transition, 8> PLAYERS_ROCKET{{
            {"pb_rpg_stand_turn90l", STAND_STOP, STAND_STOP, 90.0f},
            {"pb_rpg_stand_turn90r", STAND_STOP, STAND_STOP, -90.0f},
            {"pb_rpg_crouch_turn90l", CROUCH_STOP, CROUCH_STOP, 90.0f},
            {"pb_rpg_crouch_turn90r", CROUCH_STOP, CROUCH_STOP, -90.0f},
            {"pb_rpg_prone_turn90l", PRONE_STOP, PRONE_STOP, 90.0f},
            {"pb_rpg_prone_turn90r", PRONE_STOP, PRONE_STOP, -90.0f},
            {"pb_rpg_crouch2prone", CROUCH_STOP, PRONE_STOP, 0.0f},
            {"pb_rpg_prone2crouch", PRONE_STOP, CROUCH_STOP, 0.0f},
        }};
        constexpr std::array<Transition, 12> PLAYERS_DUAL_WIELD{{
            {"pb_dw_turn_90left", STAND_STOP, STAND_STOP, 90.0f},
            {"pb_dw_turn_90right", STAND_STOP, STAND_STOP, -90.0f},
            {"pb_dw_crouchturn_90left", CROUCH_STOP, CROUCH_STOP, 90.0f},
            {"pb_dw_crouchturn_90right", CROUCH_STOP, CROUCH_STOP, -90.0f},
            {"pb_dw_prone_turn_90left", PRONE_STOP, PRONE_STOP, 90.0f},
            {"pb_dw_prone_turn_90right", PRONE_STOP, PRONE_STOP, -90.0f},
            {"pb_dw_stand2crouch", STAND_STOP, CROUCH_STOP, 0.0f},
            {"pb_dw_crouch2stand", CROUCH_STOP, STAND_STOP, 0.0f},
            {"pb_dw_crouch2prone", CROUCH_STOP, PRONE_STOP, 0.0f},
            {"pb_dw_prone2crouch", PRONE_STOP, CROUCH_STOP, 0.0f},
            {"pb_dw_prone2stand", PRONE_STOP, STAND_STOP, 0.0f},
            {"pb_dw_prone2sprint", PRONE_STOP, STAND_SPRINT, 0.0f},
        }};

        // The dogs' (multiplayer's dog scripts, Zombies' hellhound's): setting off running (straight, or turning
        // round), stopping, turning on the spot and running
        constexpr std::array<Transition, 10> SHEPHERD{{
            {"german_shepherd_run_start", STAND_STOP, STAND_RUN, 0.0f},
            {"german_shepherd_run_start_180_l", STAND_STOP, STAND_RUN, 180.0f},
            {"german_shepherd_run_start_180_r", STAND_STOP, STAND_RUN, -180.0f},
            {"german_shepherd_run_stop", STAND_RUN, STAND_STOP, 0.0f},
            {"german_shepard_turn_90_left", STAND_STOP, STAND_STOP, 90.0f},
            {"german_shepard_turn_90_right", STAND_STOP, STAND_STOP, -90.0f},
            {"german_shepard_run_turn_90_left", STAND_RUN, STAND_RUN, 90.0f},
            {"german_shepard_run_turn_90_right", STAND_RUN, STAND_RUN, -90.0f},
            {"german_shepard_run_turn_180_left", STAND_RUN, STAND_RUN, 180.0f},
            {"german_shepard_run_turn_180_right", STAND_RUN, STAND_RUN, -180.0f},
        }};
        // (its starts neither move nor turn it: the game's code does, so it sets off with the shepherd's)
        constexpr std::array<Transition, 9> HELLHOUND{{
            {"zombie_dog_run_stop", STAND_RUN, STAND_STOP, 0.0f},
            {"zombie_dog_turn_90_left", STAND_STOP, STAND_STOP, 90.0f},
            {"zombie_dog_turn_90_right", STAND_STOP, STAND_STOP, -90.0f},
            {"zombie_dog_turn_180_left", STAND_STOP, STAND_STOP, 180.0f},
            {"zombie_dog_turn_180_right", STAND_STOP, STAND_STOP, -180.0f},
            {"zombie_dog_run_turn_90_left", STAND_RUN, STAND_RUN, 90.0f},
            {"zombie_dog_run_turn_90_right", STAND_RUN, STAND_RUN, -90.0f},
            {"zombie_dog_run_turn_180_left", STAND_RUN, STAND_RUN, 180.0f},
            {"zombie_dog_run_turn_180_right", STAND_RUN, STAND_RUN, -180.0f},
        }};

        template <std::size_t N>
        void Append(std::vector<Transition>& to, const std::array<Transition, N>& from)
        {
            to.insert(to.end(), from.begin(), from.end());
        }

        // whether a transition from or to a state goes with a state an actor is in (one that sprints goes as one that
        // runs where no sprint's is listed first)
        bool Matches(const State& listed, const State& state)
        {
            return listed.posture == state.posture &&
                   (listed.movement == state.movement ||
                    (listed.movement == Movement::Run && state.movement == Movement::Sprint));
        }

        // how far a transition's turn is from the turn wanted (degrees, the short way round; straight back counts
        // as both)
        float TurnError(float listed, float wanted)
        {
            return std::abs(std::remainder(listed - wanted, 360.0f));
        }
    }  // namespace

    std::vector<Transition> GetTransitions(const ActorLogic::Body& body)
    {
        std::vector<Transition> list;
        // (a killstreak has none: the game moves it in code)
        if (IsVehicle(body))
            return list;
        if (IsDog(body))
        {
            if (body.breed == ActorLogic::DogBreed::Hellhound)
                Append(list, HELLHOUND);
            Append(list, SHEPHERD);
            return list;
        }
        switch (body.set)
        {
            case Set::Cqb:
                Append(list, CQB);
                Append(list, LOW_READY);
                break;
            case Set::LowReady:
            case Set::RunAndGun:
                Append(list, LOW_READY);
                break;
            default:
                switch (body.gun)
                {
                    case Types::GunClass::Pistol:
                        Append(list, PLAYERS_PISTOL);
                        break;
                    case Types::GunClass::RocketLauncher:
                        Append(list, PLAYERS_ROCKET);
                        break;
                    case Types::GunClass::DualWield:
                    case Types::GunClass::BallisticKnife:
                        Append(list, PLAYERS_DUAL_WIELD);
                        break;
                    default:
                        break;
                }
                Append(list, PLAYERS_RIFLE);
                break;
        }
        return list;
    }

    Blends GetBlends(const ActorLogic::Body& body, const State& from, const State& to)
    {
        // a dog's are quick (its scripts blend in a tenth of a second)
        if (IsDog(body))
            return {100, 100};
        // the players' as the legs blend (BG_SetNewAnimation): from standing, with a stance change taking longer
        if (body.set == Set::Players || body.set == Set::Auto)
            return from.posture != to.posture ? Blends{ActorLogic::LEGS_BLEND_STANCE, ActorLogic::LEGS_BLEND}
                                              : Blends{ActorLogic::LEGS_BLEND, ActorLogic::LEGS_BLEND};
        // the AI's (animscripts): a running turn from a tenth of a second, its run back in the last 50 ms (turn.gsc);
        // setting off in 0.2 s, running on in its last 0.15 s (cover_arrival.gsc's exits); stopping in 0.3 s, its idle
        // in 0.3 s (the arrivals, stop.gsc's transitionToIdle); a change of stance or a turn on the spot in 0.2 s
        const bool fromMoving = IsMoving(from.movement), toMoving = IsMoving(to.movement);
        if (fromMoving && toMoving)
            return {100, 50};
        if (toMoving)
            return {200, 150};
        if (fromMoving)
            return {300, 300};
        return {200, 200};
    }

    std::string Describe(const State& from, const State& to, float turn)
    {
        const auto turning =
            std::abs(turn) >= 22.5f
                ? std::format("{} {:.0f} deg", turn > 0.0f ? "left" : "right", std::abs(turn) > 179.0f ? 180.0f
                                                                                                      : std::abs(turn))
                : std::string();
        const auto gait = [](Movement movement) {
            switch (movement)
            {
                case Movement::Walk:
                    return "walking";
                case Movement::Sprint:
                    return "sprinting";
                default:
                    return "running";
            }
        };
        const bool fromMoving = IsMoving(from.movement), toMoving = IsMoving(to.movement);
        std::string text;
        if (from.posture != to.posture)
        {
            switch (to.posture)
            {
                case Posture::Crouch:
                    text = fromMoving && !toMoving ? "Stop and crouch" : "Crouch";
                    break;
                case Posture::Prone:
                    text = "Go prone";
                    break;
                case Posture::Back:
                    text = "Lie on its back";
                    break;
                default:
                    text = from.posture == Posture::Crouch ? "Stand up" : "Get up";
                    break;
            }
            if (toMoving)
                text += std::format(" and set off {}", gait(to.movement));
        }
        else if (!fromMoving && toMoving)
            text = std::format("Set off {}", gait(to.movement));
        else if (fromMoving && !toMoving)
            text = "Stop";
        else if (!turning.empty())
            return std::format("Turn {}{}", turning, toMoving ? std::format(", {}", gait(to.movement)) : "");
        else
            text = "Go on";
        return turning.empty() ? text : std::format("{}, turning {}", text, turning);
    }

    namespace
    {
        // the transitions of the body's that the game has (the animations it can play)
        std::vector<Transition> GetAvailable(const ActorLogic::Body& body,
                                             const ActorLogic::AnimationLookup& animations)
        {
            auto list = GetTransitions(body);
            std::erase_if(list, [&](const Transition& transition) {
                return !animations(std::string(transition.animation)).has_value();
            });
            return list;
        }

        Link MakeLink(const Transition& transition, const ActorLogic::Body& body, std::optional<float> turn)
        {
            return {std::string(transition.animation), turn, GetBlends(body, transition.from, transition.to),
                    Describe(transition.from, transition.to, turn.value_or(transition.turn))};
        }
    }  // namespace

    std::vector<Link> JoinStates(const State& from, const State& to, const ActorLogic::Body& body,
                                 const ActorLogic::AnimationLookup& animations, float turn)
    {
        if (!IsKnown(from) || !IsKnown(to))
            return {};
        const bool same = from == to;
        if (same && std::abs(turn) <= TURN_THRESHOLD)
            return {};
        const auto list = GetAvailable(body, animations);

        // the one that goes from one to the other, turning nearest the turn wanted (as one that turns, between alike
        // states); it turns the actor as far as wanted when it turns or is asked to
        const Transition* best = nullptr;
        for (const auto& transition : list)
        {
            if (!Matches(transition.from, from) || !Matches(transition.to, to) || (same && transition.turn == 0.0f))
                continue;
            if (!best || TurnError(transition.turn, turn) < TurnError(best->turn, turn))
                best = &transition;
        }
        // (the way round its animation turns: turning back, to its side)
        const auto turnOf = [&](const Transition& transition, float wanted) -> std::optional<float> {
            if (transition.turn == 0.0f && std::abs(wanted) < 1.0f)
                return std::nullopt;
            return transition.turn + std::remainder(wanted - transition.turn, 360.0f);
        };
        if (best)
            return {MakeLink(*best, body, turnOf(*best, turn))};
        if (same)
            return {};

        // two, through a state between them (prone to running: getting up into a crouch, then setting off from it; not
        // down and up again: crouching to a crouched run doesn't go prone), the first of the list's order whose turns
        // together are nearest
        const auto height = [](Types::AnimationPosture posture) {
            switch (posture)
            {
                case Types::AnimationPosture::Stand:
                    return 2;
                case Types::AnimationPosture::Crouch:
                    return 1;
                default:
                    return 0;
            }
        };
        const auto between = [&](Types::AnimationPosture posture) {
            const auto low = std::min(height(from.posture), height(to.posture));
            const auto high = std::max(height(from.posture), height(to.posture));
            return height(posture) >= low && height(posture) <= high;
        };
        const Transition *first = nullptr, *second = nullptr;
        float error = 0.0f;
        for (const auto& a : list)
        {
            if (!Matches(a.from, from) || a.to == from || a.to == to || !between(a.to.posture))
                continue;
            for (const auto& b : list)
            {
                if (!Matches(b.from, a.to) || !Matches(b.to, to))
                    continue;
                const auto e = TurnError(a.turn + b.turn, turn);
                if (!first || e < error - 1e-3f)
                {
                    first = &a;
                    second = &b;
                    error = e;
                }
            }
        }
        if (!first)
            return {};
        // (the second turns what is left of the turn)
        return {MakeLink(*first, body, std::nullopt), MakeLink(*second, body, turnOf(*second, turn - first->turn))};
    }

    std::vector<Link> JoinClips(const Types::Actor& actor, std::size_t from, std::size_t to,
                                const ActorLogic::Body& body, const ActorLogic::AnimationLookup& animations, float turn)
    {
        if (from >= actor.clips.size() || to >= actor.clips.size())
            return {};
        // (a death plays at once, and nothing goes on from one)
        for (const auto index : {from, to})
        {
            const auto& clip = actor.clips[index];
            const auto info = animations(clip.animation);
            if (info && info->death.value_or(ActorLogic::IsDeathAnimation(clip.animation, clip.loop, info->notetracks)))
                return {};
        }
        return JoinStates(GetClipState(actor, from, true, body, animations),
                          GetClipState(actor, to, false, body, animations), body, animations, turn);
    }

    Pose MakePose(const Types::AnimationPose& pose)
    {
        Pose made;
        for (std::size_t i = 0; i < SIGNATURE_BONES.size(); ++i)
            if (const auto bone = pose.FindBone(SIGNATURE_BONES[i]); bone && *bone < pose.positions.size())
                made[i] = pose.positions[*bone];
        return made;
    }

    bool IsCleanJoin(float distance, std::uint32_t blendIn)
    {
        return distance * 50.0f / static_cast<float>(std::max(blendIn, 50u)) <= CLEAN_JOIN;
    }

    std::optional<float> GetPoseDistance(const Pose& a, const Pose& b)
    {
        float sum = 0.0f;
        std::size_t count = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i] && b[i])
            {
                const auto d = *a[i] - *b[i];
                sum += glm::dot(d, d);
                ++count;
            }
        if (count == 0)
            return std::nullopt;
        return std::sqrt(sum / static_cast<float>(count));
    }

    std::vector<Candidate> ChainCandidates(const Types::Actor& actor, std::size_t index, const ActorLogic::Body& body,
                                           const Types::AnimationCatalog& catalog,
                                           const ActorLogic::AnimationLookup& animations,
                                           const std::function<bool(const Types::AnimationEntry&)>& fits,
                                           const PoseLookup& poses, std::size_t most)
    {
        std::vector<Candidate> candidates;
        if (index >= actor.clips.size())
            return candidates;
        const auto& clip = actor.clips[index];
        const auto state = GetClipState(actor, index, true, body, animations);
        const auto add = [&](std::string animation, std::string label, bool table) {
            if (candidates.size() >= most ||
                std::ranges::find(candidates, animation, &Candidate::animation) != candidates.end())
                return;
            candidates.push_back({std::move(animation), std::move(label), table, std::nullopt});
        };
        const auto fitting = [&](std::string_view name) {
            const auto entry = catalog.Find(name);
            return !entry || !fits || fits(*entry);
        };

        // the set's own, as the game's tables have them
        if (IsKnown(state))
            for (const auto& transition : GetAvailable(body, animations))
                if (Matches(transition.from, state) && fitting(transition.animation))
                    add(std::string(transition.animation), Describe(transition.from, transition.to, transition.turn),
                        true);

        // the catalog's others that change the state (or turn), one-offs that aren't deaths: those that start in it, or
        // where it isn't known all of them, the nearest the clip's last pose first
        struct Other
        {
            const Types::AnimationEntry* entry;
            std::optional<float> distance;
        };
        std::vector<Other> others;
        const auto last = poses ? poses(clip.animation) : nullptr;
        for (const auto& entry : catalog.entries)
        {
            if (entry.loop || entry.death || !IsKnown(entry.from) || !IsKnown(entry.to) ||
                (entry.from == entry.to && std::abs(entry.turn) < 22.5f) || (IsKnown(state) && !Matches(entry.from, state)) ||
                (fits && !fits(entry)))
                continue;
            others.push_back({&entry, std::nullopt});
        }
        if (!IsKnown(state) && last && poses)
        {
            for (auto& other : others)
                if (const auto signature = poses(other.entry->name))
                    other.distance = GetPoseDistance(last->last, signature->first);
            std::ranges::stable_sort(others, [](const Other& a, const Other& b) {
                return a.distance.value_or(FLT_MAX) < b.distance.value_or(FLT_MAX);
            });
        }
        for (const auto& other : others)
        {
            const auto before = candidates.size();
            add(other.entry->name, Describe(other.entry->from, other.entry->to, other.entry->turn), false);
            if (candidates.size() > before)
                candidates.back().distance = other.distance;
        }
        if (last && poses)
            for (auto& candidate : candidates)
                if (!candidate.distance)
                    if (const auto signature = poses(candidate.animation))
                        candidate.distance = GetPoseDistance(last->last, signature->first);
        return candidates;
    }

    namespace
    {
        constexpr std::uint32_t MIN_LENGTH = 100;

        std::optional<std::size_t> FindClip(const Types::Actor& actor, std::uint32_t clipId)
        {
            const auto found = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id);
            return found != actor.clips.end() ? std::optional(static_cast<std::size_t>(found - actor.clips.begin()))
                                              : std::nullopt;
        }

        // Moves a clip by so many ticks (later, or earlier when negative), as the chain before it grows (or goes): a
        // looping one gives up (or gets back) as much of its start, keeping its end; one that doesn't, and the clips
        // after it, move, with the Hits their deaths start with
        void Shift(Types::Actor& actor, std::size_t index, std::int64_t by)
        {
            auto& clip = actor.clips[index];
            const auto end = static_cast<std::int64_t>(clip.End());
            if (clip.loop && end - (static_cast<std::int64_t>(clip.start) + by) >= MIN_LENGTH)
            {
                const auto start = std::max<std::int64_t>(static_cast<std::int64_t>(clip.start) + by, 0);
                clip.start = static_cast<std::uint32_t>(start);
                clip.length = static_cast<std::uint32_t>(end - start);
                return;
            }
            for (auto k = index; k < actor.clips.size(); ++k)
            {
                auto& moved = actor.clips[k];
                const auto start = static_cast<std::uint32_t>(
                    std::max<std::int64_t>(static_cast<std::int64_t>(moved.start) + by, 0));
                for (auto& event : actor.events)
                    if (event.type == Types::ActorEventType::Hit && event.tick == moved.start && !moved.loop)
                        event.tick = start;
                moved.start = start;
            }
        }

        // the chain's links put in before the clip at an index, which starts after them; the clip before ends where
        // they start
        void Insert(Types::Actor& actor, std::size_t index, const std::vector<Link>& links,
                    const ActorLogic::AnimationLookup& animations)
        {
            const auto at = actor.clips[index].start;
            if (index > 0)
            {
                auto& before = actor.clips[index - 1];
                if (before.End() > at)
                    before.length = std::max(1u, at - before.start);
            }
            std::vector<Types::ActorClip> chain;
            auto start = at;
            std::uint32_t blendIn = 0;
            for (std::size_t i = 0; i < links.size(); ++i)
            {
                const auto& link = links[i];
                const auto info = animations(link.animation);
                const auto duration = ToTicks(info ? info->duration : 1.0f);
                Types::ActorClip clip{.animation = link.animation,
                                      .start = start,
                                      .length = std::max(MIN_LENGTH, duration - std::min(duration, link.blends.handOver)),
                                      .loop = false,
                                      .blendIn = i == 0 ? link.blends.in : blendIn,
                                      .moves = false,
                                      .chain = Types::ActorChain::Join,
                                      .turn = link.turn};
                start = clip.End();
                blendIn = link.blends.handOver;
                chain.push_back(std::move(clip));
            }
            Shift(actor, index, static_cast<std::int64_t>(start) - at);
            actor.clips[index].blendIn = blendIn;
            actor.clips.insert(actor.clips.begin() + static_cast<std::ptrdiff_t>(index), chain.begin(), chain.end());
            ActorLogic::Normalize(actor);
        }

        bool IsJoinable(const Types::ActorClip& clip)
        {
            return clip.chain == Types::ActorChain::None;
        }

        // a walk of a path that the path's own transitions start, turn and stop (see PlanPath)
        bool IsPathWalk(const Types::Actor& actor, std::size_t index, const ActorLogic::AnimationLookup& animations)
        {
            if (actor.movement != Types::ActorMovement::Path || actor.locomotion != Types::ActorLocomotion::Auto ||
                !ActorLogic::FindLocomotion(actor, actor.clips[index].animation))
                return false;
            const auto walk = ActorLogic::GetClipWalk(actor, actor.clips[index], animations);
            return walk.angle && walk.speed >= ActorLogic::IDLE_SPEED;
        }
    }  // namespace

    bool Unjoin(Types::Actor& actor, std::uint32_t clipId)
    {
        bool changed = false;
        // the transitions before it, and those after it (before the clip after it)
        for (const bool after : {false, true})
        {
            const auto index = FindClip(actor, clipId);
            if (!index)
                return changed;
            auto first = *index, last = *index;  // the transitions are [first, last)
            if (!after)
            {
                while (first > 0 && actor.clips[first - 1].chain == Types::ActorChain::Join)
                    --first;
            }
            else
            {
                first = last = *index + 1;
                while (last < actor.clips.size() && actor.clips[last].chain == Types::ActorChain::Join)
                    ++last;
            }
            if (first == last)
                continue;
            const auto start = actor.clips[first].start;
            actor.clips.erase(actor.clips.begin() + static_cast<std::ptrdiff_t>(first),
                              actor.clips.begin() + static_cast<std::ptrdiff_t>(last));
            // the clip after them back to where they started
            if (first < actor.clips.size())
                Shift(actor, first, static_cast<std::int64_t>(start) - actor.clips[first].start);
            ActorLogic::Normalize(actor);
            changed = true;
        }
        return changed;
    }

    bool JoinAround(Types::Actor& actor, std::uint32_t clipId, const ActorLogic::Body& body,
                    const ActorLogic::AnimationLookup& animations)
    {
        if (!actor.autoTransitions)
            return false;
        auto changed = Unjoin(actor, clipId);
        // to the clip before it, then the clip after it to it
        for (const bool after : {false, true})
        {
            const auto index = FindClip(actor, clipId);
            if (!index || !IsJoinable(actor.clips[*index]))
                return changed;
            const auto from = after ? *index : *index - 1, to = after ? *index + 1 : *index;
            if ((!after && *index == 0) || to >= actor.clips.size() || !IsJoinable(actor.clips[from]) ||
                !IsJoinable(actor.clips[to]) || IsPathWalk(actor, from, animations) || IsPathWalk(actor, to, animations))
                continue;
            const auto links = JoinClips(actor, from, to, body, animations);
            if (links.empty())
                continue;
            Insert(actor, to, links, animations);
            changed = true;
        }
        return changed;
    }

    bool MarkTurn(Types::ActorClip& clip, const ActorLogic::AnimationLookup& animations)
    {
        if (clip.chain != Types::ActorChain::None)
            return false;
        std::optional<float> turn;
        if (const auto info = animations(clip.animation); info && info->turn && info->from && info->to)
            turn = info->turn;
        if (clip.turn == turn)
            return false;
        clip.turn = turn;
        return true;
    }

    std::uint32_t PlayAfter(Types::Actor& actor, std::uint32_t clipId, const std::string& animation,
                            const ActorLogic::Body& body, const ActorLogic::AnimationLookup& animations)
    {
        const auto info = animations(animation);
        if (!info || !FindClip(actor, clipId))
            return 0;
        // (the transitions after it are joined again, to the new clip)
        Unjoin(actor, clipId);
        const auto index = *FindClip(actor, clipId);
        const auto& before = actor.clips[index];
        auto at = before.End();
        if (index + 1 < actor.clips.size())
            at = std::min(at, actor.clips[index + 1].start);
        // one play, as picked (looping if it loops: a longer clip plays it again); a death as every death is made, the
        // clips after it gone
        Types::ActorClip clip{.animation = animation, .start = at, .loop = info->loop, .picked = true};
        clip.length = ToTicks(info->duration);
        if (ActorLogic::IsDeathClip(clip, animations))
        {
            clip.blendIn = ActorLogic::DEATH_BLEND;
            const auto id = ActorLogic::AddClip(actor, clip);
            ActorLogic::MakeDeath(actor, id, info->duration);
            return id;
        }
        MarkTurn(clip, animations);
        // the game's blends of a transition from the clip it follows
        const auto from = GetClipState(actor, index, true, body, animations);
        if (info->from && info->to && IsKnown(from) && IsKnown(*info->from) && *info->from != *info->to)
            clip.blendIn = GetBlends(body, *info->from, *info->to).in;
        // the clips after it make room
        if (index + 1 < actor.clips.size() && actor.clips[index + 1].start < clip.End())
            Shift(actor, index + 1, static_cast<std::int64_t>(clip.End()) - actor.clips[index + 1].start);
        const auto id = ActorLogic::AddClip(actor, clip);
        JoinAround(actor, id, body, animations);
        return id;
    }

    namespace
    {
        // the tick between two where a test of them changes sign (the nearest it gets to 0), none when it doesn't
        std::optional<std::uint32_t> Solve(std::uint32_t low, std::uint32_t high,
                                           const std::function<float(std::uint32_t)>& test)
        {
            if (high <= low)
                return std::nullopt;
            auto atLow = test(low);
            const auto atHigh = test(high);
            if (atLow == 0.0f)
                return low;
            if ((atLow < 0.0f) == (atHigh < 0.0f))
                return std::nullopt;
            while (high - low > 1)
            {
                const auto middle = low + (high - low) / 2;
                const auto atMiddle = test(middle);
                if ((atMiddle < 0.0f) == (atLow < 0.0f))
                {
                    low = middle;
                    atLow = atMiddle;
                }
                else
                    high = middle;
            }
            return std::abs(atLow) <= std::abs(test(high)) ? low : high;
        }

        // A clip of each link of a chain from a tick on, each from where the one before hands over to it; returns where
        // the last hands over (the clip after it starts there, blending in as long)
        std::uint32_t AddLinks(std::vector<Types::ActorClip>& clips, std::size_t at, const std::vector<Link>& links,
                               std::uint32_t start, Types::ActorChain chain, std::optional<std::uint32_t> corner,
                               const ActorLogic::AnimationLookup& animations, std::uint32_t* handOver)
        {
            std::uint32_t blendIn = 0;
            for (std::size_t i = 0; i < links.size(); ++i)
            {
                const auto& link = links[i];
                const auto info = animations(link.animation);
                const auto duration = ToTicks(info ? info->duration : 1.0f);
                Types::ActorClip clip{
                    .animation = link.animation,
                    .start = start,
                    .length = std::max(100u, duration - std::min(duration, link.blends.handOver)),
                    .loop = false,
                    .blendIn = i == 0 ? link.blends.in : blendIn,
                    .moves = false,
                    .chain = chain,
                    .turn = link.turn,
                    .corner = corner,
                };
                start = clip.End();
                blendIn = link.blends.handOver;
                clips.insert(clips.begin() + static_cast<std::ptrdiff_t>(at + i), std::move(clip));
            }
            if (handOver)
                *handOver = blendIn;
            return start;
        }

        // when a link's animation ends, from its clip's start (as far as it moves the actor)
        std::uint32_t GetMotionEnd(const Types::ActorClip& clip, const ActorLogic::AnimationLookup& animations)
        {
            const auto info = animations(clip.animation);
            return clip.start + ToTicks(info ? info->duration : 1.0f);
        }

        // how near the end of the walk a plan gets the actor for it to have got there
        constexpr float ARRIVED = 4.0f;

        // the blend the walk after a turn or a wait's start at a point takes over from it over: the join's of the
        // stretch from the point (kept as the path is planned again), else as planned
        std::uint32_t GetJoinBlendIn(const Types::Actor& actor, std::size_t point, std::uint32_t planned)
        {
            if (point >= actor.stretches.size())
                return planned;
            const auto& stretch = actor.stretches[point];
            return stretch.blend && !stretch.joinAsBefore ? *stretch.blend : planned;
        }

        // what planning a walk did: the corners it turned at (their points, in order), and whether the actor gets to
        // where the walk ends, with its stop if it was to stop
        struct WalkPlan
        {
            std::vector<std::size_t> turned;
            bool arrives = true;
        };

        // Plans the transitions of one walk along the path (see PlanPath), the clip of that id, its own taken out; the
        // corners at the points given are walked round
        WalkPlan PlanOneWalk(Types::Actor& actor, std::uint32_t walkId, std::optional<float> stopTurn,
                             const ActorLogic::Body& body, const ActorLogic::AnimationLookup& animations,
                             std::span<const std::size_t> roundCorners = {})
        {
            WalkPlan plan;
            const auto found = FindClip(actor, walkId);
            if (!found)
                return plan;
            const auto index = *found;
            const auto walk = actor.clips[index];
            const auto walkState = GetClipState(actor, index, false, body, animations);
            if (!IsMoving(walkState.movement))
                return plan;
            const auto path = ActorLogic::GetRoundedPath(actor, animations);
            if (path.pieces.empty())
                return plan;
            // where along the path it walks from and to, as it was fitted (to the path's end, or a stop on it)
            const auto next = index + 1 < actor.clips.size() && actor.clips[index + 1].start <= walk.End()
                                  ? std::optional(index + 1)
                                  : std::nullopt;
            const auto from = ActorLogic::GetPathProgress(actor, walk.start, animations);
            const auto to = ActorLogic::GetPathProgress(
                actor, walk.End() + (next ? actor.clips[*next].blendIn : 0u), animations);
            if (to - from < 50.0f)
                return plan;
            const auto end = ActorLogic::PlaceOnPath(path, to);
            const auto endPoint = glm::vec2(end.position);
            const auto endWay = glm::vec2(std::cos(glm::radians(end.yaw)), std::sin(glm::radians(end.yaw)));

            // as it stood before it and stands after it
            const auto before = index > 0 ? GetClipState(actor, index - 1, true, body, animations) : State{};
            const auto after = next ? GetClipState(actor, *next, false, body, animations) : State{};
            // the way it faces as it sets off (at the path's start, as it faced before it walked) and the way it goes
            const auto facing = from < 0.5f ? actor.yaw : ActorLogic::EvaluatePlacement(actor, walk.start, animations).yaw;
            float firstWay = ActorLogic::PlaceOnPath(path, from + 1.0f).yaw;
            for (const auto& piece : path.pieces)
                if (piece.radius <= 0.0f && piece.distance + piece.length > from + 1.0f)
                {
                    firstWay = glm::degrees(std::atan2(piece.direction.y, piece.direction.x));
                    break;
                }
            const auto onSpot = [&](Posture posture, float turn) {
                return JoinStates({posture, Movement::Stop}, {posture, Movement::Stop}, body, animations, turn);
            };

            // setting off: its start, or a turn on the spot
            std::vector<Link> start;
            const auto startTurn = std::remainder(firstWay - facing, 360.0f);
            if (IsKnown(before) && !IsMoving(before.movement))
            {
                start = JoinStates(before, walkState, body, animations, startTurn);
                if (start.empty() && std::abs(startTurn) > TURN_THRESHOLD)
                    start = onSpot(before.posture, startTurn);
            }
            // the corners it runs through, turning more than the threshold: a running turn, or stopping to turn
            struct Corner
            {
                std::size_t point;
                glm::vec2 position, in, out;  // the corner's point, and the ways in and out of it
                float angle;                  // degrees to the left
                std::vector<Link> links;
                bool onSpot = false;
                // a wait at the point: links are its stop, then it stands with
                // the idle for the wait and sets off with the start
                bool halt = false;
                std::vector<Link> start;
                std::string idle;
                std::uint32_t wait = 0;
            };
            std::vector<Corner> corners;
            for (const auto& piece : path.pieces)
            {
                // (walking, it turns as it walks round them: the game's turns are for running; sprinting, the same, as
                // a turn would slow it down more than a run's and a faster gait arrive later; with gaits, a corner
                // walked or sprinted into or out of, or one it stands at for a while, the same; and those asked to)
                if (std::ranges::find(roundCorners, piece.point) != roundCorners.end())
                    continue;
                auto cornerState = walkState;
                if (const auto* leaving = ActorLogic::GetStretch(actor, piece.point))
                {
                    const auto* arriving = ActorLogic::GetStretch(actor, piece.point > 0 ? piece.point - 1 : 0);
                    if (leaving->gait != Types::ActorGait::Run || arriving->gait != Types::ActorGait::Run ||
                        leaving->wait > 0)
                        continue;
                    if (actor.stance == Types::ActorStance::Auto)
                        cornerState.posture = ToPosture(ActorLogic::GetStretchStance(*leaving));
                    cornerState.movement = ToMovement(ActorLogic::ToGait(leaving->gait));
                }
                else if (walkState.movement == Types::AnimationMovement::Walk ||
                         walkState.movement == Types::AnimationMovement::Sprint)
                    break;
                if (piece.radius <= 0.0f || piece.stop || piece.distance <= from || piece.distance >= to)
                    continue;
                const auto turnsLeft = piece.direction.x * piece.normal.y - piece.direction.y * piece.normal.x > 0.0f;
                const auto angle = glm::degrees(piece.length / piece.radius) * (turnsLeft ? 1.0f : -1.0f);
                if (std::abs(angle) <= TURN_THRESHOLD || piece.point >= actor.path.size())
                    continue;
                Corner corner{.point = piece.point,
                              .position = glm::vec2(actor.path[piece.point]),
                              .in = glm::vec2(piece.direction),
                              .angle = angle};
                const auto out = glm::radians(glm::degrees(std::atan2(piece.direction.y, piece.direction.x)) + angle);
                corner.out = glm::vec2(std::cos(out), std::sin(out));
                corner.links = JoinStates(cornerState, cornerState, body, animations, angle);
                if (corner.links.empty())
                {
                    corner.links = onSpot(cornerState.posture, angle);
                    corner.onSpot = true;
                }
                if (!corner.links.empty())
                    corners.push_back(std::move(corner));
            }
            // stopping: its stop, or a turn on the spot once it stood still
            std::vector<Link> stop;
            bool stopOnSpot = false;
            // (as it goes as it gets there: the last stretch's gait, with gaits)
            const auto arriving =
                actor.stretches.empty() ? walkState : GetClipState(actor, index, true, body, animations);
            if (IsKnown(after) && !IsMoving(after.movement) && IsMoving(arriving.movement))
            {
                stop = JoinStates(arriving, after, body, animations, stopTurn.value_or(0.0f));
                if (stop.empty() && stopTurn && std::abs(*stopTurn) > TURN_THRESHOLD)
                {
                    stop = onSpot(after.posture, *stopTurn);
                    stopOnSpot = true;
                }
            }
            // the waits at its points: stopping there, standing as long, setting off again (each in the path's order
            // among the corners)
            if (!actor.stretches.empty())
                for (std::size_t point = 1; point + 1 < actor.path.size(); ++point)
                {
                    const auto* leaving = ActorLogic::GetStretch(actor, point);
                    const auto* arrivingAt = ActorLogic::GetStretch(actor, point - 1);
                    const auto distance = ActorLogic::GetPointDistance(path, point);
                    if (!leaving || !arrivingAt || leaving->wait == 0 || !distance || *distance <= from ||
                        *distance >= to)
                        continue;
                    auto moving = walkState, standing = walkState;
                    moving.movement = ToMovement(ActorLogic::ToGait(arrivingAt->gait));
                    if (actor.stance == Types::ActorStance::Auto)
                        moving.posture = standing.posture = ToPosture(ActorLogic::GetStretchStance(*arrivingAt));
                    standing.movement = Movement::Stop;
                    auto leavingState = moving;
                    leavingState.movement = ToMovement(ActorLogic::ToGait(leaving->gait));
                    const auto in = glm::normalize(glm::vec2(actor.path[point] - actor.path[point - 1]));
                    const auto out = glm::normalize(glm::vec2(actor.path[point + 1] - actor.path[point]));
                    const auto angle = glm::degrees(std::atan2(in.x * out.y - in.y * out.x, glm::dot(in, out)));
                    Corner halt{
                        .point = point, .position = glm::vec2(actor.path[point]), .in = in, .out = out, .angle = angle};
                    halt.links = JoinStates(moving, standing, body, animations, 0.0f);
                    halt.start = JoinStates(standing, leavingState, body, animations, angle);
                    halt.idle = ActorLogic::FindLocomotionAnimation(
                        body, ActorLogic::GetStretchStance(*arrivingAt), ActorLogic::Gait::Idle,
                        ActorLogic::Heading::Forward,
                        [&](std::string_view name) { return animations(std::string(name)).has_value(); });
                    halt.wait = leaving->wait;
                    halt.halt = true;
                    // (a body without stops and starts waits as before, standing at its walk's idle)
                    if (halt.links.empty() || halt.idle.empty())
                        continue;
                    const auto before =
                        std::ranges::find_if(corners, [&](const Corner& corner) { return corner.point > point; });
                    corners.insert(before, std::move(halt));
                }
            if (start.empty() && corners.empty() && stop.empty())
                return plan;

            // the walk's clips, from its start on: the start's, then a stretch of the walk to each turn and after it,
            // then the stop's; the clips after it (left out while the walk's are fitted, as they start later) back after
            // them, from where they hand over
            auto planned = actor;
            auto& clips = planned.clips;
            std::vector<Types::ActorClip> later(clips.begin() + static_cast<std::ptrdiff_t>(index + 1), clips.end());
            clips.erase(clips.begin() + static_cast<std::ptrdiff_t>(index), clips.end());
            std::uint32_t handOver = walk.blendIn;
            auto t = walk.start;
            if (!start.empty())
                t = AddLinks(clips, clips.size(), start, t, Types::ActorChain::Start, std::nullopt, animations,
                             &handOver);
            auto stretch = walk;
            stretch.start = t;
            stretch.length = std::max(100u, walk.End() > t ? walk.End() - t : 0u);
            stretch.blendIn = handOver;
            clips.push_back(stretch);
            // (with gaits, the slowest stretch's, and its waits added: for how long to look for where a turn starts)
            auto speed = std::max(ActorLogic::GetClipWalk(planned, clips.back(), animations).speed, 1.0f);
            std::uint32_t waits = 0;
            for (const auto& each : actor.stretches)
            {
                speed = std::min(speed, std::max(ActorLogic::GetStretchSpeed(actor, each, animations), 1.0f));
                waits += each.wait;
            }
            const auto position = [&](std::uint32_t tick) {
                return glm::vec2(ActorLogic::EvaluatePlacement(planned, tick, animations).position);
            };
            // how long it takes to walk somewhere (and a little longer)
            const auto timeTo = [&](glm::vec2 a, glm::vec2 b, std::uint32_t more) {
                return static_cast<std::uint32_t>(glm::distance(a, b) / speed * 1000.0f) + more;
            };

            for (const auto& corner : corners)
            {
                // a stretch to the turn (the last clip), the turn, a stretch on (long, for now)
                const auto stretchAt = clips.size() - 1;
                const auto stretchStart = clips[stretchAt].start;
                if (corner.halt)
                {
                    // a stretch to the point, the stop (landing at it), the idle for the wait, the start, the walk on;
                    // returns when the stop's move ends
                    const auto applyHalt = [&](std::uint32_t stopStart) {
                        clips.resize(stretchAt + 1);
                        clips[stretchAt].length = std::max(1u, stopStart - stretchStart);
                        std::uint32_t over = 0;
                        const auto standing = AddLinks(clips, clips.size(), corner.links, stopStart,
                                                       Types::ActorChain::Stop, corner.point, animations, &over);
                        const auto moved = GetMotionEnd(clips.back(), animations);
                        clips.push_back({.animation = corner.idle,
                                         .start = standing,
                                         .length = std::max(corner.wait, 1u),
                                         .blendIn = over,
                                         .moves = false,
                                         .chain = Types::ActorChain::Stop,
                                         .corner = corner.point});
                        std::uint32_t off = 200;
                        const auto onward = AddLinks(clips, clips.size(), corner.start, clips.back().End(),
                                                     Types::ActorChain::Start, std::nullopt, animations, &off);
                        auto rest = walk;
                        rest.id = 0;
                        rest.chain = Types::ActorChain::Walk;
                        rest.start = onward;
                        rest.length = 600000;
                        rest.blendIn = GetJoinBlendIn(actor, corner.point, off);
                        clips.push_back(rest);
                        return moved;
                    };
                    // (landing on the point: as far along the path, which its stop has sharp there, as the point is;
                    // past a right angle a projection on the way in would stay at it)
                    const auto low = stretchStart + 100u,
                               high = stretchStart + timeTo(position(stretchStart), corner.position, 3000u);
                    const auto solved = Solve(low, high, [&](std::uint32_t stopStart) {
                        const auto landed = applyHalt(stopStart) + 1;
                        const auto point =
                            ActorLogic::GetPointDistance(ActorLogic::GetRoundedPath(planned, animations), corner.point);
                        return ActorLogic::GetPathProgress(planned, landed, animations) - point.value_or(0.0f);
                    });
                    if (!solved)
                    {
                        // (no room: it waits standing on its walk, as before)
                        clips.resize(stretchAt + 1);
                        clips[stretchAt].length = 600000;
                        continue;
                    }
                    applyHalt(*solved);
                    continue;
                }
                const auto apply = [&](std::uint32_t turnStart) {
                    clips.resize(stretchAt + 1);
                    clips[stretchAt].length = std::max(1u, turnStart - stretchStart);
                    std::uint32_t over = 0;
                    const auto onward = AddLinks(clips, clips.size(), corner.links, turnStart,
                                                 Types::ActorChain::Turn, corner.point, animations, &over);
                    auto rest = walk;
                    rest.id = 0;
                    rest.chain = Types::ActorChain::Walk;
                    rest.start = onward;
                    rest.length = 600000;
                    rest.blendIn = GetJoinBlendIn(actor, corner.point, over);
                    clips.push_back(rest);
                    return onward;
                };
                // (starting before the corner: past it the walk goes on along the stretch after it)
                const auto low = stretchStart + 100u;
                const auto high =
                    stretchStart + static_cast<std::uint32_t>(
                                       std::max(glm::distance(position(stretchStart), corner.position) - 5.0f, 0.0f) /
                                       speed * 1000.0f);
                std::optional<std::uint32_t> solved;
                if (corner.onSpot)
                {
                    // stopping just before the corner as the turn has blended in
                    const auto blend = corner.links.front().blends.in;
                    solved = Solve(low, high + blend, [&](std::uint32_t turnStart) {
                        apply(turnStart);
                        return glm::dot(corner.in, position(turnStart + blend) - corner.position) + 0.5f;
                    });
                }
                else
                {
                    // landing on the stretch after the corner (at its point, turning round), as the walk takes over
                    const bool across = std::abs(corner.in.x * corner.out.y - corner.in.y * corner.out.x) >= 0.5f;
                    solved = Solve(low, high, [&](std::uint32_t turnStart) {
                        const auto onward = apply(turnStart);
                        const auto landed = position(onward - 1) - corner.position;
                        return across ? corner.out.x * landed.y - corner.out.y * landed.x
                                      : glm::dot(corner.in, landed);
                    });
                }
                if (!solved)
                {
                    // (no room: the corner is walked round)
                    clips.resize(stretchAt + 1);
                    clips[stretchAt].length = 600000;
                    continue;
                }
                apply(*solved);
                plan.turned.push_back(corner.point);
            }

            // the last stretch to the stop (or to the walk's end), the stop, and the clip after it from where it hands
            // over
            const auto at = clips.size() - 1;
            const auto stretchStart = clips[at].start;
            const auto low = stretchStart + 100u,
                       high = stretchStart + timeTo(position(stretchStart), endPoint, 3000u + waits);
            // the clips up to the stretch's end there, and the stop's after it; and the clip after the walk from where
            // that hands over (for a while, to fit with)
            const auto fitTo = [&](std::uint32_t stretchEnd, bool withStop, bool withAfter) {
                clips.resize(at + 1);
                clips[at].length = std::max(1u, stretchEnd - stretchStart);
                auto onward = stretchEnd;
                std::uint32_t over = 0;
                if (withStop)
                    onward = AddLinks(clips, clips.size(), stop, stretchEnd, Types::ActorChain::Stop, std::nullopt,
                                      animations, &over);
                if (withAfter && !later.empty())
                {
                    auto first = later.front();
                    first.length = first.End() > onward ? first.End() - onward : first.length;
                    first.start = onward;
                    if (withStop)
                        first.blendIn = over;
                    clips.push_back(first);
                }
                return std::pair(onward, over);
            };
            std::optional<std::uint32_t> solved;
            // (a stop it has no room for is left out: it gets there without it, running into its idle)
            const bool stops = !stop.empty();
            if (!stop.empty() && !stopOnSpot)
            {
                // the stop's move ends at the walk's end
                solved = Solve(low, high, [&](std::uint32_t stretchEnd) {
                    fitTo(stretchEnd, true, true);
                    const auto landed = position(GetMotionEnd(clips[at + stop.size()], animations) + 1);
                    return glm::dot(endWay, landed - endPoint);
                });
                if (!solved)
                    stop.clear();
            }
            if (!solved)
            {
                // the walk to its end, as the clip after it takes over (a turn on the spot after it, if any)
                const auto blend = !stop.empty() ? stop.front().blends.in : !later.empty() ? later.front().blendIn : 0u;
                solved = Solve(low, high, [&](std::uint32_t stretchEnd) {
                    fitTo(stretchEnd, false, false);
                    return glm::dot(endWay, position(stretchEnd + blend) - endPoint);
                });
                if (!solved)
                    solved = stretchStart + std::max(100u, walk.End() > stretchStart ? walk.End() - stretchStart : 0u);
            }
            const auto [onward, over] = fitTo(*solved, !stop.empty(), false);
            // when it has got as far as it goes: its stop's move over, else its walk's as the clip after takes over
            const auto finish = !stop.empty() && !stopOnSpot ? GetMotionEnd(clips[at + stop.size()], animations)
                                                             : *solved + (!stop.empty()    ? stop.front().blends.in
                                                                          : !later.empty() ? later.front().blendIn
                                                                                           : 0u);
            // the clips after it from there (one that loops keeping its end), moved on with the first
            if (!later.empty())
            {
                const auto afterIndex = clips.size();
                clips.insert(clips.end(), later.begin(), later.end());
                Shift(planned, afterIndex, static_cast<std::int64_t>(onward) - clips[afterIndex].start);
                if (!stop.empty())
                    clips[afterIndex].blendIn = over;
            }
            ActorLogic::Normalize(planned);
            // (a walk longer than the path gets no further than its end)
            plan.arrives = stops == !stop.empty() && ActorLogic::GetPathProgress(planned, finish + 1, animations) >=
                                                         std::min(to, path.length) - ARRIVED;
            actor = std::move(planned);
            return plan;
        }

        // Plans one walk (see PlanOneWalk); where its turns leave it no room to get to the walk's end (a turn close to
        // the end lands it too far on for its stop), plans it again with the corner turned last walked round, and so
        // on: a few times at most
        void PlanArriving(Types::Actor& actor, std::uint32_t walkId, std::optional<float> stopTurn,
                          const ActorLogic::Body& body, const ActorLogic::AnimationLookup& animations)
        {
            constexpr int PASSES = 3;
            std::vector<std::size_t> round;
            for (int pass = 0; pass < PASSES; ++pass)
            {
                auto planned = actor;
                const auto plan = PlanOneWalk(planned, walkId, stopTurn, body, animations, round);
                if (plan.arrives || plan.turned.empty() || pass + 1 == PASSES)
                {
                    actor = std::move(planned);
                    return;
                }
                round.push_back(plan.turned.back());
            }
        }
    }  // namespace

    namespace
    {
        // Takes a path's transitions out of the clips from one index to another (excluded), putting their walks back
        // together: a start's walk starting where it started, a turn's stretch after it the walk's before it, the clip
        // after a stop starting where it started
        bool Unplan(Types::Actor& actor, std::size_t first, std::size_t last)
        {
            std::vector<Types::ActorClip> kept(actor.clips.begin(),
                                               actor.clips.begin() + static_cast<std::ptrdiff_t>(first));
            std::optional<std::uint32_t> startsAt;  // where the clip after a start or stop taken out starts
            bool changed = false;
            for (std::size_t k = first; k < actor.clips.size(); ++k)
            {
                auto clip = actor.clips[k];
                if (k < last)
                    switch (clip.chain)
                    {
                        case Types::ActorChain::Start:
                        case Types::ActorChain::Stop:
                            if (!startsAt)
                                startsAt = clip.start;
                            changed = true;
                            continue;
                        case Types::ActorChain::Turn:
                            changed = true;
                            continue;
                        case Types::ActorChain::Walk:
                            if (!kept.empty())
                                kept.back().length = std::max(kept.back().length, clip.End() - kept.back().start);
                            // (a wait's stop and start before it: the walk goes on, not the clip after it)
                            startsAt.reset();
                            changed = true;
                            continue;
                        default:
                            break;
                    }
                if (startsAt && *startsAt < clip.start)
                {
                    clip.length += clip.start - *startsAt;
                    clip.start = *startsAt;
                }
                startsAt.reset();
                kept.push_back(std::move(clip));
            }
            if (changed)
            {
                actor.clips = std::move(kept);
                ActorLogic::Normalize(actor);
            }
            return changed;
        }
    }  // namespace

    namespace
    {
        // the clips of a walk's plan, from the index of its start's first to after its stop's last (the walk's own
        // without them)
        std::optional<std::pair<std::size_t, std::size_t>> FindWalkPlan(const Types::Actor& actor, std::uint32_t walkId)
        {
            const auto index = FindClip(actor, walkId);
            if (!index)
                return std::nullopt;
            auto first = *index, last = *index + 1;
            while (first > 0 && actor.clips[first - 1].chain == Types::ActorChain::Start)
                --first;
            // (a wait's start among them, after its stop and idle)
            while (last < actor.clips.size() && (actor.clips[last].chain == Types::ActorChain::Turn ||
                                                 actor.clips[last].chain == Types::ActorChain::Walk ||
                                                 actor.clips[last].chain == Types::ActorChain::Stop ||
                                                 actor.clips[last].chain == Types::ActorChain::Start))
                ++last;
            return std::pair(first, last);
        }
    }  // namespace

    bool UnplanWalk(Types::Actor& actor, std::uint32_t walkId)
    {
        // its start before it, its turns, stretches and stop after it
        const auto plan = FindWalkPlan(actor, walkId);
        return plan && Unplan(actor, plan->first, plan->second);
    }

    std::optional<float> GetStopTurn(const Types::Actor& actor, std::uint32_t walkId)
    {
        std::optional<float> turn;
        if (const auto index = FindClip(actor, walkId))
            for (auto k = *index + 1; k < actor.clips.size() && actor.clips[k].chain != Types::ActorChain::None; ++k)
                if (actor.clips[k].chain == Types::ActorChain::Stop && actor.clips[k].turn)
                    turn = actor.clips[k].turn;
        return turn;
    }

    bool PlanWalk(Types::Actor& actor, std::uint32_t walkId, const ActorLogic::Body& body,
                  const ActorLogic::AnimationLookup& animations, std::optional<float> stopTurn)
    {
        // (as its stop turned, unless given)
        if (!stopTurn)
            stopTurn = GetStopTurn(actor, walkId);
        const auto before = actor.clips;
        UnplanWalk(actor, walkId);
        if (actor.autoTransitions && actor.locomotion == Types::ActorLocomotion::Auto &&
            actor.movement == Types::ActorMovement::Path && actor.path.size() >= 2)
            PlanArriving(actor, walkId, stopTurn, body, animations);
        return actor.clips != before;
    }

    std::vector<std::uint32_t> GetPathWalks(const Types::Actor& actor, const ActorLogic::AnimationLookup& animations)
    {
        std::vector<std::uint32_t> walks;
        if (actor.movement != Types::ActorMovement::Path || actor.locomotion != Types::ActorLocomotion::Auto)
            return walks;
        for (const auto& clip : actor.clips)
        {
            if (clip.chain != Types::ActorChain::None || !ActorLogic::FindLocomotion(actor, clip.animation))
                continue;
            // (forward, or a few degrees off as some walk: the players' sprint goes 3 degrees to the right)
            const auto walk = ActorLogic::GetClipWalk(actor, clip, animations);
            if (walk.angle && std::abs(*walk.angle) < MAX_WALK_ANGLE && walk.speed >= ActorLogic::IDLE_SPEED)
                walks.push_back(clip.id);
        }
        return walks;
    }

    std::pair<std::uint32_t, std::uint32_t> GetWalkSpan(const Types::Actor& actor, std::uint32_t walkId)
    {
        const auto plan = FindWalkPlan(actor, walkId);
        if (!plan)
            return {0u, 0u};
        std::uint32_t end = 0;
        for (auto k = plan->first; k < plan->second; ++k)
            end = std::max(end, actor.clips[k].End());
        return {actor.clips[plan->first].start, end};
    }

    bool IsPlanned(const Types::Actor& actor, std::uint32_t walkId)
    {
        const auto plan = FindWalkPlan(actor, walkId);
        return plan && plan->second - plan->first > 1;
    }

    bool UnplanPath(Types::Actor& actor)
    {
        return Unplan(actor, 0, actor.clips.size());
    }

    bool PlanPath(Types::Actor& actor, const ActorLogic::Body& body, const ActorLogic::AnimationLookup& animations,
                  std::optional<std::pair<std::uint32_t, float>> stopTurn)
    {
        // the turns its stops were to make, kept as it is planned again: by the walk they end
        std::map<std::uint32_t, float> stopTurns;
        std::optional<std::uint32_t> lastWalk;
        for (const auto& clip : actor.clips)
        {
            if (clip.chain == Types::ActorChain::None && ActorLogic::FindLocomotion(actor, clip.animation))
                lastWalk = clip.id;
            if (clip.chain == Types::ActorChain::Stop && clip.turn && lastWalk)
                stopTurns[*lastWalk] = *clip.turn;
        }
        if (stopTurn)
            stopTurns[stopTurn->first] = stopTurn->second;

        const auto before = actor.clips;
        UnplanPath(actor);
        if (!actor.autoTransitions || actor.locomotion != Types::ActorLocomotion::Auto ||
            actor.movement != Types::ActorMovement::Path || actor.path.size() < 2)
            return actor.clips != before;
        // its walks along the path, forward
        for (const auto id : GetPathWalks(actor, animations))
        {
            const auto turn = stopTurns.find(id);
            PlanArriving(actor, id, turn != stopTurns.end() ? std::optional(turn->second) : std::nullopt, body,
                         animations);
        }
        return actor.clips != before;
    }

    ActorLogic::RootMotion GetTurnMotion(const ActorLogic::RootMotion* motion,
                                         std::span<const Types::ActorNotetrack> notetracks, float turn, bool start)
    {
        // the animation's own (a still one without)
        std::vector<glm::vec3> own;
        if (motion && motion->samples.size() >= 2)
            own = motion->samples;
        else
            own.assign(33, glm::vec3(0.0f));
        const auto count = own.size();
        const auto timeOf = [&](std::size_t i) { return static_cast<float>(i) / static_cast<float>(count - 1); };
        const auto notetrack = [&](std::string_view name) -> std::optional<float> {
            const auto found = std::ranges::find(notetracks, name, &Types::ActorNotetrack::name);
            return found != notetracks.end() ? std::optional(found->time) : std::nullopt;
        };
        const auto ramp = [](float t, float from, float to) {
            return to > from ? std::clamp((t - from) / (to - from), 0.0f, 1.0f) : (t >= to ? 1.0f : 0.0f);
        };

        // the way it faces at a sample, from the way it started facing (one that doesn't turn as a whole is turned in
        // code: the players' turns, whose delta rotation the game leaves out, even where it wobbles)
        std::vector<float> facing(count);
        const auto exitAlign = notetrack("exit_align");
        const auto total = own.back().z - own.front().z;
        const bool turns = std::abs(total) >= 1.0f;
        if (!turns)
            for (auto& sample : own)
                sample.z = own.front().z;
        if (exitAlign && !start)
        {
            // as turn.gsc turns its AI: held, then at a steady rate from exit_align to code_move (or the end)
            const auto codeMove = notetrack("code_move");
            const auto until = codeMove && *codeMove > *exitAlign ? *codeMove : 1.0f;
            for (std::size_t i = 0; i < count; ++i)
                facing[i] = turn * ramp(timeOf(i), *exitAlign, until);
        }
        else if (turns)
        {
            // its own turn, and the rest over the part of it that turns
            std::size_t first = 0, last = count - 1;
            while (first + 1 < count && std::abs(own[first + 1].z - own.front().z) < 0.01f)
                ++first;
            while (last > first && std::abs(own.back().z - own[last - 1].z) < 0.01f)
                --last;
            for (std::size_t i = 0; i < count; ++i)
                facing[i] = own[i].z - own.front().z + (turn - total) * ramp(timeOf(i), timeOf(first), timeOf(last));
        }
        else
        {
            // turned in code as it plays, up to loop_begin (MP's turns) or its end
            const auto loopBegin = notetrack("loop_begin");
            const auto until = loopBegin && *loopBegin > 0.0f ? *loopBegin : 1.0f;
            for (std::size_t i = 0; i < count; ++i)
                facing[i] = turn * ramp(timeOf(i), 0.0f, until);
        }

        // the root goes as far as the animation moves it each step, the way the actor faces against the way the
        // animation's root faces then
        ActorLogic::RootMotion turned;
        turned.samples.reserve(count);
        glm::dvec2 position(0.0);
        turned.samples.emplace_back(0.0f, 0.0f, facing.front());
        for (std::size_t i = 1; i < count; ++i)
        {
            const glm::dvec2 step(own[i].x - own[i - 1].x, own[i].y - own[i - 1].y);
            const auto angle = glm::radians((static_cast<double>(facing[i]) + facing[i - 1]) / 2.0 -
                                            (static_cast<double>(own[i].z) + own[i - 1].z) / 2.0 + own.front().z);
            position += glm::dvec2(step.x * std::cos(angle) - step.y * std::sin(angle),
                                   step.x * std::sin(angle) + step.y * std::cos(angle));
            turned.samples.emplace_back(static_cast<float>(position.x), static_cast<float>(position.y), facing[i]);
        }
        return turned;
    }
}  // namespace IWXMVM::TransitionLogic
