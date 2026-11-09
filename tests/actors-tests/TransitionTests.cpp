#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/AnimationCatalogLogic.hpp"
#include "Components/TransitionLogic.hpp"

using namespace IWXMVM;

// Chaining the short turns and transitions (TransitionLogic; step 8): the states clips
// start and end in, the game's transitions between them, the chain that joins two clips, what can follow a clip, Auto
// transitions putting the chain in, and how a transition turns the actor

namespace
{
    using Posture = Types::AnimationPosture;
    using Movement = Types::AnimationMovement;
    using Set = Types::ActorMovementSet;
    using State = Types::AnimationState;

    const std::filesystem::path SHIPPED_CATALOG =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" / "resources" /
        "animations.json";

    // the catalog that comes with the mod, whose states the transitions go by
    const Types::AnimationCatalog& Shipped()
    {
        static const auto catalog = [] {
            try
            {
                std::ifstream file(SHIPPED_CATALOG);
                return AnimationCatalogLogic::Parse(nlohmann::json::parse(file));
            }
            catch (const std::exception& e)
            {
                Test::Fail(__FILE__, __LINE__, std::string("the catalog doesn't read: ") + e.what());
                return Types::AnimationCatalog{};
            }
        }();
        return catalog;
    }

    // root motions some tests give animations, by name
    std::map<std::string, std::shared_ptr<const ActorLogic::RootMotion>, std::less<>>& Tracks()
    {
        static std::map<std::string, std::shared_ptr<const ActorLogic::RootMotion>, std::less<>> tracks;
        return tracks;
    }

    // what the game would know of the catalog's animations: their numbers, states, notetracks and motion in a play
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
            if (const auto track = Tracks().find(name); track != Tracks().end())
                info.rootMotion = track->second;
            return info;
        };
    }

    ActorLogic::Body Person(Set set, Types::GunClass gun = Types::GunClass::Rifle)
    {
        ActorLogic::Body body(gun);
        body.set = set;
        return body;
    }

    Types::ActorClip Clip(std::string animation, std::uint32_t start, std::uint32_t length, bool loop = true)
    {
        return {.animation = std::move(animation), .start = start, .length = length, .loop = loop};
    }

    Types::Actor Actor(std::vector<Types::ActorClip> clips)
    {
        Types::Actor actor;
        actor.id = 1;
        actor.clips = std::move(clips);
        ActorLogic::Normalize(actor);
        return actor;
    }

    std::vector<std::string> Names(const std::vector<TransitionLogic::Link>& links)
    {
        std::vector<std::string> names;
        for (const auto& link : links)
            names.push_back(link.animation);
        return names;
    }

    std::vector<std::string> ClipNames(const Types::Actor& actor)
    {
        std::vector<std::string> names;
        for (const auto& clip : actor.clips)
            names.push_back(clip.animation);
        return names;
    }

    std::uint32_t Ticks(float seconds)
    {
        return std::max(1u, static_cast<std::uint32_t>(std::lround(seconds * 1000.0f)));
    }

    bool CompatibleMovement(Movement listed, Movement catalog)
    {
        const auto moving = [](Movement movement) {
            return movement == Movement::Walk || movement == Movement::Run || movement == Movement::Sprint;
        };
        return listed == catalog || (moving(listed) && moving(catalog));
    }
}  // namespace

TEST("transitions: every one the tables list is in the catalog, going between the states the catalog says")
{
    const std::array bodies{Person(Set::Players),
                            Person(Set::Players, Types::GunClass::Pistol),
                            Person(Set::Players, Types::GunClass::RocketLauncher),
                            Person(Set::Players, Types::GunClass::DualWield),
                            Person(Set::LowReady),
                            Person(Set::Cqb),
                            ActorLogic::Body::Dog(ActorLogic::DogBreed::Shepherd),
                            ActorLogic::Body::Dog(ActorLogic::DogBreed::Hellhound)};
    for (const auto& body : bodies)
        for (const auto& transition : TransitionLogic::GetTransitions(body))
        {
            const auto entry = Shipped().Find(transition.animation);
            CHECK(entry != nullptr);
            if (!entry)
            {
                std::printf("  missing: %s\n", std::string(transition.animation).c_str());
                continue;
            }
            CHECK(!entry->loop || transition.animation.starts_with("pb_"));
            CHECK(!entry->death);
            // (where the catalog knows the states: the postures alike, standing or moving alike)
            if (TransitionLogic::IsKnown(entry->from) && body.kind == Types::ActorKind::Human)
            {
                const bool fromOk = entry->from.posture == transition.from.posture &&
                                    CompatibleMovement(transition.from.movement, entry->from.movement);
                const bool toOk = entry->to.posture == transition.to.posture &&
                                  CompatibleMovement(transition.to.movement, entry->to.movement);
                CHECK(fromOk && toOk);
                if (!fromOk || !toOk)
                    std::printf("  states differ: %s\n", std::string(transition.animation).c_str());
            }
        }
}

TEST("transitions: idle to run sets off with stand_2_run (the AI's exit forward)")
{
    const auto actor = Actor({Clip("casual_stand_idle", 0, 3000), Clip("run_lowready_f", 3000, 5000)});
    const auto links = TransitionLogic::JoinClips(actor, 0, 1, Person(Set::LowReady), Animations());
    CHECK(Names(links) == std::vector<std::string>{"stand_2_run_f_2"});
    CHECK(!links.empty() && !links.front().turn);
    // the game's blends of an exit: in over 0.2 s, the run taking over in its last 0.15 s
    CHECK(!links.empty() && links.front().blends.in == 200 && links.front().blends.handOver == 150);
    CHECK(!links.empty() && links.front().label == "Set off running");
    // to the left, the one that sets off to the left
    const auto left = TransitionLogic::JoinClips(actor, 0, 1, Person(Set::LowReady), Animations(), 80.0f);
    CHECK(Names(left) == std::vector<std::string>{"stand_2_run_l"});
    CHECK(!left.empty() && left.front().turn == 80.0f);
}

TEST("transitions: run to idle turning 90 degrees stops with run_2_stand_90l")
{
    const auto actor = Actor({Clip("run_lowready_f", 0, 3000), Clip("casual_stand_idle", 3000, 5000)});
    const auto links = TransitionLogic::JoinClips(actor, 0, 1, Person(Set::LowReady), Animations(), 90.0f);
    CHECK(Names(links) == std::vector<std::string>{"run_2_stand_90l"});
    CHECK(!links.empty() && links.front().turn == 90.0f);
    CHECK(!links.empty() && links.front().label == "Stop, turning left 90 deg");
    // straight, the forward one; to the right, the right one; round, the one turning round
    CHECK(Names(TransitionLogic::JoinClips(actor, 0, 1, Person(Set::LowReady), Animations())) ==
          std::vector<std::string>{"run_2_stand_f_6"});
    CHECK(Names(TransitionLogic::JoinClips(actor, 0, 1, Person(Set::LowReady), Animations(), -100.0f)) ==
          std::vector<std::string>{"run_2_stand_90r"});
    CHECK(Names(TransitionLogic::JoinClips(actor, 0, 1, Person(Set::LowReady), Animations(), 170.0f)) ==
          std::vector<std::string>{"run_2_stand_180l"});
    // into a crouch: the one stopping crouched
    const auto crouched = Actor({Clip("run_lowready_f", 0, 3000), Clip("casual_crouch_idle", 3000, 5000)});
    CHECK(Names(TransitionLogic::JoinClips(crouched, 0, 1, Person(Set::LowReady), Animations())) ==
          std::vector<std::string>{"run_2_crouch_f"});
}

TEST("transitions: prone to run takes two (up into a crouch, then off from it)")
{
    const auto actor = Actor({Clip("prone_idle", 0, 3000), Clip("run_lowready_f", 3000, 5000)});
    const auto links = TransitionLogic::JoinClips(actor, 0, 1, Person(Set::LowReady), Animations());
    CHECK(Names(links) == (std::vector<std::string>{"prone2crouch_straight", "crouch_2run_f"}));
    // stand to prone straight, crouched to standing
    CHECK(Names(TransitionLogic::JoinStates({Posture::Stand, Movement::Stop}, {Posture::Prone, Movement::Stop},
                                            Person(Set::LowReady), Animations())) ==
          std::vector<std::string>{"stand_2_prone"});
    CHECK(Names(TransitionLogic::JoinStates({Posture::Crouch, Movement::Stop}, {Posture::Stand, Movement::Stop},
                                            Person(Set::LowReady), Animations())) ==
          std::vector<std::string>{"exposed_crouch_2_stand"});
    // running into prone: stopping, then down
    CHECK(Names(TransitionLogic::JoinStates({Posture::Stand, Movement::Run}, {Posture::Prone, Movement::Stop},
                                            Person(Set::LowReady), Animations())) ==
          (std::vector<std::string>{"run_2_stand_f_6", "stand_2_prone"}));
}

TEST("transitions: the players' turn on the spot with pb_turn_90 (by the gun's class) and have no starts or stops")
{
    const auto actor = Actor({Clip("pb_stand_alert", 0, 3000), Clip("pb_stand_alert", 3000, 5000)});
    const auto left = TransitionLogic::JoinClips(actor, 0, 1, Person(Set::Players), Animations(), 90.0f);
    CHECK(Names(left) == std::vector<std::string>{"pb_turn_90left"});
    CHECK(!left.empty() && left.front().turn == 90.0f);
    CHECK(Names(TransitionLogic::JoinClips(actor, 0, 1, Person(Set::Players), Animations(), -70.0f)) ==
          std::vector<std::string>{"pb_turn_90right"});
    CHECK(Names(TransitionLogic::JoinClips(actor, 0, 1, Person(Set::Players, Types::GunClass::Pistol), Animations(),
                                           90.0f)) == std::vector<std::string>{"pb_pistol_turn_90left"});
    // within the threshold the legs just turn
    CHECK(TransitionLogic::JoinClips(actor, 0, 1, Person(Set::Players), Animations(), 30.0f).empty());
    // standing to running: none (the game blends its run in)
    const auto running = Actor({Clip("pb_stand_alert", 0, 3000), Clip("pb_combatrun_forward_loop", 3000, 5000)});
    CHECK(TransitionLogic::JoinClips(running, 0, 1, Person(Set::Players), Animations()).empty());
    // crouching: the players' own
    const auto crouching = Actor({Clip("pb_stand_alert", 0, 3000), Clip("pb_crouch_alert", 3000, 5000)});
    CHECK(Names(TransitionLogic::JoinClips(crouching, 0, 1, Person(Set::Players), Animations())) ==
          std::vector<std::string>{"pb_rifle_stand2crouch"});
}

TEST("transitions: a dog sets off, stops and turns with its breed's own")
{
    const auto shepherd = ActorLogic::Body::Dog(ActorLogic::DogBreed::Shepherd);
    const auto hellhound = ActorLogic::Body::Dog(ActorLogic::DogBreed::Hellhound);
    const auto actor = Actor({Clip("german_shepherd_idle", 0, 3000), Clip("german_shepherd_run", 3000, 5000),
                              Clip("german_shepherd_idle", 8000, 3000)});
    CHECK(Names(TransitionLogic::JoinClips(actor, 0, 1, shepherd, Animations())) ==
          std::vector<std::string>{"german_shepherd_run_start"});
    CHECK(Names(TransitionLogic::JoinClips(actor, 0, 1, shepherd, Animations(), 180.0f)) ==
          std::vector<std::string>{"german_shepherd_run_start_180_l"});
    CHECK(Names(TransitionLogic::JoinClips(actor, 1, 2, shepherd, Animations())) ==
          std::vector<std::string>{"german_shepherd_run_stop"});
    CHECK(Names(TransitionLogic::JoinStates({Posture::Stand, Movement::Stop}, {Posture::Stand, Movement::Stop},
                                            shepherd, Animations(), 90.0f)) ==
          std::vector<std::string>{"german_shepard_turn_90_left"});
    CHECK(Names(TransitionLogic::JoinStates({Posture::Stand, Movement::Run}, {Posture::Stand, Movement::Run},
                                            shepherd, Animations(), -90.0f)) ==
          std::vector<std::string>{"german_shepard_run_turn_90_right"});
    // the hellhound's own first, but its starts don't move it (the game's code does): the shepherd's
    const auto hound = Actor({Clip("zombie_dog_idle", 0, 3000), Clip("zombie_dog_run", 3000, 5000),
                              Clip("zombie_dog_idle", 8000, 3000)});
    CHECK(Names(TransitionLogic::JoinClips(hound, 0, 1, hellhound, Animations())) ==
          std::vector<std::string>{"german_shepherd_run_start"});
    CHECK(Names(TransitionLogic::JoinClips(hound, 1, 2, hellhound, Animations())) ==
          std::vector<std::string>{"zombie_dog_run_stop"});
    // (a person's isn't a dog's)
    CHECK(TransitionLogic::JoinClips(actor, 0, 1, Person(Set::LowReady), Animations()).empty());
}

TEST("transitions: nothing between alike states, into a death or where a state isn't known")
{
    const auto runs = Actor({Clip("run_lowready_f", 0, 3000), Clip("run_lowready_f", 3000, 5000)});
    CHECK(TransitionLogic::JoinClips(runs, 0, 1, Person(Set::LowReady), Animations()).empty());
    const auto death = Actor({Clip("run_lowready_f", 0, 3000), Clip("ai_death_fallforward", 3000, 1500, false)});
    CHECK(TransitionLogic::JoinClips(death, 0, 1, Person(Set::LowReady), Animations()).empty());
    // a cutscene's animation says nothing of its states
    const auto unknown = Actor({Clip("casual_stand_idle", 0, 3000), Clip("ch_khe_e1b_chinookdropoff_guy05", 3000, 5000)});
    CHECK(TransitionLogic::JoinClips(unknown, 0, 1, Person(Set::LowReady), Animations()).empty());
}

TEST("transitions: a locomotion clip is in the state its locomotion plays, an upper body clip in its legs'")
{
    auto actor = Actor({Clip("pb_stand_alert", 0, 3000), Clip("pb_stand_shoot_walk_forward", 3000, 3000),
                        Clip("pt_reload_stand_auto", 6000, 2000, false)});
    actor.movement = Types::ActorMovement::Path;
    actor.path = {glm::vec3(0.0f), glm::vec3(2000.0f, 0.0f, 0.0f)};
    actor.locomotion = Types::ActorLocomotion::Auto;
    actor.pathSpeed = 211.0f;
    const auto body = Person(Set::Players);
    // the walk clip runs at the actor's speed
    CHECK((TransitionLogic::GetClipState(actor, 1, true, body, Animations()) == State{Posture::Stand, Movement::Run}));
    actor.pathSpeed = 94.0f;
    CHECK((TransitionLogic::GetClipState(actor, 1, true, body, Animations()) == State{Posture::Stand, Movement::Walk}));
    CHECK((TransitionLogic::GetClipState(actor, 0, true, body, Animations()) == State{Posture::Stand, Movement::Stop}));
    // the reload walks on with the walk before it
    CHECK((TransitionLogic::GetClipState(actor, 2, false, body, Animations()) ==
           State{Posture::Stand, Movement::Walk}));
}

TEST("transitions: Auto transitions puts the chain in, and joining again replaces it")
{
    auto actor = Actor({Clip("casual_stand_idle", 0, 3000), Clip("run_lowready_f", 3000, 5000)});
    actor.autoTransitions = true;
    const auto run = actor.clips[1].id;
    const auto body = Person(Set::LowReady);
    CHECK(TransitionLogic::JoinAround(actor, run, body, Animations()));
    CHECK(ClipNames(actor) == (std::vector<std::string>{"casual_stand_idle", "stand_2_run_f_2", "run_lowready_f"}));
    // the transition plays once from where the idle ends, the run taking over in its last 0.15 s and keeping its end
    const auto exit = Ticks(Shipped().Find("stand_2_run_f_2")->Duration());
    CHECK(actor.clips[0].End() == 3000);
    CHECK(actor.clips[1].start == 3000 && actor.clips[1].length == exit - 150);
    CHECK(actor.clips[1].chain == Types::ActorChain::Join && !actor.clips[1].loop && !actor.clips[1].moves);
    CHECK(actor.clips[1].blendIn == 200);
    CHECK(actor.clips[2].start == actor.clips[1].End() && actor.clips[2].End() == 8000);
    CHECK(actor.clips[2].blendIn == 150);
    // again: the same, not one more
    const auto once = actor;
    TransitionLogic::JoinAround(actor, run, body, Animations());
    CHECK(actor.clips == once.clips);
    // taken out: the run back where it started
    CHECK(TransitionLogic::Unjoin(actor, run));
    CHECK(ClipNames(actor) == (std::vector<std::string>{"casual_stand_idle", "run_lowready_f"}));
    CHECK(actor.clips[1].start == 3000 && actor.clips[1].End() == 8000);
    // off, nothing
    actor.autoTransitions = false;
    CHECK(!TransitionLogic::JoinAround(actor, run, body, Animations()));
    CHECK(actor.clips.size() == 2);
}

TEST("transitions: a clip that plays once moves on with the clips after it to make room")
{
    // a reload after crouching: the reload and the death after it move on as far as the change of stance takes, and
    // the death's hit with it
    auto actor = Actor({Clip("exposed_idle_alert_v1", 0, 3000), Clip("exposed_crouch_idle_alert_v1", 3000, 2000, false),
                        Clip("ai_death_fallforward", 6000, 1500, false)});
    actor.events.push_back({.type = Types::ActorEventType::Hit, .tick = 6000});
    ActorLogic::Normalize(actor);
    actor.autoTransitions = true;
    const auto crouched = actor.clips[1].id;
    TransitionLogic::JoinAround(actor, crouched, Person(Set::LowReady), Animations());
    CHECK(ClipNames(actor) == (std::vector<std::string>{"exposed_idle_alert_v1", "exposed_stand_2_crouch",
                                                        "exposed_crouch_idle_alert_v1", "ai_death_fallforward"}));
    const auto by = actor.clips[2].start - 3000;
    CHECK(by > 0 && actor.clips[2].length == 2000);
    CHECK(actor.clips[3].start == 6000 + by);
    CHECK(actor.events.front().tick == 6000 + by);
}

TEST("transitions: Then... plays an animation after a clip, joined to the clips around it")
{
    auto actor = Actor({Clip("run_lowready_f", 0, 3000), Clip("run_lowready_f", 3000, 5000)});
    const auto body = Person(Set::LowReady);
    const auto stop = TransitionLogic::PlayAfter(actor, actor.clips[0].id, "run_2_stand_90l", body, Animations());
    CHECK(stop != 0);
    CHECK(ClipNames(actor) == (std::vector<std::string>{"run_lowready_f", "run_2_stand_90l", "run_lowready_f"}));
    CHECK(actor.clips[1].start == 3000 && actor.clips[1].length == 1000 && !actor.clips[1].loop);
    CHECK(actor.clips[1].blendIn == 300);
    // the run after it gives up as much of its start
    CHECK(actor.clips[2].start == 4000 && actor.clips[2].End() == 8000);
    // with Auto transitions, set off again after it
    actor = Actor({Clip("run_lowready_f", 0, 3000), Clip("run_lowready_f", 3000, 5000)});
    actor.autoTransitions = true;
    TransitionLogic::PlayAfter(actor, actor.clips[0].id, "run_2_stand_f_6", body, Animations());
    CHECK(ClipNames(actor) ==
          (std::vector<std::string>{"run_lowready_f", "run_2_stand_f_6", "stand_2_run_f_2", "run_lowready_f"}));
}

TEST("transitions: what can follow a clip, its set's first")
{
    const auto actor = Actor({Clip("run_lowready_f", 0, 3000)});
    const auto candidates = TransitionLogic::ChainCandidates(actor, 0, Person(Set::LowReady), Shipped(), Animations(),
                                                             nullptr, nullptr, 60);
    CHECK(!candidates.empty() && candidates.front().animation == "run_2_stand_f_6" && candidates.front().table);
    CHECK(!candidates.empty() && candidates.front().label == "Stop");
    const auto has = [&](std::string_view name) {
        return std::ranges::find(candidates, name, &TransitionLogic::Candidate::animation) != candidates.end();
    };
    CHECK(has("ai_run_lowready_f_turn_90_l") && has("run_2_crouch_f"));
    // none starts other than running
    CHECK(!has("stand_2_run_f_2") && !has("exposed_stand_2_crouch"));
    // the catalog's others after the table's
    const auto other = std::ranges::find_if(candidates, [](const auto& candidate) { return !candidate.table; });
    CHECK(other != candidates.end());
    CHECK(std::all_of(other, candidates.end(), [](const auto& candidate) { return !candidate.table; }));
    // a dog's
    const auto dog = Actor({Clip("german_shepherd_run", 0, 3000)});
    const auto dogs = TransitionLogic::ChainCandidates(dog, 0, ActorLogic::Body::Dog(ActorLogic::DogBreed::Shepherd),
                                                       Shipped(), Animations(), nullptr, nullptr, 10);
    CHECK(!dogs.empty() && dogs.front().animation == "german_shepherd_run_stop");
}

TEST("transitions: what a transition does, in words")
{
    const State standStop{Posture::Stand, Movement::Stop}, standRun{Posture::Stand, Movement::Run};
    const State crouchStop{Posture::Crouch, Movement::Stop}, proneStop{Posture::Prone, Movement::Stop};
    CHECK(TransitionLogic::Describe(standRun, standStop, 0.0f) == "Stop");
    CHECK(TransitionLogic::Describe(standRun, standStop, -90.0f) == "Stop, turning right 90 deg");
    CHECK(TransitionLogic::Describe(standStop, standRun, 0.0f) == "Set off running");
    CHECK(TransitionLogic::Describe(standStop, standStop, 135.0f) == "Turn left 135 deg");
    CHECK(TransitionLogic::Describe(standRun, standRun, -180.0f) == "Turn right 180 deg, running");
    CHECK(TransitionLogic::Describe(standStop, crouchStop, 0.0f) == "Crouch");
    CHECK(TransitionLogic::Describe(crouchStop, standRun, 0.0f) == "Stand up and set off running");
    CHECK(TransitionLogic::Describe(proneStop, crouchStop, 0.0f) == "Crouch");
    CHECK(TransitionLogic::Describe(standStop, proneStop, 0.0f) == "Go prone");
}

TEST("transitions: a turn faces the way the game turns its AI (held, then steadily from exit_align to code_move)")
{
    // a running turn going 48 forward, then 88 to the left, turning 90 from 0.2 to 0.6 of its play
    ActorLogic::RootMotion own;
    constexpr int STEPS = 40;
    for (int i = 0; i <= STEPS; ++i)
    {
        const auto t = static_cast<float>(i) / STEPS;
        const auto turned = 90.0f * std::clamp((t - 0.2f) / 0.4f, 0.0f, 1.0f);
        own.samples.emplace_back(48.0f * std::min(t / 0.6f, 1.0f), 88.0f * std::max(t - 0.6f, 0.0f) / 0.4f, turned);
    }
    const std::vector<Types::ActorNotetrack> notetracks{{"exit_align", 0.4f}, {"code_move", 0.6f}, {"end", 1.0f}};
    const auto turned = TransitionLogic::GetTurnMotion(&own, notetracks, 100.0f);
    CHECK(turned.samples.size() == own.samples.size());
    // held to exit_align, steadily to the turn by code_move, then held
    CHECK_NEAR(ActorLogic::GetRootMotion(turned, 0.3f).z, 0.0f, 1e-3f);
    CHECK_NEAR(ActorLogic::GetRootMotion(turned, 0.4f).z, 0.0f, 1e-3f);
    CHECK_NEAR(ActorLogic::GetRootMotion(turned, 0.5f).z, 50.0f, 1e-3f);
    CHECK_NEAR(ActorLogic::GetRootMotion(turned, 0.6f).z, 100.0f, 1e-3f);
    CHECK_NEAR(ActorLogic::GetRootMotion(turned, 1.0f).z, 100.0f, 1e-3f);
    // the root's travel the way the actor faces: the last stretch (88 along the animation's own left, which it faces
    // 90 to) goes 100 to the left of the start
    const auto end = ActorLogic::GetRootMotion(turned, 1.0f), atCodeMove = ActorLogic::GetRootMotion(turned, 0.6f);
    const auto last = glm::vec2(end - atCodeMove);
    CHECK_NEAR(glm::length(last), 88.0f, 0.1f);
    CHECK_NEAR(glm::degrees(std::atan2(last.y, last.x)), 100.0f, 0.1f);
    // as far as its animation turns: its own turn
    const auto same = TransitionLogic::GetTurnMotion(&own, notetracks, 90.0f);
    CHECK_NEAR(ActorLogic::GetRootMotion(same, 1.0f).z, 90.0f, 1e-3f);

    // a start (setting off from a stop, exit_node.gsc) turns as its animation does from its first frame, the rest
    // steadily over the part of it that turns, exit_align or not (it held the way it faced, then turned the whole way
    // between exit_align and code_move: the user's actor turned on the spot as its run started, 2026-10-28)
    const auto start = TransitionLogic::GetTurnMotion(&own, notetracks, 100.0f, true);
    CHECK_NEAR(ActorLogic::GetRootMotion(start, 0.15f).z, 0.0f, 1e-3f);
    CHECK_NEAR(ActorLogic::GetRootMotion(start, 0.3f).z, 22.5f + 2.5f, 0.5f);
    CHECK_NEAR(ActorLogic::GetRootMotion(start, 0.4f).z, 45.0f + 5.0f, 0.5f);
    CHECK_NEAR(ActorLogic::GetRootMotion(start, 1.0f).z, 100.0f, 1e-3f);
    const auto own90 = TransitionLogic::GetTurnMotion(&own, notetracks, 90.0f, true);
    CHECK(own90.samples.size() == own.samples.size());
    for (std::size_t i = 0; i < own90.samples.size() && i < own.samples.size(); ++i)
        CHECK(glm::distance(own90.samples[i], own.samples[i]) < 1e-3f);

    // without exit_align: its own turn, and the rest steadily over the part of it that turns
    const auto own2 = TransitionLogic::GetTurnMotion(&own, {}, 100.0f);
    CHECK_NEAR(ActorLogic::GetRootMotion(own2, 0.15f).z, 0.0f, 1e-3f);
    CHECK_NEAR(ActorLogic::GetRootMotion(own2, 0.45f).z, 56.25f + 6.25f, 0.5f);
    CHECK_NEAR(ActorLogic::GetRootMotion(own2, 1.0f).z, 100.0f, 1e-3f);

    // MP's, which the game turns in code: steadily up to loop_begin, staying where it is
    const std::vector<Types::ActorNotetrack> mp{{"loop_begin", 0.5f}, {"end", 1.0f}};
    const auto code = TransitionLogic::GetTurnMotion(nullptr, mp, -90.0f);
    CHECK_NEAR(ActorLogic::GetRootMotion(code, 0.25f).z, -45.0f, 0.5f);
    CHECK_NEAR(ActorLogic::GetRootMotion(code, 0.5f).z, -90.0f, 1e-3f);
    CHECK_NEAR(ActorLogic::GetRootMotion(code, 1.0f).z, -90.0f, 1e-3f);
    CHECK_NEAR(glm::length(glm::vec2(ActorLogic::GetRootMotion(code, 1.0f))), 0.0f, 1e-4f);
}

TEST("transitions: an actor turned by a transition faces as far as it turns, after it too")
{
    // an MP turn on the spot (no root motion of its own) asked to turn 100 to the left
    auto actor = Actor({Clip("pb_stand_alert", 0, 1000)});
    const auto duration = Ticks(Shipped().Find("pb_turn_90left")->Duration());
    actor.clips.push_back({.animation = "pb_turn_90left",
                           .start = 1000,
                           .length = duration,
                           .loop = false,
                           .blendIn = 170,
                           .moves = false,
                           .chain = Types::ActorChain::Join,
                           .turn = 100.0f});
    actor.clips.push_back(Clip("pb_stand_alert", 1000 + duration, 3000));
    actor.yaw = 10.0f;
    ActorLogic::Normalize(actor);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 900, Animations()).yaw, 10.0f, 1e-3f);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 1000 + duration + 500, Animations()).yaw, 110.0f, 1e-2f);
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 5000, Animations()).yaw, 110.0f, 1e-2f);
    // turning steadily, by its blend in at first
    float last = 10.0f, most = 0.0f;
    for (std::uint32_t tick = 1000; tick <= 1000 + duration; ++tick)
    {
        const auto yaw = ActorLogic::EvaluatePlacement(actor, tick, Animations()).yaw;
        most = std::max(most, std::abs(std::remainder(yaw - last, 360.0f)));
        last = yaw;
    }
    CHECK(most < 1.0f);
    // staying in place without
    actor.clips[1].rootMotion = false;
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, 5000, Animations()).yaw, 10.0f, 1e-3f);
}

TEST("transitions: scenes keep the chain, the turn, the movement set and Auto transitions; old scenes have none")
{
    ActorLogic::Scene scene;
    auto actor = Actor({Clip("run_lowready_f", 0, 3000)});
    actor.clips.push_back({.animation = "run_2_stand_90l",
                           .start = 3000,
                           .length = 700,
                           .loop = false,
                           .chain = Types::ActorChain::Turn,
                           .turn = 95.0f,
                           .corner = 2});
    actor.movementSet = Set::Cqb;
    actor.autoTransitions = true;
    ActorLogic::Normalize(actor);
    scene.actors.push_back(actor);
    const auto read = ActorLogic::SceneFromJson(ActorLogic::SceneToJson(scene, "demo"), 10000);
    CHECK(read.actors.size() == 1);
    if (read.actors.size() == 1)
    {
        CHECK(read.actors[0].clips == actor.clips);
        CHECK(read.actors[0].movementSet == Set::Cqb && read.actors[0].autoTransitions);
    }
    // a scene from before: the players' set, no Auto transitions, clips of the user's
    auto json = ActorLogic::SceneToJson(scene, "demo");
    for (auto& node : json["actors"])
    {
        node.erase("movementSet");
        node.erase("autoTransitions");
        for (auto& clip : node["clips"])
        {
            clip.erase("chain");
            clip.erase("turn");
            clip.erase("corner");
        }
    }
    const auto old = ActorLogic::SceneFromJson(json, 10000);
    CHECK(old.actors.size() == 1 && old.actors[0].movementSet == Set::Players && !old.actors[0].autoTransitions);
    CHECK(old.actors.size() == 1 && std::ranges::all_of(old.actors[0].clips, [](const Types::ActorClip& clip) {
              return clip.chain == Types::ActorChain::None && !clip.turn && !clip.corner;
          }));
    // new actors join and walk as their bodies were made for
    const auto start = ActorLogic::MakeStart(ActorLogic::Start::Standing, {.idle = "casual_stand_idle"}, 0, 5000, "");
    CHECK(start.autoTransitions && start.movementSet == Set::Auto);
}

TEST("transitions: how far apart two poses are, and whether a join of them shows over its blend")
{
    TransitionLogic::Pose a{}, b{};
    a[0] = glm::vec3(0, 0, 40);
    a[3] = glm::vec3(0, 0, 70);
    b[0] = glm::vec3(3, 4, 40);  // 5 units off
    b[3] = glm::vec3(0, 0, 70);
    b[5] = glm::vec3(10, 0, 20);  // a bone only one has doesn't count
    const auto distance = TransitionLogic::GetPoseDistance(a, b);
    CHECK(distance && std::abs(*distance - std::sqrt(12.5f)) < 1e-4f);
    CHECK(!TransitionLogic::GetPoseDistance(a, TransitionLogic::Pose{}));
    // 9.5 units apart (casual_stand_idle to stand_2_run_f_2): blended over the game's 0.2 s, 2.4 units in 50 ms
    CHECK(TransitionLogic::IsCleanJoin(9.5f, 200));
    CHECK(!TransitionLogic::IsCleanJoin(9.5f, 50));
    CHECK(!TransitionLogic::IsCleanJoin(9.5f, 0));
    CHECK(TransitionLogic::IsCleanJoin(6.0f, 0));
}
