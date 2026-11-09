#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/AnimationCatalogLogic.hpp"
#include "Components/TransitionLogic.hpp"

#include <set>

using namespace IWXMVM;

// The actor workflow (proposals 1a, 3, 7, 8, 9): the path owns movement through looks,
// one-offs hang on anchors and are put back where those have them as the path is planned again, the pace keeps the
// arrival, new points take the gait shown, waits stop and start, and scenes from before looks convert

namespace
{
    using Set = Types::ActorMovementSet;
    using Gait = Types::ActorGait;

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

    // the root motion of some transitions and cycles as the game measures them
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
                    motion->samples.emplace_back(sample[0].get<float>(), sample[1].get<float>(),
                                                 sample[2].get<float>());
                read[name] = std::move(motion);
            }
            return read;
        }();
        return tracks;
    }

    // what the game knows once it has built the animations (the catalog's numbers and states, the motion of a play),
    // or before it has measured them (the catalog's motion only)
    ActorLogic::AnimationLookup Animations(bool measured = true)
    {
        return [measured](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            const auto entry = Shipped().Find(name);
            if (!entry)
                return std::nullopt;
            ActorLogic::AnimationInfo info{entry->Duration(),
                                           measured ? std::optional(glm::vec2(entry->motion)) : std::nullopt,
                                           entry->notetracks, entry->kind, entry->family};
            info.death = entry->death;
            info.alert = entry->alert;
            info.stance = AnimationCatalogLogic::GetStance(entry->from.posture);
            info.delta = entry->delta;
            info.loop = entry->loop;
            if (entry->delta)
                info.catalogMotion = glm::vec2(entry->motion);
            if (entry->from != Types::AnimationState{})
                info.from = entry->from;
            if (entry->to != Types::AnimationState{})
                info.to = entry->to;
            if (entry->turn != 0.0f)
                info.turn = entry->turn;
            info.footfalls = entry->footfalls;
            if (const auto track = Tracks().find(name);
                measured && track != Tracks().end() && track->second->samples.size() >= 2)
                info.rootMotion = track->second;
            return info;
        };
    }

    bool Has(std::string_view name)
    {
        return Shipped().Find(name) != nullptr;
    }

    ActorLogic::Body Person(Set set)
    {
        ActorLogic::Body body;
        body.set = set;
        return body;
    }

    float Speed(std::string_view animation, const ActorLogic::AnimationLookup& animations = Animations())
    {
        return ActorLogic::GetAnimationSpeed(animation, animations);
    }

    // its walk fitted to its path and planned (the start, turns and stop), its one-offs taken out meanwhile and put
    // back where their anchors have them, those of `anchored` anchored where they are first: as
    // Components::Actors::FitOnlyWalk does it
    void Replan(Types::Actor& actor, const ActorLogic::Body& body, const std::set<std::uint32_t>& anchored = {})
    {
        const auto animations = Animations();
        auto oneOffs = ActorLogic::TakeOutOneOffs(actor, animations);
        TransitionLogic::UnplanPath(actor);
        ActorLogic::FitOnlyWalk(actor, animations);
        TransitionLogic::PlanPath(actor, body, animations);
        const auto timing = ActorLogic::GetPathTiming(actor, animations);
        for (auto& oneOff : oneOffs)
            if (anchored.contains(oneOff.clip.id))
                ActorLogic::Anchor(oneOff.clip, timing, oneOff.death);
        ActorLogic::PutBackOneOffs(actor, std::move(oneOffs), animations);
    }

    // An actor of a movement set standing a second, then going along a path at a gait (Locomotion Auto, Auto
    // transitions: fitted and planned), then standing, as the Actors tab makes one
    Types::Actor Runner(std::vector<glm::vec3> path, Set set, Gait gait)
    {
        const auto body = Person(set);
        Types::Actor actor;
        actor.id = 1;
        actor.movement = Types::ActorMovement::Path;
        actor.path = std::move(path);
        actor.locomotion = Types::ActorLocomotion::Auto;
        actor.movementSet = set;
        actor.autoTransitions = true;
        const auto idle = ActorLogic::FindLocomotionAnimation(body, Types::ActorStance::Stand, ActorLogic::Gait::Idle,
                                                              ActorLogic::Heading::Forward, Has);
        actor.clips = {
            {.animation = idle, .start = 0, .length = 1000, .moves = false},
            {.animation = ActorLogic::GetGaitAnimation(body, gait, false, Has), .start = 1000, .length = 1000},
            {.animation = idle, .start = 2000, .length = 40000, .moves = false}};
        ActorLogic::Normalize(actor);
        actor.stretches.assign(actor.path.size(), {.gait = gait});
        ActorLogic::ResolveStretches(actor, body, Has);
        Replan(actor, body);
        return actor;
    }

    // a path round a right angle corner: 600 units on, 500 to the left
    std::vector<glm::vec3> Corner()
    {
        return {glm::vec3(0, 0, 0), glm::vec3(600, 0, 0), glm::vec3(600, 500, 0)};
    }

    ActorLogic::PathTiming Timing(const Types::Actor& actor)
    {
        return ActorLogic::GetPathTiming(actor, Animations());
    }

    const Types::ActorClip* Find(const Types::Actor& actor, std::string_view animation)
    {
        const auto found = std::ranges::find(actor.clips, animation, &Types::ActorClip::animation);
        return found != actor.clips.end() ? &*found : nullptr;
    }

    std::size_t IndexOf(const Types::Actor& actor, std::string_view animation)
    {
        return static_cast<std::size_t>(std::ranges::find(actor.clips, animation, &Types::ActorClip::animation) -
                                        actor.clips.begin());
    }

    bool Near(float a, float b, float within)
    {
        return std::abs(a - b) <= within;
    }

    // the clips of the user's (not the path's transitions) that walk it, as the path's own walk is one
    std::size_t CountWalks(const Types::Actor& actor)
    {
        return TransitionLogic::GetPathWalks(actor, Animations()).size();
    }
}  // namespace

TEST("workflow: a look goes as fast as its animation (measured, else as the catalog has it) at the pace, per body")
{
    // (an AI run of the campaign's, which isn't a set's own: before the game measured it, as far as the catalog has a
    // play move it)
    const auto lean = "ai_run_lowready_f_lean_l";
    CHECK(Speed(lean, Animations(false)) > 160.0f && Speed(lean, Animations(false)) < 180.0f);
    CHECK(Near(Speed(lean), Speed(lean, Animations(false)), 0.1f));
    // the gait it is a look of: its locomotion's, else the body's nearest in speed
    const auto low = Person(Set::LowReady), players = Person(Set::Players);
    CHECK(ActorLogic::GetLookGait("pb_sprint", players, Animations()) == std::pair(Gait::Sprint, false));
    CHECK(ActorLogic::GetLookGait(lean, low, Animations()) == std::pair(Gait::Run, false));
    CHECK(ActorLogic::GetLookGait("walk_lowready_f", low, Animations()) == std::pair(Gait::Walk, false));
    CHECK(!ActorLogic::GetLookGait("casual_stand_idle", low, Animations()));

    // the whole path: the actor's look of its gait, which the stretch goes as fast as, at the pace
    auto actor = Runner(Corner(), Set::LowReady, Gait::Run);
    actor.pace = 1.1f;
    CHECK(ActorLogic::SetLook(actor, std::nullopt, lean, low, Animations(), Has) == std::pair(Gait::Run, false));
    CHECK(actor.looks.size() == 1 && ActorLogic::GetLook(actor, actor.stretches[0]) == lean);
    CHECK(Near(ActorLogic::GetStretchSpeed(actor, actor.stretches[1], Animations()), Speed(lean) * 1.1f, 0.5f));
    // a stretch's own, of another gait: that stretch goes at it, the other as it did
    auto mixed = Runner(Corner(), Set::Players, Gait::Run);
    CHECK(ActorLogic::SetLook(mixed, 1, "pb_sprint", players, Animations(), Has) == std::pair(Gait::Sprint, false));
    CHECK(mixed.stretches[0].gait == Gait::Run && mixed.stretches[0].look.empty());
    CHECK(mixed.stretches[1].gait == Gait::Sprint && mixed.stretches[1].look == "pb_sprint" && mixed.looks.empty());
    CHECK(Near(ActorLogic::GetStretchSpeed(mixed, mixed.stretches[1], Animations()), Speed("pb_sprint"), 0.5f));
}

TEST("workflow: the locomotion plays a stretch's look going forward, at the rate the pace asks")
{
    const auto body = Person(Set::LowReady);
    auto actor = Runner({glm::vec3(0, 0, 0), glm::vec3(1500, 0, 0)}, Set::LowReady, Gait::Run);
    ActorLogic::SetLook(actor, std::nullopt, "ai_run_lowready_f_lean_l", body, Animations(), Has);
    actor.pace = 0.9f;
    Replan(actor, body);
    const auto timing = Timing(actor);
    CHECK(timing.start && timing.arrival);
    if (!timing.start || !timing.arrival)
        return;
    const auto middle = (*timing.start + *timing.arrival) / 2;
    const auto layers = ActorLogic::GetLayers(actor, middle, Animations(), body);
    const auto legs =
        std::ranges::find(layers.legs, std::string("ai_run_lowready_f_lean_l"), &ActorLogic::LegsSample::animation);
    CHECK(legs != layers.legs.end() && Near(legs->rate, 0.9f, 0.02f) && Near(legs->weight, 1.0f, 0.01f));
    // (as fast as the look at the pace, forward along the path)
    const auto a = ActorLogic::EvaluatePlacement(actor, middle - 500, Animations());
    const auto b = ActorLogic::EvaluatePlacement(actor, middle + 500, Animations());
    CHECK(Near(glm::distance(glm::vec2(a.position), glm::vec2(b.position)), Speed("ai_run_lowready_f_lean_l") * 0.9f,
               2.0f));
    CHECK(Near(b.position.y, 0.0f, 0.5f));
}

TEST("workflow: a swap keeps the arrival with a pace within 80-120% when one can, else the arrival moves")
{
    const auto body = Person(Set::LowReady);
    const auto fit = [&](Types::Actor& fitted) { Replan(fitted, body); };
    const auto actor = Runner(Corner(), Set::LowReady, Gait::Run);
    const auto arrival = Timing(actor).arrival;
    CHECK(arrival.has_value());
    if (!arrival)
        return;
    // a slower run: later, unless the pace takes it back
    auto slower = actor;
    ActorLogic::SetLook(slower, std::nullopt, "ai_run_lowready_f_lean_l", body, Animations(), Has);
    Replan(slower, body);
    CHECK(Timing(slower).arrival > *arrival + 200);
    const auto pace = ActorLogic::SolvePace(slower, *arrival, Animations(), fit);
    CHECK(pace && *pace >= ActorLogic::MIN_PACE && *pace <= ActorLogic::MAX_PACE);
    if (!pace)
        return;
    CHECK(Near(*pace, Speed("run_lowready_f") / Speed("ai_run_lowready_f_lean_l"), 0.03f));
    slower.pace = *pace;
    Replan(slower, body);
    const auto kept = Timing(slower).arrival;
    CHECK(kept && std::abs(static_cast<int>(*kept) - static_cast<int>(*arrival)) < 30);
    // one too slow for any pace that keeps its feet on the ground: the arrival moves
    auto wounded = actor;
    ActorLogic::SetLook(wounded, std::nullopt, "ai_run_lowready_f_wounded", body, Animations(), Has);
    Replan(wounded, body);
    const auto needed = ActorLogic::SolvePace(wounded, *arrival, Animations(), fit);
    CHECK(needed && *needed > ActorLogic::MAX_PACE);
}

TEST("workflow: one-offs taken out and put back as they were leave the clips and the Hits as they were")
{
    const auto body = Person(Set::Players);
    auto actor = Runner(Corner(), Set::Players, Gait::Run);
    const auto timing = Timing(actor);
    CHECK(timing.reached[1] && timing.arrival);
    if (!timing.reached[1] || !timing.arrival)
        return;
    // a reload as it runs (what was shown goes on after it), and a death after it arrives
    ActorLogic::PlayAt(actor, *timing.reached[1] - 800, "pt_reload_stand_auto", 2.3f, false);
    ActorLogic::AddDeath(actor, *timing.arrival + 600, "pb_stand_death_headchest_topple", 2.0f);
    const auto before = actor;
    auto oneOffs = ActorLogic::TakeOutOneOffs(actor, Animations());
    CHECK(oneOffs.size() == 2 && !oneOffs[0].death && oneOffs[1].death && oneOffs[1].hits.size() == 1);
    CHECK(!Find(actor, "pt_reload_stand_auto") && !Find(actor, "pb_stand_death_headchest_topple"));
    CHECK(CountWalks(actor) == 1);
    ActorLogic::PutBackOneOffs(actor, std::move(oneOffs), Animations());
    CHECK(actor.clips == before.clips);
    CHECK(actor.events == before.events);
}

TEST("workflow: a one-off joined by Auto transitions comes out with its joins, and is put back to be joined again")
{
    const auto body = Person(Set::LowReady);
    // (a straight run picked by the user, as scenes from before looks have them: the path's own walk isn't joined to)
    auto actor = Runner({glm::vec3(0, 0, 0), glm::vec3(1500, 0, 0)}, Set::LowReady, Gait::Run);
    TransitionLogic::UnplanPath(actor);
    actor.clips[1].animation = "ai_run_lowready_f_lean_l";
    actor.clips[1].picked = true;
    ActorLogic::FitOnlyWalk(actor, Animations());
    const auto timing = Timing(actor);
    CHECK(timing.start && timing.arrival);
    if (!timing.start || !timing.arrival)
        return;
    // an idle played as it runs, joined to the run before and after it
    const auto at = (*timing.start + *timing.arrival) / 2;
    const auto idle = ActorLogic::PlayAt(actor, at, "casual_stand_idle", 1.0f, true);
    TransitionLogic::JoinAround(actor, idle, body, Animations());
    const auto joins = [](const Types::Actor& each) {
        return std::ranges::count(each.clips, Types::ActorChain::Join, &Types::ActorClip::chain);
    };
    CHECK(joins(actor) > 0);
    auto oneOffs = ActorLogic::TakeOutOneOffs(actor, Animations());
    CHECK(oneOffs.size() == 1 && oneOffs.front().joined && joins(actor) == 0 &&
          std::ranges::count(actor.clips, std::string("ai_run_lowready_f_lean_l"), &Types::ActorClip::animation) == 1);
    const auto joined = ActorLogic::PutBackOneOffs(actor, std::move(oneOffs), Animations());
    CHECK(joined.size() == 1 && joined.front() == idle && Find(actor, "casual_stand_idle"));
    TransitionLogic::JoinAround(actor, idle, body, Animations());
    CHECK(joins(actor) > 0);
}

TEST("workflow: anchors: a death on the path's end goes with the arrival, a gesture with its point, its Hit with it")
{
    const auto body = Person(Set::Players);
    auto actor = Runner(Corner(), Set::Players, Gait::Run);
    auto timing = Timing(actor);
    CHECK(timing.reached[1] && timing.arrival);
    if (!timing.reached[1] || !timing.arrival)
        return;
    // a reload just after the corner: the point it reaches nearest; a death 0.26 s after it arrives: the end
    const auto reload = ActorLogic::PlayAt(actor, *timing.reached[1] + 300, "pt_reload_stand_auto", 2.3f, false);
    ActorLogic::AddDeath(actor, *timing.arrival + 260, "pb_stand_death_headchest_topple", 2.0f);
    const auto death =
        std::ranges::find(actor.clips, "pb_stand_death_headchest_topple", &Types::ActorClip::animation)->id;
    Replan(actor, body, {reload, death});
    auto* gesture = Find(actor, "pt_reload_stand_auto");
    auto* dies = Find(actor, "pb_stand_death_headchest_topple");
    CHECK(gesture && gesture->anchor == Types::ActorAnchor::Point && gesture->anchorPoint == 1 &&
          gesture->anchorOffset == 300);
    CHECK(dies && dies->anchor == Types::ActorAnchor::End && dies->anchorOffset == 260);

    // the second stretch walked: it gets to the end later; the reload hangs on its point, the death on the end
    ActorLogic::SetGait(actor, 1, actor.path.size(), Gait::Walk, false, body, Animations(), Has);
    Replan(actor, body);
    timing = Timing(actor);
    gesture = Find(actor, "pt_reload_stand_auto");
    dies = Find(actor, "pb_stand_death_headchest_topple");
    CHECK(timing.arrival && gesture && dies);
    if (!timing.arrival || !gesture || !dies)
        return;
    CHECK(gesture->start == *timing.reached[1] + 300);
    CHECK(dies->start == *timing.arrival + 260);
    const auto hit = std::ranges::find(actor.events, Types::ActorEventType::Hit, &Types::ActorEvent::type);
    CHECK(hit != actor.events.end() && hit->death == dies->id && hit->tick == dies->start);
    // (after the reload, the walk goes on as it was)
    const auto after = IndexOf(actor, "pt_reload_stand_auto") + 1;
    CHECK(after < actor.clips.size() && actor.clips[after].start == gesture->End() &&
          actor.clips[after].animation == actor.clips[after - 2].animation);
    // and the walk is planned as one again next time
    auto again = actor;
    Replan(again, body);
    CHECK(again.clips == actor.clips);
}

TEST("workflow: a death before the arrival cuts the walk with no held frame; one after it, it arrives, stops, stands")
{
    const auto body = Person(Set::LowReady);
    auto actor = Runner(Corner(), Set::LowReady, Gait::Run);
    const auto timing = Timing(actor);
    CHECK(timing.reached[1] && timing.arrival);
    if (!timing.reached[1] || !timing.arrival)
        return;
    // on its time, mid-run (it moves until then, the death blending in over the run)
    const auto at = (*timing.reached[1] + *timing.arrival) / 2;
    auto running = actor;
    ActorLogic::AddDeath(running, at, "ai_death_fallforward", 1.33f);
    Replan(running, body);
    // a gait changed after: still on its time
    ActorLogic::SetGait(running, 0, 1, Gait::Walk, false, body, Animations(), Has);
    Replan(running, body);
    const auto index = IndexOf(running, "ai_death_fallforward");
    CHECK(index < running.clips.size() && index + 1 == running.clips.size());
    if (index >= running.clips.size() || index == 0)
        return;
    CHECK(running.clips[index].start == at && running.clips[index].anchor == Types::ActorAnchor::Time);
    CHECK(running.clips[index - 1].End() == at);
    CHECK(TransitionLogic::GetPathWalks(running, Animations()).size() == 1);
    const auto a = ActorLogic::EvaluatePlacement(running, at - 40, Animations());
    const auto b = ActorLogic::EvaluatePlacement(running, at - 1, Animations());
    CHECK(glm::distance(glm::vec2(a.position), glm::vec2(b.position)) > 3.0f);
    CHECK(!Timing(running).arrival);

    // after the arrival: its stop, then standing until the death
    auto arriving = actor;
    ActorLogic::AddDeath(arriving, *timing.arrival + 1500, "ai_death_fallforward", 1.33f);
    Replan(arriving, body);
    const auto death = IndexOf(arriving, "ai_death_fallforward");
    CHECK(death >= 2 && death < arriving.clips.size());
    if (death < 2 || death >= arriving.clips.size())
        return;
    CHECK(std::ranges::any_of(arriving.clips.begin(), arriving.clips.begin() + static_cast<std::ptrdiff_t>(death),
                              [](const Types::ActorClip& clip) { return clip.chain == Types::ActorChain::Stop; }));
    CHECK(arriving.clips[death - 1].animation == "casual_stand_idle" &&
          arriving.clips[death - 1].End() == arriving.clips[death].start);
    CHECK(Timing(arriving).arrival.has_value());
}

TEST("workflow: new points take the gait shown: the last stretch walked, or the gait picked for the next one")
{
    const auto body = Person(Set::Players);
    auto actor = Runner(Corner(), Set::Players, Gait::Run);
    ActorLogic::SetGait(actor, 0, actor.path.size(), Gait::Sprint, false, body, Animations(), Has);
    // (one past the end goes on as the last stretch walked)
    auto added = ActorLogic::InsertWaypoint(actor, actor.path.size() - 1);
    CHECK(added == 3 && actor.stretches.size() == 4 && actor.stretches[2].gait == Gait::Sprint);
    // picked while placing: the stretch to it at that gait (and the last point's goes as it does)
    added = ActorLogic::InsertWaypoint(actor, actor.path.size() - 1, std::pair(Gait::Walk, false));
    CHECK(added == 4 && actor.stretches[3].gait == Gait::Walk && actor.stretches[2].gait == Gait::Sprint);
    CHECK(actor.stretches.back().gait == Gait::Walk);
    // (with its animation, as fast as a walk goes)
    CHECK(Near(ActorLogic::GetStretchSpeed(actor, actor.stretches[3], Animations()),
               Speed("pb_stand_shoot_walk_forward"), 0.5f));
    // a point put in between: the stretch to it at the gait picked, the one from it as the stretch it was put in
    added = ActorLogic::InsertWaypoint(actor, 0, std::pair(Gait::Run, false));
    CHECK(added == 1 && actor.stretches[0].gait == Gait::Run && actor.stretches[1].gait == Gait::Sprint);
}

TEST("workflow: a wait's stop, idle and start count in the path's times, and its point stays sharp")
{
    const auto body = Person(Set::LowReady);
    auto actor = Runner(Corner(), Set::LowReady, Gait::Run);
    const auto without = Timing(actor);
    actor.stretches[1].wait = 2000;
    Replan(actor, body);
    const auto with = Timing(actor);
    CHECK(without.arrival && with.arrival && with.reached[1] && with.left[1]);
    if (!without.arrival || !with.arrival || !with.reached[1] || !with.left[1])
        return;
    // later by the wait, and by stopping and setting off again (less the turn it no longer runs)
    CHECK(*with.arrival > *without.arrival + 2000);
    CHECK(*with.left[1] == *with.reached[1] + 2000);
    // it stands at the point through the wait
    const auto idle = std::ranges::find_if(actor.clips, [](const Types::ActorClip& clip) {
        return clip.chain == Types::ActorChain::Stop && clip.animation == "casual_stand_idle";
    });
    CHECK(idle != actor.clips.end());
    if (idle != actor.clips.end())
        CHECK(
            glm::distance(
                glm::vec2(ActorLogic::EvaluatePlacement(actor, idle->start + idle->length / 2, Animations()).position),
                glm::vec2(600, 0)) < 3.0f);
    // the wait taken off: one run again
    actor.stretches[1].wait = 0;
    Replan(actor, body);
    CHECK(std::ranges::none_of(actor.clips, [](const Types::ActorClip& clip) {
        return clip.chain == Types::ActorChain::Stop && clip.animation == "casual_stand_idle";
    }));
    const auto again = Timing(actor);
    CHECK(again.arrival && std::abs(static_cast<int>(*again.arrival) - static_cast<int>(*without.arrival)) < 30);
}

TEST("workflow: scenes from before looks: a picked walk over the path becomes the look of the stretches it covered")
{
    const auto body = Person(Set::LowReady);
    // the whole walk picked: the actor's look of its gait, the gait the look's, the same way along the path
    auto actor = Runner({glm::vec3(0, 0, 0), glm::vec3(900, 0, 0)}, Set::LowReady, Gait::Walk);
    TransitionLogic::UnplanPath(actor);
    auto& walk = actor.clips[1];
    walk.animation = "ai_run_lowready_f_lean_l";
    walk.picked = true;
    ActorLogic::FitOnlyWalk(actor, Animations());
    const auto before = actor;
    CHECK(ActorLogic::ConvertPickedWalks(actor, body, Animations(), Has));
    CHECK((actor.looks == std::vector<Types::ActorLook>{{Gait::Run, false, "ai_run_lowready_f_lean_l"}}));
    CHECK(actor.stretches[0].gait == Gait::Run && actor.stretches[0].look.empty());
    CHECK(!actor.clips[1].picked && actor.clips[1].animation == "run_lowready_f");
    for (const auto tick : {1200u, 2500u, 4000u, 5500u})
        CHECK(glm::distance(ActorLogic::EvaluatePlacement(before, tick, Animations()).position,
                            ActorLogic::EvaluatePlacement(actor, tick, Animations()).position) < 1.0f);
    CHECK(!ActorLogic::ConvertPickedWalks(actor, body, Animations(), Has));

    // a piece after a corner picked: the look of the stretches it covered only
    auto corner = Runner(Corner(), Set::LowReady, Gait::Run);
    const auto piece = std::ranges::find(corner.clips, Types::ActorChain::Walk, &Types::ActorClip::chain);
    CHECK(piece != corner.clips.end());
    if (piece == corner.clips.end())
        return;
    piece->animation = "ai_run_lowready_f_lean_l";
    piece->picked = true;
    CHECK(ActorLogic::ConvertPickedWalks(corner, body, Animations(), Has));
    CHECK(corner.looks.empty() && corner.stretches[0].look.empty() &&
          corner.stretches[1].look == "ai_run_lowready_f_lean_l" && corner.stretches[1].gait == Gait::Run);
}

TEST("workflow: a path beside another is as far from each of its stretches, corners mitred")
{
    const auto path = Corner();
    const auto right = ActorLogic::OffsetPath(path, -48.0f);
    CHECK(right.size() == path.size());
    if (right.size() != path.size())
        return;
    // (the first stretch goes along +x, the second along +y: right of them is -y, then +x)
    CHECK(glm::distance(right[0], glm::vec3(0, -48, 0)) < 0.01f);
    CHECK(glm::distance(right[1], glm::vec3(648, -48, 0)) < 0.01f);
    CHECK(glm::distance(right[2], glm::vec3(648, 500, 0)) < 0.01f);
    // to the left, inside the corner
    const auto left = ActorLogic::OffsetPath(path, 48.0f);
    CHECK(glm::distance(left[1], glm::vec3(552, 48, 0)) < 0.01f);
    // a straight path, a point in the middle of it
    const auto straight =
        ActorLogic::OffsetPath({glm::vec3(0, 0, 5), glm::vec3(100, 0, 5), glm::vec3(300, 0, 5)}, 48.0f);
    CHECK(glm::distance(straight[1], glm::vec3(100, 48, 5)) < 0.01f);
}

TEST("workflow: scenes keep looks and anchors; those without them read as before")
{
    auto actor = Runner(Corner(), Set::LowReady, Gait::Run);
    actor.looks.push_back({Gait::Run, false, "ai_run_lowready_f_lean_l"});
    actor.stretches[1].look = "combat_run_fast_3";
    actor.clips.back().anchor = Types::ActorAnchor::Point;
    actor.clips.back().anchorPoint = 1;
    actor.clips.back().anchorOffset = -250;
    const auto json = ActorLogic::SceneToJson({{actor}, ""}, "demo");
    const auto read = ActorLogic::SceneFromJson(json, 60000);
    CHECK(read.actors.size() == 1 && read.actors[0].looks == actor.looks &&
          read.actors[0].stretches[1].look == "combat_run_fast_3");
    CHECK(read.actors[0].clips.back().anchor == Types::ActorAnchor::Point &&
          read.actors[0].clips.back().anchorPoint == 1 && read.actors[0].clips.back().anchorOffset == -250);
    CHECK(json.dump().find("\"anchor\":0") == std::string::npos);
    // (without them: on their times, no looks)
    auto plain = json;
    plain["actors"][0].erase("looks");
    for (auto& clip : plain["actors"][0]["clips"])
        clip.erase("anchor");
    const auto old = ActorLogic::SceneFromJson(plain, 60000);
    CHECK(old.actors[0].looks.empty() && old.actors[0].clips.back().anchor == Types::ActorAnchor::Time);
}

// Joins: where the look changes, the legs blend and the speed eases over the join's
// blend, centred on the point (or before, or after it), Auto one step of the slower look

namespace
{
    // intro_cinematic's walk on a straight path: running with
    // ch_khe_e1b_troopssprint_2 (211 a second) to point 1, 600 units on, then walking with walk_cqb_f (98 a second)
    // of its own, 400 more
    Types::Actor TwoLooks()
    {
        const auto body = Person(Set::Players);
        auto actor = Runner({glm::vec3(0, 0, 0), glm::vec3(600, 0, 0), glm::vec3(1000, 0, 0)}, Set::Players, Gait::Run);
        ActorLogic::SetLook(actor, std::nullopt, "ch_khe_e1b_troopssprint_2", body, Animations(), Has);
        ActorLogic::SetLook(actor, 1, "walk_cqb_f", body, Animations(), Has);
        Replan(actor, body);
        return actor;
    }

    // how fast the actor goes along its path at a tick, units a second (over the 20 ms around it)
    float SpeedAt(const Types::Actor& actor, std::uint32_t tick)
    {
        return (ActorLogic::GetPathProgress(actor, tick + 10, Animations()) -
                ActorLogic::GetPathProgress(actor, tick - 10, Animations())) /
               0.02f;
    }

    // the share of the legs an animation of the locomotion has at a tick
    float LegsShare(const Types::Actor& actor, std::uint32_t tick, std::string_view animation)
    {
        float share = 0.0f;
        for (const auto& legs : ActorLogic::GetLayers(actor, tick, Animations(), Person(Set::Players)).legs)
            if (!legs.clip && legs.animation == animation)
                share += legs.weight;
        return share;
    }

    int Off(std::optional<std::uint32_t> a, std::optional<std::uint32_t> b)
    {
        return a && b ? std::abs(static_cast<int>(*a) - static_cast<int>(*b)) : 100000;
    }

    // the time into an animation of the locomotion its legs sample has at a tick, none when it isn't shown
    std::optional<float> LegsTime(const Types::Actor& actor, std::uint32_t tick, std::string_view animation,
                                  const ActorLogic::AnimationLookup& animations = Animations())
    {
        for (const auto& legs : ActorLogic::GetLayers(actor, tick, animations, Person(Set::Players)).legs)
            if (!legs.clip && legs.animation == animation)
                return legs.time;
        return std::nullopt;
    }

    // the foot an animation came down on last at a time into it, how far it is from there to its next footfall (0 to
    // 1), and the foot that comes down next
    struct Step
    {
        bool lastLeft = false;
        float through = 0.0f;
        bool nextLeft = false;
    };
    Step StepAt(std::string_view animation, float time)
    {
        const auto info = Animations()(std::string(animation));
        const auto feet = ActorLogic::GetFootfalls(*info);
        const auto phase = time / info->duration - std::floor(time / info->duration);
        std::size_t last = feet.size() - 1;
        for (std::size_t i = 0; i < feet.size(); ++i)
            if (feet[i].time <= phase)
                last = i;
        const auto& next = feet[(last + 1) % feet.size()];
        const auto wrap = [](float v) { return v - std::floor(v); };
        return {feet[last].left, wrap(phase - feet[last].time) / wrap(next.time - feet[last].time), next.left};
    }
}  // namespace

TEST("joins: a join where the look changes, none where nothing does; Auto is one step of the slower look")
{
    auto actor = TwoLooks();
    CHECK(actor.stretches[0].animation == "ch_khe_e1b_troopssprint_2" && actor.stretches[1].animation == "walk_cqb_f");
    CHECK(!ActorLogic::HasJoin(actor, 0) && ActorLogic::HasJoin(actor, 1) && !ActorLogic::HasJoin(actor, 2));
    // walk_cqb_f's six footsteps in 2.07 s: 0.34 s apart
    CHECK(ActorLogic::GetAutoJoinBlend(actor, 1, Animations()) == 340);
    CHECK(ActorLogic::GetJoinBlend(actor, 1, Animations()) == 340);
    actor.stretches[1].blend = 600;
    CHECK(ActorLogic::GetJoinBlend(actor, 1, Animations()) == 600);
    // a stretch of its own pace has one too
    auto paced = Runner({glm::vec3(0, 0, 0), glm::vec3(600, 0, 0), glm::vec3(1000, 0, 0)}, Set::Players, Gait::Run);
    CHECK(!ActorLogic::HasJoin(paced, 1));
    CHECK(!Timing(paced).joins[1]);
    paced.stretches[1].pace = 0.9f;
    CHECK(ActorLogic::HasJoin(paced, 1));
    // the footsteps a look's notetracks have, the right foot's first; the players' runs have none, but the catalog's
    const auto run = Animations()("pb_combatrun_forward_loop");
    CHECK(run && ActorLogic::GetFootfalls(*run).size() == 6 && run->footfalls.size() == 6);
    const auto walk = Animations()("walk_cqb_f");
    CHECK(walk && ActorLogic::GetFootfalls(*walk).size() == 6 && !ActorLogic::GetFootfalls(*walk).front().left);
}

TEST("joins: the legs blend over the join's blend, centred on the point, the speed midway there")
{
    auto actor = TwoLooks();
    auto timing = Timing(actor);
    CHECK(timing.reached[1] && timing.joins[1]);
    if (!timing.reached[1] || !timing.joins[1])
        return;
    const auto reached = *timing.reached[1];
    const auto [start, end] = *timing.joins[1];
    CHECK(end - start == 340);
    CHECK(std::abs(static_cast<int>(start + end) / 2 - static_cast<int>(reached)) <= 20);
    // the legs: all the run before, half and half at the point, all the walk after
    CHECK(LegsShare(actor, start - 20, "ch_khe_e1b_troopssprint_2") > 0.99f);
    CHECK_NEAR(LegsShare(actor, reached, "walk_cqb_f"), 0.5, 0.1);
    CHECK_NEAR(LegsShare(actor, reached, "ch_khe_e1b_troopssprint_2"), 0.5, 0.1);
    CHECK(LegsShare(actor, end + 10, "walk_cqb_f") > 0.99f);
    // the speed: the run's, midway at the point, the walk's
    CHECK_NEAR(SpeedAt(actor, start - 50), Speed("ch_khe_e1b_troopssprint_2"), 1.5);
    CHECK_NEAR(SpeedAt(actor, reached), (Speed("ch_khe_e1b_troopssprint_2") + Speed("walk_cqb_f")) / 2.0f, 12.0);
    CHECK_NEAR(SpeedAt(actor, end + 50), Speed("walk_cqb_f"), 1.5);
    // a blend of 0.6 s: the legs half way 0.3 s in
    actor.stretches[1].blend = 600;
    Replan(actor, Person(Set::Players));
    timing = Timing(actor);
    CHECK(timing.joins[1] && timing.joins[1]->second - timing.joins[1]->first == 600);
    if (timing.joins[1])
    {
        CHECK_NEAR(LegsShare(actor, timing.joins[1]->first + 300, "walk_cqb_f"), 0.5, 0.05);
        CHECK(LegsShare(actor, timing.joins[1]->first + 590, "walk_cqb_f") > 0.97f);
    }
}

TEST("joins: the incoming look comes in on the next foot; one without footfalls (or feet not matched) as before")
{
    auto actor = TwoLooks();
    const auto timing = Timing(actor);
    CHECK(timing.joins[1]);
    if (!timing.joins[1])
        return;
    // (a moment into the blend, both shown)
    const auto start = timing.joins[1]->first + 10;
    const auto outgoing = LegsTime(actor, start, "ch_khe_e1b_troopssprint_2");
    const auto incoming = LegsTime(actor, start, "walk_cqb_f");
    CHECK(outgoing && incoming);
    if (!outgoing || !incoming)
        return;
    // the other foot next, as far through the step
    const auto out = StepAt("ch_khe_e1b_troopssprint_2", *outgoing), in = StepAt("walk_cqb_f", *incoming);
    CHECK(out.nextLeft == in.nextLeft && in.lastLeft != in.nextLeft);
    CHECK_NEAR(in.through, out.through, 0.05);
    // and on from there at its rate (faster than its own while the speed eases down to it)
    const auto later = LegsTime(actor, start + 600, "walk_cqb_f");
    CHECK(later && *later - *incoming > 0.6f && *later - *incoming < 0.9f);

    // feet as they fall: as far into it as its rates have played it since the walk set off
    auto loose = actor;
    loose.stretches[1].matchFeet = false;
    const auto unmatched = LegsTime(loose, start, "walk_cqb_f");
    CHECK(unmatched && std::abs(*unmatched - *incoming) > 1e-3f);
    // (a step later the same distance on: only where it started differs)
    const auto unmatchedLater = LegsTime(loose, start + 600, "walk_cqb_f");
    CHECK(unmatchedLater && later && std::abs((*unmatchedLater - *unmatched) - (*later - *incoming)) < 1e-3f);
    // a look without footfalls: the same as feet not matched
    const ActorLogic::AnimationLookup plain = [](const std::string& name) {
        auto info = Animations()(name);
        if (info && name == "walk_cqb_f")
        {
            info->notetracks = {};
            info->footfalls = {};
        }
        return info;
    };
    const auto plainTime = LegsTime(actor, start, "walk_cqb_f", plain);
    CHECK(plainTime && std::abs(*plainTime - *unmatched) < 1e-4f);
    // the players' walk has footfalls worked out from its legs, six a play, one foot after the other
    const auto walk = Animations()("pb_stand_shoot_walk_forward");
    CHECK(walk && walk->notetracks.size() <= 1 && ActorLogic::GetFootfalls(*walk).size() == 6);
}

TEST("joins: the walk is drawn in pieces named by their looks, one more after each join")
{
    const auto actor = TwoLooks();
    const auto timing = Timing(actor);
    auto pieces = ActorLogic::GetWalkPieces(actor, timing, Animations());
    CHECK(pieces.size() == 2 && timing.joins[1]);
    if (pieces.size() != 2 || !timing.joins[1])
        return;
    const auto walk = std::ranges::find(actor.clips, pieces[0].clip, &Types::ActorClip::id);
    CHECK(walk != actor.clips.end() && pieces[0].start == walk->start && !pieces[0].joined);
    CHECK(pieces[0].first == 0 && pieces[0].last == 0 && pieces[0].animation == "ch_khe_e1b_troopssprint_2");
    CHECK(pieces[1].clip == pieces[0].clip && pieces[1].first == 1 && pieces[1].last == 1 && pieces[1].joined);
    CHECK(pieces[1].animation == "walk_cqb_f" && pieces[1].blend == 340);
    CHECK(pieces[0].end == pieces[1].start && pieces[1].start == timing.joins[1]->first);
    // one look along the whole path: one piece over every stretch
    const auto plain =
        Runner({glm::vec3(0, 0, 0), glm::vec3(600, 0, 0), glm::vec3(1000, 0, 0)}, Set::Players, Gait::Run);
    pieces = ActorLogic::GetWalkPieces(plain, Timing(plain), Animations());
    CHECK(pieces.size() == 1 && pieces[0].first == 0 && pieces[0].last == 1);
    // a turn at the point: the walk after it is a piece of its own, from the point's stretch
    const auto turning = Runner(Corner(), Set::LowReady, Gait::Run);
    pieces = ActorLogic::GetWalkPieces(turning, Timing(turning), Animations());
    CHECK(pieces.size() == 2 && pieces[0].first == 0 && pieces[1].first == 1 && !pieces[1].joined);
    if (pieces.size() == 2)
        CHECK(std::ranges::find(turning.clips, pieces[1].clip, &Types::ActorClip::id)->chain ==
              Types::ActorChain::Walk);
}

TEST("joins: a slide moves everything of the actor from the set-off, the clip before lasting until it")
{
    auto actor = TwoLooks();
    const auto timing = Timing(actor);
    CHECK(timing.start && timing.arrival && actor.clips.front().start < *timing.start);
    if (!timing.start || !timing.arrival)
        return;
    // a shot before it sets off, a death after it arrives
    ActorLogic::AddEvent(actor, {.type = Types::ActorEventType::Fire, .tick = *timing.start - 200});
    ActorLogic::AddDeath(actor, *timing.arrival + 500, "pb_stand_death_headchest_topple", 2.0f);
    const auto before = actor;
    // (the walk sets off with its clip: the clip before is the idle)
    const auto setOff = actor.clips[1].start;
    CHECK(setOff <= *timing.start && actor.clips.front().End() >= setOff);
    CHECK(ActorLogic::SlideWalk(actor, *timing.start, 700) == 700);
    const auto moved = Timing(actor);
    CHECK(moved.start == *timing.start + 700 && moved.arrival == *timing.arrival + 700);
    CHECK(actor.clips.front().start == before.clips.front().start && actor.clips.front().End() == setOff + 700);
    for (std::size_t k = 1; k < actor.clips.size(); ++k)
        CHECK(actor.clips[k].start == before.clips[k].start + 700);
    CHECK(actor.events.front().tick == before.events.front().tick);
    CHECK(actor.events.back().tick == before.events.back().tick + 700);
    CHECK(ActorLogic::FindDeathClip(actor, actor.events.back()) != nullptr);
    // back: no further than the clip before has played a while
    auto back = before;
    const auto earliest = static_cast<std::int64_t>(back.clips.front().start + ActorLogic::MIN_WALK_LENGTH) - setOff;
    CHECK(ActorLogic::SlideWalk(back, *timing.start, -100000) == earliest);
    CHECK(Timing(back).start == static_cast<std::uint32_t>(*timing.start + earliest));
}

TEST("joins: Before ends at the point, After starts at it")
{
    for (const auto at : {Types::ActorJoinAt::Before, Types::ActorJoinAt::After})
    {
        auto actor = TwoLooks();
        actor.stretches[1].blendAt = at;
        actor.stretches[1].blend = 400;
        Replan(actor, Person(Set::Players));
        const auto timing = Timing(actor);
        CHECK(timing.reached[1] && timing.joins[1]);
        if (!timing.reached[1] || !timing.joins[1])
            continue;
        const bool before = at == Types::ActorJoinAt::Before;
        CHECK(Off(before ? timing.joins[1]->second : timing.joins[1]->first, timing.reached[1]) <= 20);
        // (the speed by the point: the walk's already, or the run's still)
        CHECK_NEAR(SpeedAt(actor, *timing.reached[1] + (before ? 20 : -20)),
                   Speed(before ? "walk_cqb_f" : "ch_khe_e1b_troopssprint_2"), 6.0);
    }
}

TEST("joins: the arrival counts the ease, and a blend changed keeps it with a pace within 80-120%")
{
    const auto body = Person(Set::Players);
    const auto actor = TwoLooks();
    const auto timing = Timing(actor);
    CHECK(timing.arrival);
    if (!timing.arrival)
        return;
    // (got there: at its end as it arrives)
    CHECK_NEAR(ActorLogic::GetPathProgress(actor, *timing.arrival, Animations()), 1000.0, 3.0);
    // a longer blend centred on the point: the walk is faster than its own on its side of it longer than the run is
    // slower on its own, so it gets there sooner
    auto longer = actor;
    longer.stretches[1].blend = 600;
    Replan(longer, body);
    const auto sooner = Timing(longer).arrival;
    CHECK(sooner && *sooner < *timing.arrival && *timing.arrival - *sooner < 100);
    // kept by the pace
    const auto pace = ActorLogic::SolvePace(longer, *timing.arrival, Animations(),
                                            [&](Types::Actor& fitted) { Replan(fitted, body); });
    CHECK(pace && *pace >= ActorLogic::MIN_PACE && *pace <= ActorLogic::MAX_PACE && *pace < 1.0f);
    if (!pace)
        return;
    longer.pace = *pace;
    Replan(longer, body);
    CHECK(Off(Timing(longer).arrival, timing.arrival) < 30);
}

TEST("joins: the walk after a turn at the point takes over from it over the join's blend, kept by re-plans")
{
    const auto body = Person(Set::LowReady);
    auto actor = Runner(Corner(), Set::LowReady, Gait::Run);
    const auto walk = [](const Types::Actor& each) {
        return std::ranges::find(each.clips, Types::ActorChain::Walk, &Types::ActorClip::chain);
    };
    CHECK(walk(actor) != actor.clips.end());
    if (walk(actor) == actor.clips.end())
        return;
    const auto planned = walk(actor)->blendIn;
    actor.stretches[1].blend = 450;
    Replan(actor, body);
    CHECK(walk(actor) != actor.clips.end() && walk(actor)->blendIn == 450);
    Replan(actor, body);
    CHECK(walk(actor) != actor.clips.end() && walk(actor)->blendIn == 450);
    // (Auto: as planned)
    actor.stretches[1].blend.reset();
    Replan(actor, body);
    CHECK(walk(actor) != actor.clips.end() && walk(actor)->blendIn == planned);
    // no blend of the join's own there (the turn is between)
    CHECK(!Timing(actor).joins[1]);
}

TEST("joins: scenes keep them; scenes from before keep theirs as they were, 0.12 s from the point")
{
    auto actor = TwoLooks();
    actor.stretches[1].blend = 500;
    actor.stretches[1].blendAt = Types::ActorJoinAt::Before;
    actor.stretches[1].matchFeet = false;
    actor.stretches[1].pace = 1.1f;
    ActorLogic::Normalize(actor);
    auto json = ActorLogic::SceneToJson({{actor}, ""}, "demo");
    auto read = ActorLogic::SceneFromJson(json, 60000);
    CHECK(read.actors.size() == 1 && read.actors[0].stretches == actor.stretches);
    // from before joins
    for (auto& stretch : json["actors"][0]["stretches"])
    {
        stretch.erase("blendAt");
        stretch.erase("matchFeet");
        stretch.erase("blend");
        stretch.erase("pace");
    }
    read = ActorLogic::SceneFromJson(json, 60000);
    CHECK(read.actors.size() == 1);
    if (read.actors.empty())
        return;
    auto old = read.actors[0];
    CHECK(!old.stretches[0].joinAsBefore && old.stretches[1].joinAsBefore && !old.stretches[2].joinAsBefore);
    CHECK(ActorLogic::GetJoinBlend(old, 1, Animations()) == ActorLogic::LEGS_BLEND_TO_MOVING);
    const auto timing = Timing(old);
    CHECK(timing.joins[1] && Off(timing.joins[1]->first, timing.reached[1]) <= 10 &&
          timing.joins[1]->second - timing.joins[1]->first == ActorLogic::LEGS_BLEND_TO_MOVING);
    // the speed changes at the point, as it did
    if (timing.reached[1])
    {
        CHECK_NEAR(SpeedAt(old, *timing.reached[1] - 20), Speed("ch_khe_e1b_troopssprint_2"), 1.5);
        CHECK_NEAR(SpeedAt(old, *timing.reached[1] + 20), Speed("walk_cqb_f"), 1.5);
    }
    // written again, still as it was; changed, one of now (centred, the feet matched) with its blend as it was
    CHECK(ActorLogic::SceneFromJson(ActorLogic::SceneToJson({{old}, ""}, "demo"), 60000).actors[0].stretches ==
          old.stretches);
    ActorLogic::UpdateJoin(old.stretches[1]);
    CHECK(!old.stretches[1].joinAsBefore && old.stretches[1].blend == 120u &&
          old.stretches[1].blendAt == Types::ActorJoinAt::Centred && old.stretches[1].matchFeet);
    // another look there: a join of now
    auto relooked = read.actors[0];
    ActorLogic::SetLook(relooked, 1, "walk_lowready_f", Person(Set::Players), Animations(), Has);
    CHECK(!relooked.stretches[1].joinAsBefore);
}

TEST("joins: a stretch set to another gait loses the look of its own, which was one of the gait it had")
{
    auto actor = TwoLooks();
    CHECK(actor.stretches[1].look == "walk_cqb_f");
    ActorLogic::SetGait(actor, 0, actor.path.size(), Gait::Run, false, Person(Set::Players), Animations(), Has);
    CHECK(actor.stretches[1].look.empty() && actor.stretches[1].animation == "ch_khe_e1b_troopssprint_2");
    // (the same gait again keeps it)
    auto same = TwoLooks();
    ActorLogic::SetGait(same, 1, 2, Gait::Walk, false, Person(Set::Players), Animations(), Has);
    CHECK(same.stretches[1].look == "walk_cqb_f");
}

// Deaths after the path: one rule for deaths however they are made, a death on the
// path's end dies as the actor runs, and nothing is put in that the user didn't add

namespace
{
    // the path's timing as a re-plan has it before it puts the one-offs back (Components::Actors::AnchorDeath)
    ActorLogic::PathTiming PlannedTiming(Types::Actor actor, const ActorLogic::Body& body)
    {
        ActorLogic::TakeOutOneOffs(actor, Animations());
        TransitionLogic::UnplanPath(actor);
        ActorLogic::FitOnlyWalk(actor, Animations());
        TransitionLogic::PlanPath(actor, body, Animations());
        return Timing(actor);
    }

    // a death made where the user didn't time it, anchored as Components::Actors::AnchorDeath does
    void AnchorMade(Types::Actor& actor, std::uint32_t clip, const ActorLogic::Body& body)
    {
        ActorLogic::AnchorMadeDeath(actor, clip, PlannedTiming(actor, body), Animations());
    }

    const Types::ActorClip* FindId(const Types::Actor& actor, std::uint32_t id)
    {
        const auto found = std::ranges::find(actor.clips, id, &Types::ActorClip::id);
        return found != actor.clips.end() ? &*found : nullptr;
    }

    std::vector<const Types::ActorEvent*> HitsOf(const Types::Actor& actor, std::uint32_t clip)
    {
        std::vector<const Types::ActorEvent*> hits;
        for (const auto& event : actor.events)
            if (event.type == Types::ActorEventType::Hit && event.death == clip)
                hits.push_back(&event);
        return hits;
    }

    std::uint32_t Ticks(std::string_view animation)
    {
        return static_cast<std::uint32_t>(std::lround(Animations()(std::string(animation))->duration * 1000.0f));
    }

    // TwoLooks with a one-off after its walk (Then...) swapped to a death by the clip's Animation, as a user made
    // intro_cinematic's: the death's id
    std::uint32_t SwapToDeath(Types::Actor& actor, std::string_view death)
    {
        const auto body = Person(Set::Players);
        const auto walk = TransitionLogic::GetPathWalks(actor, Animations()).front();
        const auto oneOff = TransitionLogic::PlayAfter(actor, walk, "ai_pillar_stand_arrive_1", body, Animations());
        Replan(actor, body, {oneOff});
        ActorLogic::SwapAnimation(actor, oneOff, std::string(death), Animations());
        AnchorMade(actor, oneOff, body);
        Replan(actor, body);
        return oneOff;
    }
}  // namespace

TEST("deaths: a swap to a death makes what a Hit's Dies makes: once, off the path, its Hit and blood")
{
    auto actor = TwoLooks();
    const auto death = SwapToDeath(actor, "pb_death_legsout_left");
    const auto* clip = FindId(actor, death);
    CHECK(clip && clip->animation == "pb_death_legsout_left");
    if (!clip)
        return;
    CHECK(!clip->loop && !clip->moves && clip->rootMotion && clip->offset == 0.0f && clip->picked);
    CHECK(clip->length == Ticks("pb_death_legsout_left"));
    // its Hit, with a fatal wound's blood, at its start
    const auto hits = HitsOf(actor, death);
    CHECK(hits.size() == 1 && hits[0]->effect == ActorLogic::FATAL_BLOOD && hits[0]->tick == clip->start);
    // it lies there for good: nothing after it
    CHECK(&actor.clips.back() == clip);
    // as AddDeath makes one: the same but where it is
    auto added = TwoLooks();
    ActorLogic::AddDeath(added, clip->start, "pb_death_legsout_left", Animations()("pb_death_legsout_left")->duration);
    const auto* dies = Find(added, "pb_death_legsout_left");
    CHECK(dies && dies->moves == clip->moves && dies->loop == clip->loop && dies->length == clip->length &&
          dies->rootMotion == clip->rootMotion);
    CHECK(added.events.size() == 1 && added.events[0].effect == ActorLogic::FATAL_BLOOD);
}

TEST("deaths: a death on the path's end takes over from the run as it gets there, nothing put in")
{
    const auto body = Person(Set::Players);
    auto actor = TwoLooks();
    const auto death = SwapToDeath(actor, "pb_death_legsout_left");
    const auto* clip = FindId(actor, death);
    const auto timing = Timing(actor);
    CHECK(clip && timing.arrival);
    if (!clip || !timing.arrival)
        return;
    // no idle: the walk's clips, then the death
    const auto index = static_cast<std::size_t>(clip - actor.clips.data());
    CHECK(index > 0 && index + 1 == actor.clips.size());
    for (std::size_t k = 1; k < index; ++k)
        CHECK(actor.clips[k].chain != Types::ActorChain::None ||
              ActorLogic::FindLocomotion(actor, actor.clips[k].animation).has_value());
    CHECK(actor.clips[index - 1].End() == clip->start);
    // the arrival, the bar's end and the last piece's end meet at its start
    CHECK(clip->anchor == Types::ActorAnchor::End && clip->anchorOffset == 0);
    CHECK(Off(timing.arrival, clip->start) <= 10);
    const auto pieces = ActorLogic::GetWalkPieces(actor, timing, Animations());
    CHECK(!pieces.empty() && pieces.back().end == clip->start);
    // it keeps facing the way it ran as the death starts (off the path, its root motion moves it)
    const auto before = ActorLogic::EvaluatePlacement(actor, clip->start - 10, Animations());
    const auto after = ActorLogic::EvaluatePlacement(actor, clip->start + 150, Animations());
    CHECK(std::abs(std::remainder(after.yaw - before.yaw, 360.0f)) < 20.0f);
    // planned again, as it is
    auto again = actor;
    Replan(again, body);
    CHECK(again.clips == actor.clips && again.events == actor.events);

    // the idle a run stops into swapped to a death (the AI's stop, with low ready): it dies as it runs, its stop gone
    const auto lowReady = Person(Set::LowReady);
    auto runner = Runner(Corner(), Set::LowReady, Gait::Run);
    const auto stood = std::ranges::find(runner.clips.rbegin(), runner.clips.rend(), "casual_stand_idle",
                                         &Types::ActorClip::animation)
                           ->id;
    CHECK(std::ranges::any_of(runner.clips, [](const Types::ActorClip& each) {
        return each.chain == Types::ActorChain::Stop;
    }));
    ActorLogic::SwapAnimation(runner, stood, "ai_death_fallforward", Animations());
    AnchorMade(runner, stood, lowReady);
    Replan(runner, lowReady);
    const auto* fell = FindId(runner, stood);
    CHECK(fell && &runner.clips.back() == fell && fell->anchor == Types::ActorAnchor::End && fell->anchorOffset == 0);
    CHECK(std::ranges::none_of(runner.clips, [](const Types::ActorClip& each) {
        return each.chain == Types::ActorChain::Stop;
    }));
    CHECK(fell && Off(Timing(runner).arrival, fell->start) <= 10);

    // flatonback the same: it doesn't turn round (following the path, it faced -173 degrees from the way it ran)
    auto flat = TwoLooks();
    const auto flatDeath = SwapToDeath(flat, "ai_death_flatonback");
    const auto* flatClip = FindId(flat, flatDeath);
    CHECK(flatClip && !flatClip->moves);
    if (flatClip)
    {
        const auto a = ActorLogic::EvaluatePlacement(flat, flatClip->start - 10, Animations());
        const auto b = ActorLogic::EvaluatePlacement(flat, flatClip->start + 100, Animations());
        CHECK(std::abs(std::remainder(b.yaw - a.yaw, 360.0f)) < 20.0f);
    }
}

TEST("deaths: dragged later than the arrival, a gap until it with nothing put in; an idle put there is stopped into")
{
    const auto body = Person(Set::Players);
    auto actor = TwoLooks();
    const auto death = SwapToDeath(actor, "pb_death_legsout_left");
    const auto count = actor.clips.size();
    const auto arrival = Timing(actor).arrival;
    CHECK(arrival.has_value() && HitsOf(actor, death).size() == 1);
    if (!arrival || HitsOf(actor, death).size() != 1)
        return;
    // dragged half a second later (its Hit moves with it, and it is anchored again where it is)
    auto later = actor;
    ActorLogic::MoveEvent(later, HitsOf(later, death).front()->id, *arrival + 500);
    Replan(later, body, {death});
    const auto* clip = FindId(later, death);
    CHECK(clip && later.clips.size() == count);
    if (!clip)
        return;
    CHECK(clip->start == *arrival + 500 && clip->anchor == Types::ActorAnchor::End && clip->anchorOffset == 500);
    CHECK(Off(Timing(later).arrival, arrival) <= 10);
    const auto index = static_cast<std::size_t>(clip - later.clips.data());
    // (it arrives within ARRIVAL_TOLERANCE of the end, a few ms before its walk ends)
    CHECK(index > 0 && later.clips[index - 1].End() <= *arrival + 30);
    // the walk holds its last frame there: the actor stays at the path's end
    const auto a = ActorLogic::EvaluatePlacement(later, *arrival + 20, Animations());
    const auto b = ActorLogic::EvaluatePlacement(later, *arrival + 480, Animations());
    CHECK(glm::distance(a.position, b.position) < 1.0f);
    // dragged earlier, it cuts the run there
    auto earlier = actor;
    ActorLogic::MoveEvent(earlier, HitsOf(earlier, death).front()->id, *arrival - 700);
    Replan(earlier, body, {death});
    clip = FindId(earlier, death);
    CHECK(clip && clip->start == *arrival - 700 && !Timing(earlier).arrival);

    // an idle of the user's in the gap: the walk stops into it (the AI's stop, with low ready), the death after it
    const auto lowReady = Person(Set::LowReady);
    auto runner = Runner(Corner(), Set::LowReady, Gait::Run);
    const auto runArrival = Timing(runner).arrival;
    CHECK(runArrival.has_value());
    if (!runArrival)
        return;
    ActorLogic::AddDeath(runner, *runArrival + 1500, "ai_death_fallforward", 1.33f);
    // (the one after the walk, picked from the browser)
    const auto idle = std::ranges::find(runner.clips.rbegin(), runner.clips.rend(), "casual_stand_idle",
                                        &Types::ActorClip::animation);
    CHECK(idle != runner.clips.rend());
    if (idle == runner.clips.rend())
        return;
    const auto idleId = idle->id;
    ActorLogic::SwapAnimation(runner, idleId, "casual_stand_idle", Animations());
    Replan(runner, lowReady);
    const auto* stood = FindId(runner, idleId);
    const auto dies = IndexOf(runner, "ai_death_fallforward");
    CHECK(stood && stood->picked && dies < runner.clips.size());
    if (!stood || dies >= runner.clips.size())
        return;
    const auto at = static_cast<std::size_t>(stood - runner.clips.data());
    CHECK(at > 0 && runner.clips[at - 1].chain == Types::ActorChain::Stop && at + 1 == dies);
    CHECK(stood->End() == runner.clips[dies].start);
}

TEST("deaths: swapped away, the Hit goes; a one-off swapped in plays once off the path, a moving loop follows it")
{
    auto actor = TwoLooks();
    const auto death = SwapToDeath(actor, "pb_death_legsout_left");
    CHECK(HitsOf(actor, death).size() == 1);
    if (HitsOf(actor, death).size() != 1)
        return;
    // to an idle: the Hit and its blood go
    auto idled = actor;
    CHECK(ActorLogic::SwapAnimation(idled, death, "pb_stand_alert", Animations()) == 0);
    CHECK(HitsOf(idled, death).empty() && idled.events.empty());
    const auto* idle = FindId(idled, death);
    CHECK(idle && idle->loop && !idle->moves);
    // to another death: the same Hit
    auto other = actor;
    const auto hit = HitsOf(other, death).front()->id;
    CHECK(ActorLogic::SwapAnimation(other, death, "ai_death_flatonback", Animations()) == hit);
    CHECK(HitsOf(other, death).size() == 1 && FindId(other, death)->length == Ticks("ai_death_flatonback"));

    // a one-off swapped in for a clip that followed the path: off it, once, from its start
    Types::Actor walker;
    walker.clips = {{.animation = "pb_stand_alert", .start = 0, .length = 9000, .offset = 1.5f}};
    ActorLogic::Normalize(walker);
    const auto id = walker.clips[0].id;
    CHECK(walker.clips[0].moves);
    ActorLogic::SwapAnimation(walker, id, "ai_pillar_stand_arrive_1", Animations());
    CHECK(!walker.clips[0].moves && !walker.clips[0].loop && walker.clips[0].offset == 0.0f &&
          walker.clips[0].length == Ticks("ai_pillar_stand_arrive_1") && walker.clips[0].picked);
    // a run swapped in: along the path
    ActorLogic::SwapAnimation(walker, id, "pb_sprint", Animations());
    CHECK(walker.clips[0].moves && walker.clips[0].loop);
}

TEST("deaths: a saved death that follows the path loads with it off, nothing else changed")
{
    auto actor = TwoLooks();
    const auto death = SwapToDeath(actor, "ai_death_flatonback");
    // (as a swap before this left one: following the path, without its Hit)
    auto saved = actor;
    std::ranges::find(saved.clips, death, &Types::ActorClip::id)->moves = true;
    saved.events.clear();
    // (the clips get the ids 1, 2, ... in order as a scene is read)
    auto read = ActorLogic::SceneFromJson(ActorLogic::SceneToJson({{saved}, ""}, "demo"), 60000).actors.at(0);
    CHECK(read.clips.size() == saved.clips.size() && read.clips.back().moves);
    CHECK(ActorLogic::TakeDeathsOffPath(read, Animations()));
    CHECK(!read.clips.back().moves && read.events.empty());
    auto unchanged = read;
    unchanged.clips.back().moves = true;
    auto before = ActorLogic::SceneFromJson(ActorLogic::SceneToJson({{saved}, ""}, "demo"), 60000).actors.at(0);
    CHECK(unchanged == before);
    CHECK(!ActorLogic::TakeDeathsOffPath(read, Animations()));
}

TEST("deaths: Die suits the clip before the death as it ends, the path's stretch only while the walk plays it")
{
    using Types::AnimationMovement;
    using Types::AnimationPosture;
    const auto suited = [](const Types::Actor& actor, std::uint32_t tick, float speed) {
        return ActorLogic::GetSuitedState(actor, tick, speed, Timing(actor), Animations());
    };
    const auto state = [](AnimationPosture posture, AnimationMovement movement) {
        return Types::AnimationState{posture, movement};
    };
    // Actor 3 of a user's intro_cinematic: it sprints its path, arrives crouching by a pillar and crouch-idles
    auto actor = Runner(Corner(), Set::Players, Gait::Sprint);
    const auto timing = Timing(actor);
    CHECK(timing.start && timing.arrival && timing.reached.size() == 3 && timing.reached[1]);
    const auto arrival = *timing.arrival;
    const auto arrive = ActorLogic::PlayAt(actor, arrival, "ai_pillar_crouch_arrive_2", 1.933f, false);
    const auto idleAt = arrival + 1933;
    ActorLogic::PlayAt(actor, idleAt, "ai_pillar_crouch_idle", 1.267f, true);
    CHECK(std::ranges::find(actor.clips, arrive, &Types::ActorClip::id) != actor.clips.end());

    // mid-walk, the stretch at the speed it goes
    CHECK(suited(actor, *timing.reached[1] - 300, 400.0f) == state(AnimationPosture::Stand, AnimationMovement::Run));
    // the arrival crouches as it ends (its end state, not the sprint before it), the idle crouches
    CHECK(suited(actor, arrival + 500, 0.0f) == state(AnimationPosture::Crouch, AnimationMovement::Stop));
    // (one play of the idle, then the stand idle the path ends with goes on)
    CHECK(suited(actor, idleAt + 600, 0.0f) == state(AnimationPosture::Crouch, AnimationMovement::Stop));
    CHECK(ActorLogic::DescribeSuitedState(suited(actor, idleAt + 600, 0.0f)) == "crouched");

    // at a Hit with its death, the clip before the death: the crouch idle, whatever the death's own motion
    const auto hitAt = idleAt + 1000;
    ActorLogic::PlayAt(actor, hitAt, "ai_viet_exposed_crouch_death_fetal", 1.333f, false);
    CHECK(ActorLogic::IsDeath("ai_viet_exposed_crouch_death_fetal",
                              Animations()("ai_viet_exposed_crouch_death_fetal")));
    CHECK(suited(actor, hitAt, 150.0f) == state(AnimationPosture::Crouch, AnimationMovement::Stop));
    CHECK(suited(actor, hitAt + 1000, 0.0f) == state(AnimationPosture::Crouch, AnimationMovement::Stop));

    // a walk with nothing after it stands still where it arrives; a crouched stretch is crouched while it is walked
    auto walker = Runner(Corner(), Set::Players, Gait::Run);
    const auto walked = Timing(walker);
    CHECK(suited(walker, *walked.arrival + 3000, 0.0f) == state(AnimationPosture::Stand, AnimationMovement::Stop));
    for (auto& stretch : walker.stretches)
        stretch.crouched = true;
    CHECK(suited(walker, *walked.reached[1] - 300, 200.0f) == state(AnimationPosture::Crouch, AnimationMovement::Run));
    CHECK(ActorLogic::DescribeSuitedState(state(AnimationPosture::Crouch, AnimationMovement::Run)) ==
          "crouched, running");
    CHECK(ActorLogic::DescribeSuitedState(state(AnimationPosture::Stand, AnimationMovement::Walk)) == "walking");
    CHECK(ActorLogic::DescribeSuitedState(state(AnimationPosture::Prone, AnimationMovement::Stop)) == "prone");

    // without a path, the clip shown; a clip the catalog doesn't know, the actor's stance at its speed
    Types::Actor still;
    still.clips = {{.id = 1, .animation = "pb_crouch_alert", .start = 0, .length = 10000}};
    CHECK(suited(still, 5000, 0.0f) == state(AnimationPosture::Crouch, AnimationMovement::Stop));
    still.clips[0].animation = "not_in_the_catalog";
    still.stance = Types::ActorStance::Prone;
    CHECK(suited(still, 5000, 0.0f) == state(AnimationPosture::Prone, AnimationMovement::Stop));
    still.stance = Types::ActorStance::Auto;
    CHECK(suited(still, 5000, 60.0f) == state(AnimationPosture::Stand, AnimationMovement::Walk));
}
