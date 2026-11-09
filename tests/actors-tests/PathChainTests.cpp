#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/AnimationCatalogLogic.hpp"
#include "Components/TransitionLogic.hpp"

using namespace IWXMVM;

// Paths that set off, turn at their corners and stop with the game's transitions (TransitionLogic::PlanPath;
// step 8), with the movement sets' walks and runs (Locomotion Auto)

namespace
{
    using Set = Types::ActorMovementSet;
    using Gait = ActorLogic::Gait;
    using Heading = ActorLogic::Heading;
    using Stance = Types::ActorStance;
    using State = Types::AnimationState;

    const std::filesystem::path DATA = std::filesystem::path(__FILE__).parent_path() / "data";
    const std::filesystem::path SHIPPED_CATALOG =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" / "resources" /
        "animations.json";

    const Types::AnimationCatalog& Shipped()
    {
        static const auto catalog = [] {
            std::ifstream file(SHIPPED_CATALOG);
            return AnimationCatalogLogic::Parse(nlohmann::json::parse(file));
        }();
        return catalog;
    }

    // the root motion of some transitions and cycles as the game measures them (decoded from the animation pack's
    // delta parts at each frame)
    const std::map<std::string, std::shared_ptr<const ActorLogic::RootMotion>, std::less<>>& Tracks()
    {
        static const auto tracks = [] {
            std::map<std::string, std::shared_ptr<const ActorLogic::RootMotion>, std::less<>> read;
            std::ifstream file(DATA / "transition-tracks.json");
            const auto json = nlohmann::json::parse(file);
            for (const auto& [name, samples] : json.items())
            {
                auto motion = std::make_shared<ActorLogic::RootMotion>();
                for (const auto& sample : samples)
                    motion->samples.emplace_back(sample[0].get<float>(), sample[1].get<float>(), sample[2].get<float>());
                read[name] = std::move(motion);
            }
            return read;
        }();
        return tracks;
    }

    // what the game knows once it has built the animations: the catalog's numbers and states, the motion of a play
    ActorLogic::AnimationLookup Animations()
    {
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            const auto entry = Shipped().Find(name);
            if (!entry)
                return std::nullopt;
            ActorLogic::AnimationInfo info{entry->Duration(), glm::vec2(entry->motion), entry->notetracks, entry->kind,
                                           entry->family};
            info.death = entry->death;
            info.alert = entry->alert;
            info.stance = AnimationCatalogLogic::GetStance(entry->from.posture);
            info.delta = entry->delta;
            info.loop = entry->loop;
            if (entry->from != State{})
                info.from = entry->from;
            if (entry->to != State{})
                info.to = entry->to;
            if (entry->turn != 0.0f)
                info.turn = entry->turn;
            if (const auto track = Tracks().find(name); track != Tracks().end() && track->second->samples.size() >= 2)
                info.rootMotion = track->second;
            return info;
        };
    }

    ActorLogic::Body Person(Set set)
    {
        ActorLogic::Body body;
        body.set = set;
        return body;
    }

    std::string Chain(const Types::Actor& actor)
    {
        std::string text;
        for (const auto& clip : actor.clips)
            text += std::format("{}{}{}", text.empty() ? "" : " ", clip.chain != Types::ActorChain::None ? "+" : "",
                                clip.animation);
        return text;
    }

    // An actor standing, then walking a path (with Locomotion Auto, the walk fitted to it), then standing again, of a
    // movement set, with Auto transitions
    Types::Actor Walker(std::vector<glm::vec3> path, Set set, float speed, std::string idle, std::string walk,
                        float yaw = 0.0f)
    {
        Types::Actor actor;
        actor.id = 1;
        actor.movement = Types::ActorMovement::Path;
        actor.path = std::move(path);
        actor.locomotion = Types::ActorLocomotion::Auto;
        actor.pathSpeed = speed;
        actor.movementSet = set;
        actor.autoTransitions = true;
        actor.yaw = yaw;
        actor.clips = {{.animation = idle, .start = 0, .length = 1000, .moves = false},
                       {.animation = walk, .start = 1000, .length = 1000},
                       {.animation = idle, .start = 2000, .length = 30000, .moves = false}};
        ActorLogic::Normalize(actor);
        ActorLogic::FitWalk(actor, actor.clips[1].id, Animations(), speed);
        return actor;
    }

    const Types::ActorClip* FindChain(const Types::Actor& actor, Types::ActorChain chain)
    {
        const auto found = std::ranges::find(actor.clips, chain, &Types::ActorClip::chain);
        return found != actor.clips.end() ? &*found : nullptr;
    }

    // the largest step of the actor's position and yaw from one millisecond to the next between two ticks
    std::pair<float, float> LargestSteps(const Types::Actor& actor, std::uint32_t from, std::uint32_t to)
    {
        float position = 0.0f, yaw = 0.0f;
        auto last = ActorLogic::EvaluatePlacement(actor, from, Animations());
        for (auto tick = from + 1; tick <= to; ++tick)
        {
            const auto now = ActorLogic::EvaluatePlacement(actor, tick, Animations());
            position = std::max(position, glm::distance(glm::vec2(now.position), glm::vec2(last.position)));
            yaw = std::max(yaw, std::abs(std::remainder(now.yaw - last.yaw, 360.0f)));
            last = now;
        }
        return {position, yaw};
    }

    // when one of an actor's clips' animation passes a notetrack (its start without it)
    std::uint32_t TickOf(const Types::ActorClip& clip, std::string_view notetrack)
    {
        const auto entry = Shipped().Find(clip.animation);
        const auto found = std::ranges::find(entry->notetracks, notetrack, &Types::ActorNotetrack::name);
        const auto time = found != entry->notetracks.end() ? found->time : 0.0f;
        return clip.start + static_cast<std::uint32_t>(std::lround(time * entry->Duration() * 1000.0f));
    }
}  // namespace

TEST("path chains: the AI's sets walk and run with their own, which stand for the locomotion of actors of those sets")
{
    const auto lowReady = Person(Set::LowReady), cqb = Person(Set::Cqb), runAndGun = Person(Set::RunAndGun);
    CHECK(ActorLogic::GetLocomotionAnimation(lowReady, Stance::Stand, Gait::Run, Heading::Forward) == "run_lowready_f");
    CHECK(ActorLogic::GetLocomotionAnimation(lowReady, Stance::Stand, Gait::Walk, Heading::Left) == "walk_lowready_l");
    CHECK(ActorLogic::GetLocomotionAnimation(lowReady, Stance::Stand, Gait::Idle, Heading::Forward) ==
          "casual_stand_idle");
    CHECK(ActorLogic::GetLocomotionAnimation(lowReady, Stance::Crouch, Gait::Run, Heading::Forward) ==
          "crouch_fastwalk_f");
    CHECK(ActorLogic::GetLocomotionAnimation(cqb, Stance::Stand, Gait::Walk, Heading::Forward) == "walk_cqb_f");
    CHECK(ActorLogic::GetLocomotionAnimation(runAndGun, Stance::Stand, Gait::Run, Heading::Forward) == "run_n_gun_f");
    // as fast as the game's (the catalog's root motion), the run picked at its speed
    const auto choice = ActorLogic::ChooseLocomotion(lowReady, Stance::Stand, 182.0f, 0.0f, Animations());
    CHECK(choice.animation == "run_lowready_f" && std::abs(choice.rate - 1.0f) < 0.01f);
    CHECK(ActorLogic::ChooseLocomotion(lowReady, Stance::Stand, 91.0f, 0.0f, Animations()).animation ==
          "walk_lowready_f");
    const auto presets = ActorLogic::GetSpeedPresets(lowReady);
    CHECK(presets.size() == 4 && presets[1].name == "Run" && std::abs(presets[1].speed - 182.0f) < 0.5f);
    // a clip of the players' run stands for the AI's run of an actor of the AI's set (walking a path)
    Types::Actor actor;
    actor.movementSet = Set::LowReady;
    actor.movement = Types::ActorMovement::Path;
    actor.path = {{0, 0, 0}, {1000, 0, 0}};
    const auto run = ActorLogic::FindLocomotion(actor, "pb_combatrun_forward_loop");
    CHECK(run && run->gait == Gait::Run);
    const auto library = ActorLogic::GetLegsLibrary(
        [&] {
            auto mover = actor;
            mover.locomotion = Types::ActorLocomotion::Auto;
            mover.clips = {{.animation = "run_lowready_f", .start = 0, .length = 1000}};
            return mover;
        }(),
        lowReady, Animations());
    CHECK(std::ranges::find(library, "run_lowready_f") != library.end());
    CHECK(std::ranges::find(library, "casual_stand_idle") != library.end());
    CHECK(std::ranges::find(library, "pb_combatrun_forward_loop") == library.end());
}

TEST("path chains: an AI runs off, turns at a right angle corner with its running turn and stops at the end")
{
    auto actor = Walker({glm::vec3(0, 0, 0), glm::vec3(800, 0, 0), glm::vec3(800, 800, 0)}, Set::LowReady, 182.0f,
                        "casual_stand_idle", "run_lowready_f");
    CHECK(TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations()));
    CHECK(Chain(actor) == "casual_stand_idle +stand_2_run_f_2 run_lowready_f +ai_run_lowready_f_turn_90_l "
                          "+run_lowready_f +run_2_stand_f_6 casual_stand_idle");
    const auto turn = FindChain(actor, Types::ActorChain::Turn);
    const auto stop = FindChain(actor, Types::ActorChain::Stop);
    CHECK(turn && stop && turn->corner == 1u && turn->turn && std::abs(*turn->turn - 90.0f) < 0.01f);
    if (!turn || !stop)
        return;
    // the run after the turn goes on along the stretch after the corner, facing it
    const auto& after = *std::next(std::ranges::find(actor.clips, turn->id, &Types::ActorClip::id));
    const auto landed = ActorLogic::EvaluatePlacement(actor, after.start - 1, Animations());
    CHECK_NEAR(landed.position.x, 800.0f, 1.0f);
    const auto running = ActorLogic::EvaluatePlacement(actor, after.start + 500, Animations());
    CHECK_NEAR(running.position.x, 800.0f, 1.0f);
    CHECK_NEAR(running.yaw, 90.0f, 1.0f);
    // the turn as the game turns its AI: facing ahead until exit_align, 90 degrees more by its end
    const auto aligned = ActorLogic::EvaluatePlacement(actor, TickOf(*turn, "exit_align"), Animations());
    const auto turned = ActorLogic::EvaluatePlacement(actor, TickOf(*turn, "end"), Animations());
    CHECK_NEAR(aligned.yaw, 0.0f, 1.0f);
    CHECK_NEAR(std::remainder(turned.yaw - aligned.yaw, 360.0f), 90.0f, 5.0f);
    // the stop ends at the path's end, facing the last stretch's way
    const auto stopped = ActorLogic::EvaluatePlacement(actor, TickOf(*stop, "end") + 50, Animations());
    CHECK(glm::distance(glm::vec2(stopped.position), glm::vec2(800, 800)) < 1.0f);
    CHECK_NEAR(stopped.yaw, 90.0f, 1.0f);
    CHECK(glm::distance(glm::vec2(ActorLogic::EvaluatePlacement(actor, 30000, Animations()).position),
                        glm::vec2(800, 800)) < 1.0f);
    // without jumps: its position moves at most as far as a run in a millisecond (and a little), its yaw at most a
    // degree a millisecond
    const auto [position, yaw] = LargestSteps(actor, 900, stop->End() + 500);
    CHECK(position < 0.4f);
    CHECK(yaw < 1.0f);
    // taken out again: as before
    auto plain = Walker({glm::vec3(0, 0, 0), glm::vec3(800, 0, 0), glm::vec3(800, 800, 0)}, Set::LowReady, 182.0f,
                        "casual_stand_idle", "run_lowready_f");
    CHECK(TransitionLogic::UnplanPath(actor));
    CHECK(Chain(actor) == Chain(plain));
    // and planned again, the same
    auto again = actor;
    TransitionLogic::PlanPath(again, Person(Set::LowReady), Animations());
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations());
    CHECK(again.clips == actor.clips);
}

TEST("path chains: setting off, the actor turns as its start does, never on the spot")
{
    // the user's actor (2026-10-28): added facing the camera (180), then run along points heading 123 degrees; it sets
    // off with stand_2_run_r (90 to the right) asked to turn 57, which it held until exit_align and then turned in the
    // 64 ms to code_move. It turns as the start's root does, the 33 degrees it turns too far taken off over its turn
    const auto heading = glm::radians(123.0f);
    auto actor = Walker({glm::vec3(0, 0, 0), glm::vec3(600 * std::cos(heading), 600 * std::sin(heading), 0)},
                        Set::LowReady, 182.0f, "casual_stand_idle", "run_lowready_f", 180.0f);
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations(), std::pair(actor.clips[1].id, 90.0f));
    const auto start = FindChain(actor, Types::ActorChain::Start);
    CHECK(start && start->animation == "stand_2_run_r");
    if (!start)
        return;
    CHECK_NEAR(std::abs(ActorLogic::EvaluatePlacement(actor, start->start, Animations()).yaw), 180.0f, 0.5f);
    // halfway to exit_align it has turned as much of its turn as its animation has by then
    const auto halfway = (start->start + TickOf(*start, "exit_align")) / 2;
    CHECK(std::remainder(ActorLogic::EvaluatePlacement(actor, halfway, Animations()).yaw - 180.0f, 360.0f) < -10.0f);
    // at most a third of a degree a millisecond (it turned 0.86 between exit_align and code_move)
    const auto [position, yaw] = LargestSteps(actor, start->start - 200, start->End() + 1000);
    CHECK(yaw < 0.35f);
    CHECK(position < 0.4f);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, start->End() + 1500, Animations()).yaw, 123.0f, 1.0f);
}

TEST("path chains: setting off turned, the start turns the actor from the way it faced; a stop turns as asked")
{
    // facing back (west) with the path going east: it turns round as it sets off
    auto actor = Walker({glm::vec3(0, 0, 0), glm::vec3(800, 0, 0)}, Set::LowReady, 182.0f, "casual_stand_idle",
                        "run_lowready_f", 180.0f);
    const auto walk = actor.clips[1].id;
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations(), std::pair(walk, 90.0f));
    CHECK(Chain(actor) == "casual_stand_idle +stand_2_run_180_med run_lowready_f +run_2_stand_90l casual_stand_idle");
    // standing, it faces as it did; after the start, along the path; stopped, 90 more
    CHECK_NEAR(std::abs(ActorLogic::EvaluatePlacement(actor, 500, Animations()).yaw), 180.0f, 1.0f);
    const auto start = FindChain(actor, Types::ActorChain::Start);
    const auto stop = FindChain(actor, Types::ActorChain::Stop);
    if (!start || !stop)
        return;
    // (where it lands to the side of the path, it goes back onto it gently, then along it)
    CHECK(std::abs(ActorLogic::EvaluatePlacement(actor, start->End() + 300, Animations()).yaw) < 15.0f);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, start->End() + 1500, Animations()).yaw, 0.0f, 1.0f);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, start->End() + 1500, Animations()).position.y, 0.0f, 0.5f);
    const auto stopped = ActorLogic::EvaluatePlacement(actor, TickOf(*stop, "end") + 50, Animations());
    CHECK_NEAR(stopped.yaw, 90.0f, 1.0f);
    CHECK(glm::distance(glm::vec2(stopped.position), glm::vec2(800, 0)) < 1.0f);
    // planned again (as the speed changes), the stop still turns
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations());
    CHECK(Chain(actor) == "casual_stand_idle +stand_2_run_180_med run_lowready_f +run_2_stand_90l casual_stand_idle");
    const auto replanned = FindChain(actor, Types::ActorChain::Stop);
    if (!replanned)
        return;
    const auto [position, yaw] = LargestSteps(actor, 0, replanned->End() + 500);
    CHECK(position < 0.4f);
    CHECK(yaw < 1.0f);
}

TEST("path chains: corners of 45 degrees or less stay round; the players stop, turn on the spot and walk on")
{
    // a 30 degree corner: no turn of its own
    auto gentle = Walker({glm::vec3(0, 0, 0), glm::vec3(800, 0, 0), glm::vec3(1500, 404, 0)}, Set::LowReady, 182.0f,
                         "casual_stand_idle", "run_lowready_f");
    TransitionLogic::PlanPath(gentle, Person(Set::LowReady), Animations());
    CHECK(!FindChain(gentle, Types::ActorChain::Turn));
    CHECK(ActorLogic::GetRoundedPath(gentle, Animations()).pieces.size() == 3);

    // the players': no running turns, nor starts or stops
    auto player = Walker({glm::vec3(0, 0, 0), glm::vec3(800, 0, 0), glm::vec3(800, 800, 0)}, Set::Players, 211.0f,
                         "pb_stand_alert", "pb_combatrun_forward_loop");
    TransitionLogic::PlanPath(player, Person(Set::Players), Animations());
    CHECK(Chain(player) == "pb_stand_alert pb_combatrun_forward_loop +pb_turn_90left +pb_combatrun_forward_loop "
                           "pb_stand_alert");
    const auto turn = FindChain(player, Types::ActorChain::Turn);
    if (!turn)
        return;
    // it stands at the corner as it turns, and faces the way on as it walks on
    const auto turning = ActorLogic::EvaluatePlacement(player, turn->start + 300, Animations());
    CHECK(glm::distance(glm::vec2(turning.position), glm::vec2(800, 0)) < 2.0f);
    const auto& after = *std::next(std::ranges::find(player.clips, turn->id, &Types::ActorClip::id));
    const auto going = ActorLogic::EvaluatePlacement(player, after.start + 500, Animations());
    CHECK_NEAR(going.yaw, 90.0f, 1.0f);
    CHECK_NEAR(going.position.x, 800.0f, 1.0f);
    CHECK(going.position.y > 20.0f);
    const auto [position, yaw] = LargestSteps(player, 900, after.start + 1000);
    CHECK(position < 0.4f);
    CHECK(yaw < 1.0f);
}

TEST("path chains: a dog runs off, turns and stops with its own")
{
    auto dog = Walker({glm::vec3(0, 0, 0), glm::vec3(800, 0, 0), glm::vec3(800, -800, 0)}, Set::Players, 320.0f,
                      "german_shepherd_idle", "german_shepherd_run");
    const auto body = ActorLogic::Body::Dog(ActorLogic::DogBreed::Shepherd);
    TransitionLogic::PlanPath(dog, body, Animations());
    CHECK(Chain(dog) == "german_shepherd_idle +german_shepherd_run_start german_shepherd_run "
                        "+german_shepard_run_turn_90_right +german_shepherd_run +german_shepherd_run_stop "
                        "german_shepherd_idle");
    const auto stop = FindChain(dog, Types::ActorChain::Stop);
    if (!stop)
        return;
    const auto stopped = ActorLogic::EvaluatePlacement(dog, TickOf(*stop, "end") + 50, Animations());
    CHECK(glm::distance(glm::vec2(stopped.position), glm::vec2(800, -800)) < 1.0f);
    CHECK_NEAR(stopped.yaw, -90.0f, 1.0f);
    const auto [position, yaw] = LargestSteps(dog, 900, stop->End() + 500);
    CHECK(position < 0.5f);
    CHECK(yaw < 1.0f);
}

TEST("path chains: without Auto transitions or Locomotion Auto, a path has none")
{
    auto actor = Walker({glm::vec3(0, 0, 0), glm::vec3(800, 0, 0), glm::vec3(800, 800, 0)}, Set::LowReady, 182.0f,
                        "casual_stand_idle", "run_lowready_f");
    actor.autoTransitions = false;
    const auto before = actor.clips;
    CHECK(!TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations()));
    CHECK(actor.clips == before);
    actor.autoTransitions = true;
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations());
    actor.locomotion = Types::ActorLocomotion::Off;
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations());
    CHECK(std::ranges::none_of(actor.clips, [](const Types::ActorClip& clip) {
        return clip.chain != Types::ActorChain::None;
    }));
    // nor do actors of the players' set walk the AI's walks as their locomotion
    auto old = actor;
    old.locomotion = Types::ActorLocomotion::Auto;
    old.movementSet = Set::Players;
    CHECK(!ActorLogic::FindLocomotion(old, "run_lowready_f"));
}


TEST("path chains: run here and stop from standing: it sets off, runs and stops facing the way asked, without jumps")
{
    // as the view's Run here and stop (Components::Actors::WalkTo): standing still, a run from the tick
    Types::Actor actor;
    actor.id = 1;
    actor.locomotion = Types::ActorLocomotion::Auto;
    actor.pathSpeed = 91.0f;
    actor.movementSet = Set::LowReady;
    actor.autoTransitions = true;
    actor.clips = {{.animation = "casual_stand_idle", .start = 0, .length = 10000}};
    ActorLogic::Normalize(actor);
    const auto runTo = [](Types::Actor& runner, std::uint32_t tick) {
        // (the walk it is on then as it was before it was planned)
        for (const auto walk : TransitionLogic::GetPathWalks(runner, Animations()))
            if (const auto [from, to] = TransitionLogic::GetWalkSpan(runner, walk); from <= tick && tick < to)
                TransitionLogic::UnplanWalk(runner, walk);
        const auto clip = ActorLogic::WalkTo(runner, tick, glm::vec3(300, 0, -20), "run_lowready_f", "casual_stand_idle",
                                             Animations(), 182.0f);
        std::ranges::find(runner.clips, clip, &Types::ActorClip::id)->walkSpeed = 182.0f;
        ActorLogic::FitWalk(runner, clip, Animations(), 182.0f);
        TransitionLogic::PlanWalk(runner, clip, Person(Set::LowReady), Animations(), 90.0f);
        return clip;
    };
    const auto walk = runTo(actor, 1000);
    CHECK(Chain(actor) == "casual_stand_idle +stand_2_run_f_2 run_lowready_f +run_2_stand_90l casual_stand_idle");
    const auto stop = FindChain(actor, Types::ActorChain::Stop);
    if (!stop)
        return;
    const auto stopped = ActorLogic::EvaluatePlacement(actor, TickOf(*stop, "end") + 50, Animations());
    CHECK(glm::distance(glm::vec2(stopped.position), glm::vec2(300, 0)) < 1.0f);
    CHECK_NEAR(stopped.yaw, 90.0f, 1.0f);
    const auto [position, yaw] = LargestSteps(actor, 900, stop->End() + 500);
    CHECK(position < 0.4f);
    CHECK(yaw < 1.0f);
    // down the slope as it goes: as high as the path where its start and stop put it
    float rise = 0.0f;
    for (auto tick = 901u; tick <= stop->End() + 500; ++tick)
        rise = std::max(rise, std::abs(ActorLogic::EvaluatePlacement(actor, tick, Animations()).position.z -
                                       ActorLogic::EvaluatePlacement(actor, tick - 1, Animations()).position.z));
    CHECK(rise < 0.05f);
    CHECK_NEAR(stopped.position.z, -20.0f, 0.1f);
    // planned: from its start's start to its stop's end
    CHECK(TransitionLogic::IsPlanned(actor, walk));
    CHECK(TransitionLogic::GetWalkSpan(actor, walk) == std::pair(1000u, stop->End()));
    // run there again from the same tick: as the first time
    auto again = actor;
    runTo(again, 1000);
    CHECK(Chain(again) == Chain(actor));
    CHECK(glm::distance(glm::vec2(ActorLogic::EvaluatePlacement(again, 9000, Animations()).position),
                        glm::vec2(300, 0)) < 1.0f);
}

namespace
{
    // the lookup before the game measured the transitions: their root motion guessed from the catalog's decoded play
    ActorLogic::AnimationLookup Unmeasured()
    {
        return [](const std::string& name) {
            auto info = Animations()(name);
            if (info && info->from && info->to)
            {
                const auto entry = Shipped().Find(name);
                info->rootMotion = entry->delta ? std::make_shared<const ActorLogic::RootMotion>(
                                                      ActorLogic::GuessRootMotion(glm::vec2(entry->motion), entry->turn))
                                                : nullptr;
            }
            return info;
        };
    }
}  // namespace

TEST("path chains: before the game measured its transitions, a path is planned with their guessed motion")
{
    const auto guess = ActorLogic::GuessRootMotion(glm::vec2(100, -20), 90.0f);
    CHECK(guess.samples.front() == glm::vec3(0.0f));
    CHECK(guess.samples.back() == glm::vec3(100, -20, 90));
    CHECK(ActorLogic::GetRootMotion(guess, 0.5f) == glm::vec3(50, -10, 45));

    // a short first stretch: the start, the turn and the stop all fit
    auto actor = Walker({glm::vec3(0, 0, 0), glm::vec3(300, 0, 0), glm::vec3(300, 300, 0)}, Set::LowReady, 182.0f,
                        "casual_stand_idle", "run_lowready_f");
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Unmeasured());
    const auto planned = "casual_stand_idle +stand_2_run_f_2 run_lowready_f +ai_run_lowready_f_turn_90_l "
                         "+run_lowready_f +run_2_stand_f_6 casual_stand_idle";
    CHECK(Chain(actor) == planned);
    // measured: fitted and planned again (Components::Actors::ReplanOnceMeasured), the same clips, without jumps
    const auto walk = TransitionLogic::GetPathWalks(actor, Animations()).front();
    TransitionLogic::UnplanWalk(actor, walk);
    ActorLogic::FitWalk(actor, walk, Animations(), 182.0f);
    TransitionLogic::PlanWalk(actor, walk, Person(Set::LowReady), Animations());
    CHECK(Chain(actor) == planned);
    const auto stop = FindChain(actor, Types::ActorChain::Stop);
    if (!stop)
        return;
    const auto [position, yaw] = LargestSteps(actor, 900, stop->End() + 500);
    CHECK(position < 0.4f);
    CHECK(yaw < 1.0f);
    CHECK(glm::distance(glm::vec2(ActorLogic::EvaluatePlacement(actor, stop->End() + 1000, Animations()).position),
                        glm::vec2(300, 300)) < 1.0f);
}


TEST("path chains: walks and crouched walks of the low ready set blend in and out, never going down and up again")
{
    // walking round a corner, the players and the AI walk round it (the game's turns are for running)
    for (const auto& [set, idle, walk, speed] :
         {std::tuple(Set::Players, "pb_stand_alert", "pb_stand_shoot_walk_forward", 94.0f),
          std::tuple(Set::LowReady, "casual_stand_idle", "walk_lowready_f", 91.0f)})
    {
        auto cornering = Walker({glm::vec3(0, 0, 0), glm::vec3(600, 0, 0), glm::vec3(600, 600, 0)}, set, speed, idle,
                                walk);
        CHECK(TransitionLogic::GetPathWalks(cornering, Animations()).size() == 1);
        TransitionLogic::PlanPath(cornering, Person(set), Animations());
        CHECK(!FindChain(cornering, Types::ActorChain::Turn));
        CHECK(ActorLogic::GetRoundedPath(cornering, Animations()).pieces.size() == 3);
    }
    // the game's exits and arrivals are for running
    auto walking = Walker({glm::vec3(0, 0, 0), glm::vec3(600, 0, 0)}, Set::LowReady, 91.0f, "casual_stand_idle",
                          "walk_lowready_f");
    TransitionLogic::PlanPath(walking, Person(Set::LowReady), Animations());
    CHECK(Chain(walking) == "casual_stand_idle walk_lowready_f casual_stand_idle");
    // (crouching to a crouched run doesn't go prone and get up)
    auto crouched = walking;
    crouched.stance = Stance::Crouch;
    TransitionLogic::PlanPath(crouched, Person(Set::LowReady), Animations());
    CHECK(Chain(crouched) == "casual_stand_idle walk_lowready_f casual_stand_idle");
    CHECK(TransitionLogic::JoinStates({Types::AnimationPosture::Crouch, Types::AnimationMovement::Stop},
                                      {Types::AnimationPosture::Crouch, Types::AnimationMovement::Run},
                                      Person(Set::LowReady), Animations())
              .empty());
}

// The user's actor_test scene (2026-10-26): campaign bodies (Moves as Auto: the AI's low ready) with clips of the
// players' movement, one running a path, two standing. Their legs play the AI's walks and idle, made for SP bodies,
// while the players' clips get the bodies their spine rig, so the legs line its
// stabilizer up as SP clips do (without, the torso bent forward by about 90 degrees). And the AI's sprint picked for
// the clip of one standing plays, as the actor has no speed to choose a gait by.
TEST("path chains: the AI's legs under the players' clips line the spine up, and a standing actor's sprint plays")
{
    const auto animations = Animations();
    const auto lowReady = Person(Set::LowReady);
    // what the animations do with a stabilizer: the players' full body ones move it, their upper body ones leave it at
    // rest, SP ones line it up
    const auto spines = [&](const std::string& name) {
        const auto info = animations(name);
        if (info && info->family == Types::AnimationFamily::MpPlayer)
            return info->kind == Types::AnimationKind::Torso ? PoseLogic::SpineKind::AtRest
                                                             : PoseLogic::SpineKind::Moves;
        return info && info->family == Types::AnimationFamily::SpHuman ? PoseLogic::SpineKind::Lined
                                                                       : PoseLogic::SpineKind::AtRest;
    };
    const auto legsOf = [](const ActorLogic::Layers& layers) {
        std::string names;
        for (const auto& legs : layers.legs)
            names += std::format("{}{}", names.empty() ? "" : " ", legs.clip ? legs.clip->animation : legs.animation);
        return names;
    };

    // running its path, as the user's Actor 1: the AI's walk at the path's speed, the stabilizer lined up by all of it
    // (the players' clips play no part of their own)
    Types::Actor runner;
    runner.id = 1;
    runner.movement = Types::ActorMovement::Path;
    runner.path = {{0, 0, 0}, {1000, 0, 0}};
    runner.locomotion = Types::ActorLocomotion::Auto;
    runner.pathSpeed = 94.0f;
    runner.movementSet = Set::Auto;
    runner.clips = {{.animation = "pb_prone_crawl", .start = 0, .length = 548},
                    {.animation = "pb_sprint", .start = 548, .length = 5000}};
    ActorLogic::Normalize(runner);
    auto layers = ActorLogic::GetLayers(runner, 3000, animations, lowReady);
    CHECK(legsOf(layers) == "walk_lowready_f");
    auto weights = ActorLogic::WeighLayers(layers, spines);
    CHECK_NEAR(weights.lined, 1.0f, 1e-5f);
    CHECK_NEAR(weights.resting, 0.0f, 1e-6f);
    CHECK(std::ranges::none_of(weights.animations, [](const ActorLogic::TreeWeight& entry) { return entry.clip; }));
    // getting up from the crawl, both the AI's crawl and walk line it up, as much as they play together
    layers = ActorLogic::GetLayers(runner, 700, animations, lowReady);
    CHECK(layers.legs.size() == 2);
    CHECK_NEAR(ActorLogic::WeighLayers(layers, spines).lined, 1.0f, 1e-5f);
    // the players' set walks with the players' animations, which move the stabilizer themselves
    weights = ActorLogic::WeighLayers(ActorLogic::GetLayers(runner, 3000, animations, Person(Set::Players)), spines);
    CHECK_NEAR(weights.lined, 0.0f, 1e-6f);
    CHECK_NEAR(weights.resting, 0.0f, 1e-6f);
    // on a path the AI's sprint picked for the clip still stands for the locomotion: at a sprint's speed it sprints
    runner.clips[1].animation = "ai_sprint_f";
    CHECK(legsOf(ActorLogic::GetLayers(runner, 3000, animations, lowReady)) == "walk_lowready_f");
    runner.pathSpeed = 232.0f;
    CHECK(legsOf(ActorLogic::GetLayers(runner, 3000, animations, lowReady)) == "ai_sprint_f");

    // standing, as the user's Actors 2 and 3: the AI's idle for the players' one, lined up
    Types::Actor stander;
    stander.id = 2;
    stander.locomotion = Types::ActorLocomotion::Auto;
    stander.movementSet = Set::Auto;
    stander.clips = {{.animation = "pb_stand_alert", .start = 0, .length = 60000}};
    ActorLogic::Normalize(stander);
    layers = ActorLogic::GetLayers(stander, 3000, animations, lowReady);
    CHECK(legsOf(layers) == "casual_stand_idle");
    CHECK_NEAR(ActorLogic::WeighLayers(layers, spines).lined, 1.0f, 1e-5f);
    // shooting crouched with the players' upper body clip (the user's crouching actor, 2026-10-28): the AI's crouch
    // idle as its legs, the stabilizer aimed ahead for it as the players' legs hold it, not at rest (which turned the
    // torso and gun 37 degrees to the right with the idle's pelvis, and the death after it turned them back)
    auto shooter = stander;
    shooter.clips = {{.animation = "pt_crouch_shoot", .start = 0, .length = 60000}};
    ActorLogic::Normalize(shooter);
    layers = ActorLogic::GetLayers(shooter, 3000, animations, lowReady);
    CHECK(legsOf(layers) == "casual_crouch_idle");
    weights = ActorLogic::WeighLayers(layers, spines);
    CHECK(weights.aimed.size() == 1 && weights.aimed[0].first == "casual_crouch_idle" &&
          std::abs(weights.aimed[0].second - 1.0f) < 1e-5f);
    CHECK_NEAR(weights.resting, 0.0f, 1e-6f);
    CHECK_NEAR(weights.lined, 0.0f, 1e-6f);
    // with the players' legs, which hold it themselves, none
    weights = ActorLogic::WeighLayers(ActorLogic::GetLayers(shooter, 3000, animations, Person(Set::Players)), spines);
    CHECK(weights.aimed.empty());
    CHECK_NEAR(weights.resting, 0.0f, 1e-6f);
    // the AI's sprints picked for its clip play as they are (no legs), in place
    for (const auto* sprint : {"ai_sprint_f", "ai_cqb_sprint"})
    {
        stander.clips[0].animation = sprint;
        layers = ActorLogic::GetLayers(stander, 3000, animations, lowReady);
        CHECK(layers.legs.empty());
        CHECK(layers.clipCount == 1 && layers.clips[0].clip->animation == sprint && !layers.locomotion[0]);
        weights = ActorLogic::WeighLayers(layers, spines);
        CHECK(weights.animations.size() == 1 && std::abs(weights.animations[0].weight - 1.0f) < 1e-6f);
        CHECK(ActorLogic::GetRootMotionUse(stander, stander.clips[0], animations) == ActorLogic::RootMotionUse::None);
        CHECK(glm::distance(ActorLogic::EvaluatePlacement(stander, 4000, animations).position, stander.position) <
              1e-4f);
    }

    // added running a path (the starting point's players' run), before its second point is placed the run plays as it
    // is; once it has a way to go, the AI's run of its speed, lined up
    const ActorLogic::StartAnimations starts{.idle = "pb_stand_alert",
                                             .walk = "pb_stand_shoot_walk_forward",
                                             .run = "pb_combatrun_forward_loop",
                                             .locomotion = true};
    auto placing = ActorLogic::MakeStart(ActorLogic::Start::RunPath, starts, 0, 60000, "");
    placing.movementSet = Set::Auto;
    layers = ActorLogic::GetLayers(placing, 100, animations, lowReady);
    CHECK(layers.legs.empty() && layers.clipCount == 1 &&
          layers.clips[0].clip->animation == "pb_combatrun_forward_loop");
    placing.path.push_back({1000, 0, 0});
    layers = ActorLogic::GetLayers(placing, 100, animations, lowReady);
    CHECK(layers.legs.size() == 1 && !layers.legs[0].animation.starts_with("pb_"));
    CHECK_NEAR(ActorLogic::WeighLayers(layers, spines).lined, 1.0f, 1e-5f);
}

TEST("path chains: the path's times count what its start, turns and stop move: an AI's and a dog's paths arrive (H1)")
{
    // a straight run of the AI's (as a campaign body's): its start, the run, its stop
    auto actor = Walker({glm::vec3(0, 0, 0), glm::vec3(284, 0, 0)}, Set::LowReady, 182.0f, "casual_stand_idle",
                        "run_lowready_f");
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations());
    CHECK(Chain(actor) == "casual_stand_idle +stand_2_run_f_2 run_lowready_f +run_2_stand_f_6 casual_stand_idle");
    const auto stop = FindChain(actor, Types::ActorChain::Stop);
    auto timing = ActorLogic::GetPathTiming(actor, Animations());
    // setting off as its start does, there as its stop's move ends
    CHECK(timing.start && *timing.start == 1000);
    CHECK(timing.reached.front() && *timing.reached.front() == 1000);
    CHECK(timing.arrival.has_value());
    if (stop && timing.arrival)
    {
        CHECK(*timing.arrival > stop->start && *timing.arrival <= TickOf(*stop, "end") + 300);
        const auto there = ActorLogic::EvaluatePlacement(actor, *timing.arrival + 20, Animations());
        CHECK(glm::distance(glm::vec2(there.position), glm::vec2(284, 0)) < 3.0f);
    }
    // round a corner: the corner's point, then the end
    auto cornering = Walker({glm::vec3(0, 0, 0), glm::vec3(800, 0, 0), glm::vec3(800, 800, 0)}, Set::LowReady, 182.0f,
                            "casual_stand_idle", "run_lowready_f");
    TransitionLogic::PlanPath(cornering, Person(Set::LowReady), Animations());
    CHECK(FindChain(cornering, Types::ActorChain::Turn) && FindChain(cornering, Types::ActorChain::Stop));
    timing = ActorLogic::GetPathTiming(cornering, Animations());
    CHECK(timing.reached[1] && timing.arrival && *timing.reached[1] < *timing.arrival);
    // a dog's run, turn and stop
    auto dog = Walker({glm::vec3(0, 0, 0), glm::vec3(800, 0, 0), glm::vec3(800, -800, 0)}, Set::Players, 320.0f,
                      "german_shepherd_idle", "german_shepherd_run");
    TransitionLogic::PlanPath(dog, ActorLogic::Body::Dog(ActorLogic::DogBreed::Shepherd), Animations());
    timing = ActorLogic::GetPathTiming(dog, Animations());
    CHECK(timing.start && timing.reached[1] && timing.arrival);
    // without transitions, as before: from the walk's start to where it walked to the end, as the idle after it
    // blends in
    auto plain = Walker({glm::vec3(0, 0, 0), glm::vec3(284, 0, 0)}, Set::LowReady, 182.0f, "casual_stand_idle",
                        "run_lowready_f");
    timing = ActorLogic::GetPathTiming(plain, Animations());
    CHECK(timing.start && *timing.start <= 1010 && timing.arrival && *timing.arrival >= plain.clips[1].End() &&
          *timing.arrival <= plain.clips[2].start + plain.clips[2].blendIn + 10);
}

namespace
{
    bool Has(std::string_view name)
    {
        return Animations()(std::string(name)).has_value();
    }

    // an AI (low ready) running a path at a gait, fitted to it and planned
    Types::Actor GaitWalker(std::vector<glm::vec3> path, Types::ActorGait gait)
    {
        auto actor = Walker(std::move(path), Set::LowReady, 182.0f, "casual_stand_idle", "run_lowready_f");
        actor.stretches.assign(actor.path.size(), {.gait = gait});
        ActorLogic::ResolveStretches(actor, Person(Set::LowReady), Has);
        ActorLogic::FitOnlyWalk(actor, Animations());
        TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations());
        return actor;
    }
}  // namespace

TEST("path chains: a corner too near the end for its turn and the stop is run round, so the actor stops there (H6)")
{
    // the user's: 262 units, a right angle, 191 units; the running turn would land it 30 short, no room to stop
    auto actor = Walker({glm::vec3(0, 0, 0), glm::vec3(262, 0, 0), glm::vec3(262, -191, 0)}, Set::LowReady, 182.0f,
                        "casual_stand_idle", "run_lowready_f");
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations());
    CHECK(!FindChain(actor, Types::ActorChain::Turn));
    const auto stop = FindChain(actor, Types::ActorChain::Stop);
    CHECK(stop != nullptr);
    if (stop)
    {
        const auto stopped = ActorLogic::EvaluatePlacement(actor, TickOf(*stop, "end") + 50, Animations());
        CHECK(glm::distance(glm::vec2(stopped.position), glm::vec2(262, -191)) < 2.0f);
        const auto [position, yaw] = LargestSteps(actor, 900, stop->End() + 500);
        CHECK(position < 0.4f);
        CHECK(yaw < 1.0f);
    }
    CHECK(ActorLogic::GetPathTiming(actor, Animations()).arrival.has_value());
    // with room, the turn stays (as above, 800 and 800)
    CHECK(
        FindChain(GaitWalker({glm::vec3(0, 0, 0), glm::vec3(800, 0, 0), glm::vec3(800, 800, 0)}, Types::ActorGait::Run),
                  Types::ActorChain::Turn));
    // and with a walk longer than the path (not fitted to it): it gets as far as the path's end
    for (const auto& [set, speed, idle, run] :
         {std::tuple(Set::Players, 211.0f, "pb_stand_alert", "pb_combatrun_forward_loop"),
          std::tuple(Set::LowReady, 182.0f, "casual_stand_idle", "run_lowready_f")})
    {
        auto longer = Walker({glm::vec3(0, 0, 0), glm::vec3(300, 0, 0), glm::vec3(300, 300, 0)}, set, speed, idle, run);
        longer.clips[1].length = 3500;
        longer.clips[2].start = 4500;
        ActorLogic::Normalize(longer);
        TransitionLogic::PlanPath(longer, Person(set), Animations());
        CHECK(FindChain(longer, Types::ActorChain::Turn));
    }
    auto dog = Walker({glm::vec3(0, 0, 0), glm::vec3(300, 0, 0), glm::vec3(300, 300, 0)}, Set::Players, 300.0f,
                      "german_shepherd_idle", "german_shepherd_run");
    dog.clips[1].length = 3000;
    dog.clips[2].start = 4000;
    ActorLogic::Normalize(dog);
    TransitionLogic::PlanPath(dog, ActorLogic::Body::Dog(ActorLogic::DogBreed::Shepherd), Animations());
    CHECK(Chain(dog) ==
          "german_shepherd_idle +german_shepherd_run_start german_shepherd_run "
          "+german_shepard_run_turn_90_left +german_shepherd_run +german_shepherd_run_stop "
          "german_shepherd_idle");
}

TEST("path chains: sprinting, corners are run round, so a faster gait never arrives later (H6)")
{
    const std::vector<glm::vec3> path{glm::vec3(0, 0, 0), glm::vec3(800, 0, 0), glm::vec3(800, 800, 0)};
    const auto run = GaitWalker(path, Types::ActorGait::Run);
    const auto sprint = GaitWalker(path, Types::ActorGait::Sprint);
    CHECK(FindChain(run, Types::ActorChain::Turn));
    CHECK(!FindChain(sprint, Types::ActorChain::Turn));
    const auto ran = ActorLogic::GetPathTiming(run, Animations());
    const auto sprinted = ActorLogic::GetPathTiming(sprint, Animations());
    CHECK(ran.arrival && sprinted.arrival && *sprinted.arrival < *ran.arrival);
    // and a sprint after a run, the same at that corner
    auto mixed = Walker(path, Set::LowReady, 182.0f, "casual_stand_idle", "run_lowready_f");
    mixed.stretches = {{.gait = Types::ActorGait::Run}, {.gait = Types::ActorGait::Sprint}, {}};
    ActorLogic::ResolveStretches(mixed, Person(Set::LowReady), Has);
    ActorLogic::FitOnlyWalk(mixed, Animations());
    TransitionLogic::PlanPath(mixed, Person(Set::LowReady), Animations());
    CHECK(!FindChain(mixed, Types::ActorChain::Turn));
    CHECK(ActorLogic::GetPathTiming(mixed, Animations()).arrival.has_value());
}

TEST(
    "path chains: a walk a few degrees off straight (the players' sprint) is the path's walk, planned after edits (H2)")
{
    // a campaign body's run swapped for the players' sprint, picked: it goes 3 degrees to the right
    auto actor = Walker({glm::vec3(0, 0, 0), glm::vec3(600, 0, 0), glm::vec3(600, -400, 0)}, Set::LowReady, 182.0f,
                        "casual_stand_idle", "run_lowready_f");
    actor.clips[1].animation = "pb_sprint";
    actor.clips[1].picked = true;
    const auto walk = actor.clips[1].id;
    const auto angle = ActorLogic::GetClipWalk(actor, actor.clips[1], Animations()).angle;
    CHECK(angle && *angle < -2.0f && *angle > -4.0f);
    CHECK(TransitionLogic::GetPathWalks(actor, Animations()) == std::vector<std::uint32_t>{walk});
    ActorLogic::FitOnlyWalk(actor, Animations());
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations());
    CHECK(FindChain(actor, Types::ActorChain::Start) && FindChain(actor, Types::ActorChain::Stop));
    CHECK(TransitionLogic::GetPathWalks(actor, Animations()) == std::vector<std::uint32_t>{walk});
    // End path here at the corner: fitted and planned again (as Components::Actors::FitOnlyWalk does)
    actor.path.pop_back();
    ActorLogic::Normalize(actor);
    TransitionLogic::UnplanWalk(actor, walk);
    ActorLogic::FitOnlyWalk(actor, Animations());
    TransitionLogic::PlanWalk(actor, walk, Person(Set::LowReady), Animations());
    const auto stop = FindChain(actor, Types::ActorChain::Stop);
    CHECK(stop != nullptr);
    // (its stop goes the way it faces, 3 degrees off the path, so a few units to the side)
    if (stop)
        CHECK(glm::distance(glm::vec2(ActorLogic::EvaluatePlacement(actor, stop->End() + 1000, Animations()).position),
                            glm::vec2(600, 0)) < 5.0f);
    CHECK(ActorLogic::GetPathTiming(actor, Animations()).arrival.has_value());
}

TEST("path chains: a wait at a point stops there, stands with the idle as long, and sets off again (proposal 9)")
{
    auto actor = Walker({glm::vec3(0, 0, 0), glm::vec3(400, 0, 0), glm::vec3(800, 300, 0)}, Set::LowReady, 182.0f,
                        "casual_stand_idle", "run_lowready_f");
    actor.stretches.assign(actor.path.size(), {.gait = Types::ActorGait::Run});
    actor.stretches[1].wait = 2000;
    ActorLogic::ResolveStretches(actor, Person(Set::LowReady), Has);
    ActorLogic::FitOnlyWalk(actor, Animations());
    TransitionLogic::PlanPath(actor, Person(Set::LowReady), Animations());
    CHECK(Chain(actor) ==
          "casual_stand_idle +stand_2_run_f_2 run_lowready_f +run_2_stand_f_6 +casual_stand_idle "
          "+stand_2_run_f_2 +run_lowready_f +run_2_stand_f_6 casual_stand_idle");
    const auto idle = std::ranges::find_if(actor.clips, [](const Types::ActorClip& clip) {
        return clip.chain == Types::ActorChain::Stop && clip.animation == "casual_stand_idle";
    });
    CHECK(idle != actor.clips.end() && idle->length == 2000);
    if (idle != actor.clips.end())
        for (const auto tick : {idle->start + 100, idle->End() - 100})
            CHECK(glm::distance(glm::vec2(ActorLogic::EvaluatePlacement(actor, tick, Animations()).position),
                                glm::vec2(400, 0)) < 2.0f);
    CHECK(ActorLogic::GetPathTiming(actor, Animations()).arrival.has_value());
    // taken out, the walk is one again
    TransitionLogic::UnplanPath(actor);
    CHECK(Chain(actor) == "casual_stand_idle run_lowready_f casual_stand_idle");
    // (the idle after it where the walk ends, not where the wait's stop started)
    CHECK(actor.clips.size() == 3 && actor.clips[2].start >= actor.clips[1].End());
}
