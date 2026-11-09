#include "StdInclude.hpp"
#include "ActorLogic.hpp"

#include <complex>

#include "CharacterLogic.hpp"
#include "KillstreakLogic.hpp"
#include "LightLogic.hpp"
#include "TransitionLogic.hpp"
#include "VehicleLogic.hpp"

namespace IWXMVM::ActorLogic
{
    void Normalize(Types::Actor& actor)
    {
        // (a scene effect's bar is a tenth of a second at least)
        if (actor.effect)
        {
            actor.effect->end = std::max(actor.effect->end, actor.effect->start + 100);
            actor.effect->every = std::max(actor.effect->every, 100u);
        }
        if (actor.light)
            Components::LightLogic::Normalize(*actor.light);
        if (actor.placedProp)
        {
            actor.placedProp->end = std::max(actor.placedProp->end, actor.placedProp->start + 100);
            actor.placedProp->scale = std::clamp(actor.placedProp->scale, MIN_PROP_SCALE, MAX_PROP_SCALE);
        }
        // (the Face row's clips have ids of their own)
        for (auto* clips : {&actor.clips, &actor.faceClips})
        {
            std::uint32_t nextClipId = 1;
            for (const auto& clip : *clips)
                nextClipId = std::max(nextClipId, clip.id + 1);
            for (auto& clip : *clips)
            {
                if (clip.id == 0)
                    clip.id = nextClipId++;
                clip.length = std::max(clip.length, 1u);
            }
            std::ranges::stable_sort(*clips, {}, &Types::ActorClip::start);
        }
        std::ranges::stable_sort(actor.keys, {}, &Types::ActorKey::tick);

        std::uint32_t nextEventId = 1;
        for (const auto& event : actor.events)
            nextEventId = std::max(nextEventId, event.id + 1);
        for (auto& event : actor.events)
        {
            if (event.id == 0)
                event.id = nextEventId++;
            event.shots = std::max(event.shots, 1u);
            event.interval = std::max(event.interval, 1u);
            // (a Hit and its death are one: it goes off as its death starts, wherever that is moved; none once the
            // death is gone)
            if (event.type == Types::ActorEventType::Hit && event.death != 0)
            {
                const auto death = std::ranges::find(actor.clips, event.death, &Types::ActorClip::id);
                if (death == actor.clips.end())
                    event.death = 0;
                else
                    event.tick = death->start;
            }
            // (a killstreak's death's effects and Hide go off where in it they are)
            else if (event.death != 0)
                if (const auto death = std::ranges::find(actor.clips, event.death, &Types::ActorClip::id);
                    death != actor.clips.end())
                    event.tick = (event.afterDeathEnd ? death->End() : death->start) + event.deathAt;
        }
        // (and go with it)
        std::erase_if(actor.events, [&](const Types::ActorEvent& event) {
            return event.type != Types::ActorEventType::Hit && event.death != 0 &&
                   std::ranges::find(actor.clips, event.death, &Types::ActorClip::id) == actor.clips.end();
        });
        std::ranges::stable_sort(actor.events, {}, &Types::ActorEvent::tick);
        FitStretches(actor);
    }

    std::size_t Sample(const Types::Actor& actor, std::uint32_t tick, std::array<Types::ActorAnimationSample, 2>& out)
    {
        const auto& clips = actor.clips;
        if (clips.empty())
            return 0;

        const auto current = std::ranges::find_if(clips.rbegin(), clips.rend(),
                                                  [&](const Types::ActorClip& clip) { return clip.start <= tick; });
        auto timeOf = [&](const Types::ActorClip& clip, std::uint32_t limit) {
            const auto elapsed = tick >= clip.start ? std::min(tick - clip.start, limit) : 0;
            return clip.offset + elapsed / 1000.0f * std::max(clip.speed, 0.0f);
        };
        if (current == clips.rend())
        {
            out[0] = {&clips.front(), clips.front().offset, 1.0f};
            return 1;
        }

        const auto& clip = *current;
        out[0] = {&clip, timeOf(clip, clip.length), 1.0f};
        const auto previous = std::next(current);
        const auto sinceStart = tick - clip.start;
        if (previous != clips.rend() && clip.blendIn > 0 && sinceStart < clip.blendIn)
        {
            const auto weight = static_cast<float>(sinceStart) / clip.blendIn;
            out[0].weight = weight;
            out[1] = {&*previous, timeOf(*previous, previous->length + clip.blendIn), 1.0f - weight};
            return 2;
        }
        return 1;
    }

    std::vector<Types::ActorAnimationSample> Evaluate(const Types::Actor& actor, std::uint32_t tick)
    {
        std::array<Types::ActorAnimationSample, 2> samples{};
        return {samples.begin(), samples.begin() + Sample(actor, tick, samples)};
    }

    Types::ActorStance GetNameStance(std::string_view animation)
    {
        if (animation.find("prone") != std::string_view::npos)
            return Types::ActorStance::Prone;
        if (animation.find("crouch") != std::string_view::npos)
            return Types::ActorStance::Crouch;
        return Types::ActorStance::Stand;
    }

    Types::ActorStance GetStance(std::string_view animation, const std::optional<AnimationInfo>& info)
    {
        return info && info->stance ? *info->stance : GetNameStance(animation);
    }

    std::optional<Types::ActorStance> GetNotetrackStance(std::string_view notetrack)
    {
        constexpr std::string_view KEY = "anim_pose";
        if (!notetrack.starts_with(KEY))
            return std::nullopt;
        // = "crouch", with or without the spaces and quotes
        std::string value;
        for (const auto c : notetrack.substr(KEY.size()))
            if (c != ' ' && c != '=' && c != '"')
                value += c;
        if (value == "stand")
            return Types::ActorStance::Stand;
        if (value == "crouch")
            return Types::ActorStance::Crouch;
        // (crawling and lying on the back are SP's, the legs of neither are standing or crouching)
        if (value == "prone" || value == "crawl" || value == "back")
            return Types::ActorStance::Prone;
        return std::nullopt;
    }

    namespace
    {
        // The players' animations of each gun class (as GunClass) and stance (standing, crouching, prone), as
        // mp/playeranim.script has them: the idle, the walks and the runs forward, back, left and right, and the
        // sprint. Prone, they crawl. Dual wield and the ballistic knife have no crouching walk (their players can't aim
        // down the sights, and only those walk), so they run slower there; the ballistic knife sprints and crawls as
        // dual wield does.
        struct LocomotionSet
        {
            std::string_view idle;
            std::array<std::string_view, 4> walks;
            std::array<std::string_view, 4> runs;
            std::string_view sprint;  // empty for none
        };
        constexpr std::array<std::array<LocomotionSet, 3>, 6> LOCOMOTION = {{
            // rifle (and anything else)
            {{
                {"pb_stand_alert",
                 {"pb_stand_shoot_walk_forward", "pb_stand_shoot_walk_back", "pb_stand_shoot_walk_left",
                  "pb_stand_shoot_walk_right"},
                 {"pb_combatrun_forward_loop", "pb_combatrun_back_loop", "pb_combatrun_left_loop",
                  "pb_combatrun_right_loop"},
                 "pb_sprint"},
                {"pb_crouch_alert",
                 {"pb_crouch_shoot_run_forward", "pb_crouch_shoot_run_back", "pb_crouch_shoot_run_left",
                  "pb_crouch_shoot_run_right"},
                 {"pb_crouch_run_forward", "pb_crouch_run_back", "pb_crouch_run_left", "pb_crouch_run_right"},
                 {}},
                {"pb_prone_aim",
                 {"pb_prone_crawl", "pb_prone_crawl_back", "pb_prone_crawl_left", "pb_prone_crawl_right"},
                 {"pb_prone_crawl", "pb_prone_crawl_back", "pb_prone_crawl_left", "pb_prone_crawl_right"},
                 {}},
            }},
            // pistol
            {{
                {"pb_stand_alert_pistol",
                 {"pb_combatwalk_forward_loop_pistol", "pb_combatwalk_back_loop_pistol",
                  "pb_combatwalk_left_loop_pistol", "pb_combatwalk_right_loop_pistol"},
                 {"pb_pistol_run_fast", "pb_combatrun_back_loop_pistol", "pb_combatrun_left_loop_pistol",
                  "pb_combatrun_right_loop_pistol"},
                 "pb_sprint_pistol"},
                {"pb_crouch_alert_pistol",
                 {"pb_crouch_walk_forward_pistol", "pb_crouch_walk_back_pistol", "pb_crouch_walk_left_pistol",
                  "pb_crouch_walk_right_pistol"},
                 {"pb_crouch_run_forward_pistol", "pb_crouch_run_back_pistol", "pb_crouch_run_left_pistol",
                  "pb_crouch_run_right_pistol"},
                 {}},
                {"pb_prone_aim_pistol",
                 {"pb_prone_crawl_pistol", "pb_prone_crawl_pistol_back", "pb_prone_pistol_crawl_left",
                  "pb_prone_pistol_crawl_right"},
                 {"pb_prone_crawl_pistol", "pb_prone_crawl_pistol_back", "pb_prone_pistol_crawl_left",
                  "pb_prone_pistol_crawl_right"},
                 {}},
            }},
            // rocket launcher
            {{
                {"pb_stand_alert_rpg",
                 {"pb_walk_forward_rpg_ads", "pb_walk_back_rpg_ads", "pb_walk_left_rpg_ads", "pb_walk_right_rpg_ads"},
                 {"pb_combatrun_forward_rpg", "pb_combatrun_back_rpg", "pb_combatrun_left_rpg",
                  "pb_combatrun_right_rpg"},
                 "pb_sprint_rpg"},
                {"pb_crouch_alert_rpg",
                 {"pb_crouch_walk_forward_rpg", "pb_crouch_walk_back_rpg", "pb_crouch_walk_left_rpg",
                  "pb_crouch_walk_right_rpg"},
                 {"pb_crouch_run_forward_rpg", "pb_crouch_run_back_rpg", "pb_crouch_run_left_rpg",
                  "pb_crouch_run_right_rpg"},
                 {}},
                {"pb_prone_aim_rpg",
                 {"pb_rpg_prone_crawl_forward", "pb_rpg_prone_crawl_backward", "pb_rpg_prone_crawl_left",
                  "pb_rpg_prone_crawl_right"},
                 {"pb_rpg_prone_crawl_forward", "pb_rpg_prone_crawl_backward", "pb_rpg_prone_crawl_left",
                  "pb_rpg_prone_crawl_right"},
                 {}},
            }},
            // dual wield
            {{
                {"pb_dw_botharms_idle_v1",
                 {"pb_dw_walk_forward", "pb_dw_walk_back", "pb_dw_walk_left", "pb_dw_walk_right"},
                 {"pb_dw_run_forward", "pb_dw_run_back", "pb_dw_run_left", "pb_dw_run_right"},
                 "pb_dw_sprint"},
                {"pb_crouch_dw_idle_v1",
                 {"pb_crouch_dw_run_forward", "pb_crouch_dw_run_back", "pb_crouch_dw_run_left",
                  "pb_crouch_dw_run_right"},
                 {"pb_crouch_dw_run_forward", "pb_crouch_dw_run_back", "pb_crouch_dw_run_left",
                  "pb_crouch_dw_run_right"},
                 {}},
                {"pb_prone_dw_idle_v1",
                 {"pb_prone_dw_crawl_forward", "pb_prone_dw_crawl_back", "pb_prone_dw_crawl_left",
                  "pb_prone_dw_crawl_right"},
                 {"pb_prone_dw_crawl_forward", "pb_prone_dw_crawl_back", "pb_prone_dw_crawl_left",
                  "pb_prone_dw_crawl_right"},
                 {}},
            }},
            // ballistic knife
            {{
                {"pb_b_knife_stand_idle",
                 {"pb_b_knife_walk_forward", "pb_b_knife_walk_back", "pb_b_knife_walk_left", "pb_b_knife_walk_right"},
                 {"pb_b_knife_run_forward", "pb_b_knife_run_back", "pb_b_knife_run_left", "pb_b_knife_run_right"},
                 "pb_dw_sprint"},
                {"pb_b_knife_crouch_idle",
                 {"pb_b_knife_crouch_run_forward", "pb_b_knife_crouch_run_back", "pb_b_knife_crouch_run_left",
                  "pb_b_knife_crouch_run_right"},
                 {"pb_b_knife_crouch_run_forward", "pb_b_knife_crouch_run_back", "pb_b_knife_crouch_run_left",
                  "pb_b_knife_crouch_run_right"},
                 {}},
                {"pb_b_knife_prone_idle",
                 {"pb_prone_dw_crawl_forward", "pb_prone_dw_crawl_back", "pb_prone_dw_crawl_left",
                  "pb_prone_dw_crawl_right"},
                 {"pb_prone_dw_crawl_forward", "pb_prone_dw_crawl_back", "pb_prone_dw_crawl_left",
                  "pb_prone_dw_crawl_right"},
                 {}},
            }},
            // the Death Machine: it can't sprint or lie down, and it runs crouched where others walk (lying down, the
            // rifle's by the fallback)
            {{
                {"pb_minigun_stand_idle",
                 {"pb_minigun_stand_walk", "pb_minigun_stand_walk_back", "pb_minigun_stand_walk_left",
                  "pb_minigun_stand_walk_right"},
                 {"pb_minigun_stand_run", "pb_minigun_stand_run_back", "pb_minigun_stand_run_left",
                  "pb_minigun_stand_run_right"},
                 {}},
                {"pb_minigun_crouch_idle",
                 {"pb_minigun_crouch_run_forward", "pb_minigun_crouch_run_back", "pb_minigun_crouch_run_left",
                  "pb_minigun_crouch_run_right"},
                 {"pb_minigun_crouch_run_forward", "pb_minigun_crouch_run_back", "pb_minigun_crouch_run_left",
                  "pb_minigun_crouch_run_right"},
                 {}},
                {{}, {}, {}, {}},
            }},
        }};

        // how fast the rifle's animations of each stance (standing, crouching, prone) and gait go, units a second, as
        // the game measured them: for movement animations not measured yet
        constexpr std::array<std::array<float, 4>, 3> GUESSED_SPEEDS = {{
            {0.0f, 94.0f, 211.0f, 281.0f},
            {0.0f, 83.0f, 120.0f, 120.0f},
            {0.0f, 7.7f, 7.7f, 7.7f},
        }};

        // The campaign AI's of each movement set (as Types::ActorMovementSet from LowReady on) and stance, rifle, as
        // animscripts/anims_table.gsc has them: low ready (walk_lowready_*, run_lowready_*, ai_sprint_f; crouched its
        // fast crouched walk, prone its crawl), close quarters (walk_CQB_F, run_CQB_F_search_v1, ai_cqb_sprint, the
        // others low ready's), run and gun (run_n_gun_F forward, the others low ready's); the idles the casual ones the
        // AI stands in once stopped (stop.gsc's idle sets), close quarters' its own
        constexpr std::array<std::array<LocomotionSet, 3>, 3> AI_LOCOMOTION = {{
            // low ready
            {{
                {"casual_stand_idle",
                 {"walk_lowready_f", "walk_lowready_b", "walk_lowready_l", "walk_lowready_r"},
                 {"run_lowready_f", "run_lowready_b", "run_lowready_l", "run_lowready_r"},
                 "ai_sprint_f"},
                {"casual_crouch_idle",
                 {"crouch_fastwalk_f", "crouch_fastwalk_b", "crouch_fastwalk_l", "crouch_fastwalk_r"},
                 {"crouch_fastwalk_f", "crouch_fastwalk_b", "crouch_fastwalk_l", "crouch_fastwalk_r"},
                 {}},
                {"prone_idle", {"prone_crawl", "prone_crawl", "prone_crawl", "prone_crawl"},
                 {"prone_crawl", "prone_crawl", "prone_crawl", "prone_crawl"}, {}},
            }},
            // close quarters
            {{
                {"cqb_stand_idle",
                 {"walk_cqb_f", "walk_lowready_b", "walk_lowready_l", "walk_lowready_r"},
                 {"run_cqb_f_search_v1", "run_lowready_b", "run_lowready_l", "run_lowready_r"},
                 "ai_cqb_sprint"},
                {"casual_crouch_idle",
                 {"crouch_fastwalk_f", "crouch_fastwalk_b", "crouch_fastwalk_l", "crouch_fastwalk_r"},
                 {"crouch_fastwalk_f", "crouch_fastwalk_b", "crouch_fastwalk_l", "crouch_fastwalk_r"},
                 {}},
                {"prone_idle", {"prone_crawl", "prone_crawl", "prone_crawl", "prone_crawl"},
                 {"prone_crawl", "prone_crawl", "prone_crawl", "prone_crawl"}, {}},
            }},
            // run and gun
            {{
                {"casual_stand_idle",
                 {"walk_lowready_f", "walk_lowready_b", "walk_lowready_l", "walk_lowready_r"},
                 {"run_n_gun_f", "run_lowready_b", "run_lowready_l", "run_lowready_r"},
                 "ai_sprint_f"},
                {"casual_crouch_idle",
                 {"crouch_fastwalk_f", "crouch_fastwalk_b", "crouch_fastwalk_l", "crouch_fastwalk_r"},
                 {"crouch_fastwalk_f", "crouch_fastwalk_b", "crouch_fastwalk_l", "crouch_fastwalk_r"},
                 {}},
                {"prone_idle", {"prone_crawl", "prone_crawl", "prone_crawl", "prone_crawl"},
                 {"prone_crawl", "prone_crawl", "prone_crawl", "prone_crawl"}, {}},
            }},
        }};
        // how fast they go forward (the catalog's root motion of a play over its duration): by set, stance and gait
        constexpr std::array<std::array<std::array<float, 4>, 3>, 3> AI_GUESSED_SPEEDS = {{
            {{{0.0f, 90.9f, 181.8f, 232.4f}, {0.0f, 68.7f, 68.7f, 68.7f}, {0.0f, 7.7f, 7.7f, 7.7f}}},
            {{{0.0f, 97.7f, 121.0f, 211.1f}, {0.0f, 68.7f, 68.7f, 68.7f}, {0.0f, 7.7f, 7.7f, 7.7f}}},
            {{{0.0f, 90.9f, 219.6f, 232.4f}, {0.0f, 68.7f, 68.7f, 68.7f}, {0.0f, 7.7f, 7.7f, 7.7f}}},
        }};
        // and the presets of the Movement page's speed (walk, run, sprint, crouched walk) of each set
        constexpr std::array<std::array<SpeedPreset, 4>, 3> AI_SPEED_PRESETS = {{
            {{{"Walk", 91.0f, Types::ActorStance::Stand, Gait::Walk},
              {"Run", 182.0f, Types::ActorStance::Stand, Gait::Run},
              {"Sprint", 232.0f, Types::ActorStance::Stand, Gait::Sprint},
              {"Crouch walk", 69.0f, Types::ActorStance::Crouch, Gait::Walk}}},
            {{{"Walk", 98.0f, Types::ActorStance::Stand, Gait::Walk},
              {"Run", 121.0f, Types::ActorStance::Stand, Gait::Run},
              {"Sprint", 211.0f, Types::ActorStance::Stand, Gait::Sprint},
              {"Crouch walk", 69.0f, Types::ActorStance::Crouch, Gait::Walk}}},
            {{{"Walk", 91.0f, Types::ActorStance::Stand, Gait::Walk},
              {"Run", 220.0f, Types::ActorStance::Stand, Gait::Run},
              {"Sprint", 232.0f, Types::ActorStance::Stand, Gait::Sprint},
              {"Crouch walk", 69.0f, Types::ActorStance::Crouch, Gait::Walk}}},
        }};

        // the AI's set's index into those, none for the players'
        std::optional<std::size_t> GetAiSet(Types::ActorMovementSet set)
        {
            switch (set)
            {
                case Types::ActorMovementSet::LowReady:
                    return 0;
                case Types::ActorMovementSet::Cqb:
                    return 1;
                case Types::ActorMovementSet::RunAndGun:
                    return 2;
                default:
                    return std::nullopt;
            }
        }

        std::size_t GetStanceColumn(Types::ActorStance stance)
        {
            return stance == Types::ActorStance::Crouch ? 1 : stance == Types::ActorStance::Prone ? 2 : 0;
        }

        const LocomotionSet& GetLocomotionSet(Types::GunClass gun, Types::ActorStance stance)
        {
            return LOCOMOTION[std::min(static_cast<std::size_t>(gun), LOCOMOTION.size() - 1)][GetStanceColumn(stance)];
        }

        // a person's of a body: its movement set's, the AI's for an AI's set, else the players' of its gun
        const LocomotionSet& GetLocomotionSet(const Body& body, Types::ActorStance stance)
        {
            if (const auto ai = GetAiSet(body.set))
                return AI_LOCOMOTION[*ai][GetStanceColumn(stance)];
            return GetLocomotionSet(body.gun, stance);
        }

        // The dogs' animations of each breed (as DogBreed), as the game's dogs play them: the idle, walk, run and
        // sprint (empty for none), the death (multiplayer's dog_death.gsc plays german_shepherd_death_front, Zombies'
        // the hellhound's of the same name), and how fast the movement ones go, units a second, as the catalog of every
        // animation has their root motion (the game measures them the same), for before the game has measured them.
        // The hellhound trots slower than it runs, and its walk doesn't loop in the game (actors loop it).
        struct DogSet
        {
            std::string_view idle, walk, run, sprint, death;
            std::array<float, 4> speeds;  // by gait
        };
        constexpr std::array<DogSet, 2> DOGS = {{
            {"german_shepherd_idle",
             "german_shepherd_walk",
             "german_shepherd_run",
             {},
             "german_shepherd_death_front",
             {0.0f, 65.5f, 319.7f, 319.7f}},
            {"zombie_dog_idle",
             "zombie_dog_walk",
             "zombie_dog_trot",
             "zombie_dog_run",
             "zombie_dog_death_front",
             {0.0f, 52.9f, 136.2f, 362.1f}},
        }};

        const DogSet& GetDogSet(DogBreed breed)
        {
            return DOGS[std::min(static_cast<std::size_t>(breed), DOGS.size() - 1)];
        }

        // as fast as the dogs' animations of each gait go (units a second), with the stance and gait of each
        constexpr std::array<SpeedPreset, 2> SHEPHERD_SPEED_PRESETS{{
            {"Walk", 66.0f, Types::ActorStance::Stand, Gait::Walk},
            {"Run", 320.0f, Types::ActorStance::Stand, Gait::Run},
        }};
        constexpr std::array<SpeedPreset, 3> HELLHOUND_SPEED_PRESETS{{
            {"Walk", 53.0f, Types::ActorStance::Stand, Gait::Walk},
            {"Trot", 136.0f, Types::ActorStance::Stand, Gait::Run},
            {"Run", 362.0f, Types::ActorStance::Stand, Gait::Sprint},
        }};

        bool IsDog(const Body& body)
        {
            return body.kind == Types::ActorKind::Dog;
        }

        bool IsVehicle(const Body& body)
        {
            return body.kind == Types::ActorKind::Vehicle;
        }
    }  // namespace

    std::string_view GetDogAnimation(DogBreed breed, Gait gait)
    {
        const auto& set = GetDogSet(breed);
        switch (gait)
        {
            case Gait::Idle:
                return set.idle;
            case Gait::Walk:
                return set.walk;
            case Gait::Run:
                break;
            case Gait::Sprint:
                if (!set.sprint.empty())
                    return set.sprint;
                break;
        }
        return set.run;
    }

    std::string_view GetDogDeath(DogBreed breed)
    {
        return GetDogSet(breed).death;
    }

    std::span<const SpeedPreset> GetSpeedPresets(const Body& body)
    {
        if (IsVehicle(body))
            return Components::KillstreakLogic::Get(body.killstreak).presets;
        if (!IsDog(body))
        {
            if (const auto ai = GetAiSet(body.set))
                return AI_SPEED_PRESETS[*ai];
            return SPEED_PRESETS;
        }
        if (body.breed == DogBreed::Hellhound)
            return HELLHOUND_SPEED_PRESETS;
        return SHEPHERD_SPEED_PRESETS;
    }

    Gait ToGait(Types::ActorGait gait)
    {
        switch (gait)
        {
            case Types::ActorGait::Walk:
                return Gait::Walk;
            case Types::ActorGait::Run:
                return Gait::Run;
            case Types::ActorGait::Sprint:
                return Gait::Sprint;
        }
        return Gait::Walk;
    }

    Types::ActorStance GetStretchStance(const Types::ActorStretch& stretch)
    {
        return stretch.crouched ? Types::ActorStance::Crouch : Types::ActorStance::Stand;
    }

    std::vector<SpeedPreset> GetGaits(const Body& body)
    {
        if (IsVehicle(body))
            return {};
        const auto presets = GetSpeedPresets(body);
        return {presets.begin(), presets.end()};
    }

    std::string GetGaitAnimation(const Body& body, Types::ActorGait gait, bool crouched,
                                 const std::function<bool(std::string_view)>& has)
    {
        return FindLocomotionAnimation(
            body, crouched && !IsDog(body) ? Types::ActorStance::Crouch : Types::ActorStance::Stand, ToGait(gait),
            Heading::Forward, has);
    }

    const Types::ActorStretch* GetStretch(const Types::Actor& actor, std::size_t point)
    {
        if (actor.stretches.empty())
            return nullptr;
        return &actor.stretches[std::min(point, actor.stretches.size() - 1)];
    }

    float GetStretchSpeed(const Types::Actor& actor, const Types::ActorStretch& stretch,
                          const AnimationLookup& animations)
    {
        auto speed = stretch.animation.empty() ? 0.0f : GetAnimationSpeed(stretch.animation, animations);
        // (one the game doesn't have: as fast as the players' rifle's of the gait)
        if (speed <= 0.0f)
            speed = GUESSED_SPEEDS[GetStanceColumn(GetStretchStance(stretch))]
                                  [static_cast<std::size_t>(ToGait(stretch.gait))];
        return speed * std::clamp(actor.pace, MIN_PACE_FROM_BEFORE, MAX_PACE_FROM_BEFORE) *
               std::clamp(stretch.pace, MIN_PACE, MAX_PACE);
    }

    void ResolveStretches(Types::Actor& actor, const Body& body, const std::function<bool(std::string_view)>& has)
    {
        for (auto& stretch : actor.stretches)
        {
            const auto look = GetLook(actor, stretch);
            stretch.animation =
                !look.empty() ? std::string(look) : GetGaitAnimation(body, stretch.gait, stretch.crouched, has);
        }
    }

    std::string_view GetLook(const Types::Actor& actor, const Types::ActorStretch& stretch)
    {
        if (!stretch.look.empty())
            return stretch.look;
        const auto look = std::ranges::find_if(actor.looks, [&](const Types::ActorLook& each) {
            return each.gait == stretch.gait && each.crouched == stretch.crouched;
        });
        return look != actor.looks.end() ? std::string_view(look->animation) : std::string_view();
    }

    std::optional<std::pair<Types::ActorGait, bool>> GetLookGait(const std::string& animation, const Body& body,
                                                                 const AnimationLookup& animations)
    {
        // as its locomotion's, else by how fast it goes: the body's gait whose speed is nearest
        if (const auto locomotion = FindLocomotion(animation); locomotion && locomotion->gait != Gait::Idle)
        {
            const auto gait = locomotion->gait == Gait::Sprint ? Types::ActorGait::Sprint
                              : locomotion->gait == Gait::Run  ? Types::ActorGait::Run
                                                               : Types::ActorGait::Walk;
            return std::pair(gait, locomotion->stance == Types::ActorStance::Crouch && !IsDog(body));
        }
        const auto speed = GetAnimationSpeed(animation, animations);
        if (speed < IDLE_SPEED)
            return std::nullopt;
        std::optional<std::pair<Types::ActorGait, bool>> nearest;
        float off = std::numeric_limits<float>::max();
        for (const auto& preset : GetGaits(body))
            if (const auto distance = std::abs(preset.speed - speed); distance < off)
            {
                off = distance;
                nearest = std::pair(preset.gait == Gait::Sprint ? Types::ActorGait::Sprint
                                    : preset.gait == Gait::Run  ? Types::ActorGait::Run
                                                                : Types::ActorGait::Walk,
                                    preset.stance == Types::ActorStance::Crouch);
            }
        return nearest;
    }

    namespace
    {
        // what a stretch plays and how fast, which a join is between (see HasJoin)
        bool GoesAlike(const Types::ActorStretch& a, const Types::ActorStretch& b)
        {
            return a.animation == b.animation && a.gait == b.gait && a.crouched == b.crouched && a.pace == b.pace;
        }

        // the joins from before of the stretches that changed since they were as `was`, and of those after them, are
        // joins of now (a new look comes in as joins do now)
        void ForgetJoinsAsBefore(Types::Actor& actor, const std::vector<Types::ActorStretch>& was)
        {
            auto& stretches = actor.stretches;
            for (std::size_t i = 0; i < stretches.size(); ++i)
                if (i >= was.size() || !GoesAlike(stretches[i], was[i]))
                {
                    stretches[i].joinAsBefore = false;
                    if (i + 1 < stretches.size())
                        stretches[i + 1].joinAsBefore = false;
                }
        }
    }  // namespace

    std::optional<std::pair<Types::ActorGait, bool>> SetLook(Types::Actor& actor, std::optional<std::size_t> stretch,
                                                             const std::string& animation, const Body& body,
                                                             const AnimationLookup& animations,
                                                             const std::function<bool(std::string_view)>& has)
    {
        if (!MakeStretches(actor, body, animations, has) || actor.stretches.empty())
            return std::nullopt;
        FitStretches(actor);
        const auto was = actor.stretches;
        // the gait it is a look of: the stretch's (or the path's first) becomes it
        const auto& on = actor.stretches[std::min(stretch.value_or(0), actor.stretches.size() - 1)];
        const auto gait = GetLookGait(animation, body, animations).value_or(std::pair(on.gait, on.crouched));
        if (stretch && *stretch < actor.stretches.size())
        {
            auto& one = actor.stretches[*stretch];
            one.gait = gait.first;
            one.crouched = gait.second;
            one.look = animation;
        }
        else
        {
            // the whole path: the actor's look of the gait, which its stretches of that gait take (their own taken
            // off); those of the gait the first stretch was at go at the look's gait
            const auto was = std::pair(on.gait, on.crouched);
            for (auto& each : actor.stretches)
                if (std::pair(each.gait, each.crouched) == was || std::pair(each.gait, each.crouched) == gait)
                {
                    each.gait = gait.first;
                    each.crouched = gait.second;
                    each.look.clear();
                }
            std::erase_if(actor.looks, [&](const Types::ActorLook& each) {
                return each.gait == gait.first && each.crouched == gait.second;
            });
            actor.looks.push_back({gait.first, gait.second, animation});
        }
        FitStretches(actor);
        ResolveStretches(actor, body, has);
        ForgetJoinsAsBefore(actor, was);
        return gait;
    }

    bool HasJoin(const Types::Actor& actor, std::size_t stretch)
    {
        const auto& stretches = actor.stretches;
        return stretch > 0 && stretch < stretches.size() && stretch + 1 < actor.path.size() &&
               !GoesAlike(stretches[stretch], stretches[stretch - 1]);
    }

    std::vector<Footfall> GetFootfalls(const AnimationInfo& info)
    {
        std::vector<Footfall> footfalls;
        for (const auto& notetrack : info.notetracks)
        {
            const std::string_view name = notetrack.name;
            if (!name.starts_with("footstep_"))
                continue;
            if (name.find("left") != std::string_view::npos)
                footfalls.push_back({notetrack.time, true});
            else if (name.find("right") != std::string_view::npos)
                footfalls.push_back({notetrack.time, false});
        }
        if (footfalls.empty())
            footfalls.assign(info.footfalls.begin(), info.footfalls.end());
        return footfalls;
    }

    std::uint32_t GetAutoJoinBlend(const Types::Actor& actor, std::size_t stretch, const AnimationLookup& animations)
    {
        if (stretch == 0 || stretch >= actor.stretches.size())
            return MIN_JOIN_BLEND;
        const auto& before = actor.stretches[stretch - 1];
        const auto& after = actor.stretches[stretch];
        const auto speedBefore = GetStretchSpeed(actor, before, animations);
        const auto speedAfter = GetStretchSpeed(actor, after, animations);
        // one step of the slower look, as fast as it plays there
        const auto& slower = speedAfter < speedBefore ? after : before;
        const auto speed = std::min(speedBefore, speedAfter);
        float step = 0.0f;
        if (const auto info = slower.animation.empty() ? std::nullopt : animations(slower.animation);
            info && info->duration > 0.0f)
        {
            const auto own = GetAnimationSpeed(slower.animation, animations);
            const auto rate = own > 0.0f ? std::clamp(speed / own, MIN_LOCOMOTION_RATE, MAX_LOCOMOTION_RATE) : 1.0f;
            const auto footfalls = GetFootfalls(*info);
            const auto steps = footfalls.size() >= 2 ? static_cast<float>(footfalls.size()) : 4.0f;
            step = info->duration / steps / rate;
        }
        auto blend =
            std::clamp(static_cast<std::uint32_t>(std::lround(step * 100.0f)) * 10u, MIN_JOIN_BLEND, MAX_JOIN_BLEND);
        if (before.crouched != after.crouched)
            blend = std::max(blend, LEGS_BLEND_STANCE);
        return blend;
    }

    std::uint32_t GetJoinBlend(const Types::Actor& actor, std::size_t stretch, const AnimationLookup& animations)
    {
        if (stretch >= actor.stretches.size())
            return MIN_JOIN_BLEND;
        const auto& own = actor.stretches[stretch];
        if (own.joinAsBefore)
            return LEGS_BLEND_TO_MOVING;
        return own.blend ? *own.blend : GetAutoJoinBlend(actor, stretch, animations);
    }

    void UpdateJoin(Types::ActorStretch& stretch)
    {
        if (!stretch.joinAsBefore)
            return;
        stretch.joinAsBefore = false;
        stretch.blend = LEGS_BLEND_TO_MOVING;
        stretch.blendAt = Types::ActorJoinAt::Centred;
        stretch.matchFeet = true;
    }

    bool MakeStretches(Types::Actor& actor, const Body& body, const AnimationLookup& animations,
                       const std::function<bool(std::string_view)>& has)
    {
        if (!actor.stretches.empty())
            return true;
        if (actor.movement != Types::ActorMovement::Path || actor.locomotion != Types::ActorLocomotion::Auto ||
            IsVehicle(body))
            return false;
        // (its one walk run at a speed of its own, as Run here makes it: at that speed)
        auto speed = actor.pathSpeed;
        std::vector<std::size_t> own;
        for (std::size_t k = 0; k < actor.clips.size(); ++k)
            if (const auto& clip = actor.clips[k]; clip.walkSpeed > 0.0f && !clip.picked)
                if (const auto locomotion = FindLocomotion(actor, clip.animation);
                    locomotion && locomotion->gait != Gait::Idle)
                    own.push_back(k);
        if (own.size() == 1)
            speed = actor.clips[own.front()].walkSpeed;
        const bool crouched = actor.stance == Types::ActorStance::Crouch && !IsDog(body);
        const auto choice = ChooseLocomotion(body, crouched ? Types::ActorStance::Crouch : Types::ActorStance::Stand,
                                             std::max(speed, IDLE_SPEED), 0.0f, animations);
        Types::ActorStretch stretch{.crouched = crouched};
        stretch.gait = choice.gait == Gait::Sprint ? Types::ActorGait::Sprint
                       : choice.gait == Gait::Run  ? Types::ActorGait::Run
                                                   : Types::ActorGait::Walk;
        stretch.animation = GetGaitAnimation(body, stretch.gait, crouched, has);
        const auto own_ = GetAnimationSpeed(stretch.animation, animations);
        actor.pace = own_ > 0.0f ? std::clamp(speed / own_, MIN_PACE_FROM_BEFORE, MAX_PACE_FROM_BEFORE) : 1.0f;
        actor.stretches.assign(std::max<std::size_t>(actor.path.size(), 1), stretch);
        if (own.size() == 1)
            actor.clips[own.front()].walkSpeed = 0.0f;
        return true;
    }

    void SetGait(Types::Actor& actor, std::size_t from, std::size_t to, Types::ActorGait gait, bool crouched,
                 const Body& body, const AnimationLookup& animations, const std::function<bool(std::string_view)>& has)
    {
        if (!MakeStretches(actor, body, animations, has))
            return;
        FitStretches(actor);
        const auto was = actor.stretches;
        for (auto i = from; i < std::min(to, actor.stretches.size()); ++i)
        {
            auto& stretch = actor.stretches[i];
            // (a look of its own is one of the gait it had)
            if (stretch.gait != gait || stretch.crouched != (crouched && !IsDog(body)))
                stretch.look.clear();
            stretch.gait = gait;
            stretch.crouched = crouched && !IsDog(body);
        }
        FitStretches(actor);
        ResolveStretches(actor, body, has);
        ForgetJoinsAsBefore(actor, was);
    }

    Types::ActorEvent MakeGunFire(std::uint32_t fireTime, Types::GunClass gun, std::uint32_t tick)
    {
        Types::ActorEvent fire{.type = Types::ActorEventType::Fire, .tick = tick, .shots = 5};
        if (fireTime > 0)
            fire.interval = fireTime;
        if (gun == Types::GunClass::RocketLauncher)
            fire.shots = 1;
        return fire;
    }

    std::string_view GetStanceIdle(Types::GunClass gun, Types::ActorStance stance)
    {
        return GetLocomotionSet(gun, stance).idle;
    }

    std::string FindStanceIdle(Types::GunClass gun, Types::ActorStance stance,
                               const std::function<bool(std::string_view)>& has)
    {
        for (const auto idle : {GetStanceIdle(gun, stance), GetStanceIdle(Types::GunClass::Rifle, stance)})
            if (has(idle))
                return std::string(idle);
        return {};
    }

    Heading GetHeading(float angle)
    {
        const auto turned = std::remainder(angle, 360.0f);
        if (std::abs(turned) <= 45.0f)
            return Heading::Forward;
        if (std::abs(turned) >= 135.0f)
            return Heading::Back;
        return turned > 0.0f ? Heading::Left : Heading::Right;
    }

    float GetHeadingAngle(Heading heading)
    {
        constexpr std::array<float, 4> ANGLES = {0.0f, 180.0f, 90.0f, -90.0f};
        return ANGLES[static_cast<std::size_t>(heading)];
    }

    std::string_view GetLocomotionAnimation(const Body& body, Types::ActorStance stance, Gait gait, Heading heading)
    {
        if (IsDog(body))
            return GetDogAnimation(body.breed, gait);
        if (IsVehicle(body))
            return Components::KillstreakLogic::GetMotion(Components::KillstreakLogic::Get(body.killstreak).vehicleClass,
                                                         gait);
        const auto& set = GetLocomotionSet(body, stance);
        const auto way = static_cast<std::size_t>(heading);
        switch (gait)
        {
            case Gait::Idle:
                return set.idle;
            case Gait::Walk:
                return set.walks[way];
            case Gait::Run:
                break;
            case Gait::Sprint:
                if (heading == Heading::Forward && !set.sprint.empty())
                    return set.sprint;
                break;
        }
        return set.runs[way];
    }

    std::string FindLocomotionAnimation(const Body& body, Types::ActorStance stance, Gait gait, Heading heading,
                                        const std::function<bool(std::string_view)>& has)
    {
        // (a dog falls back to the shepherd, a person to the rifle, an AI's set to low ready's, then the players')
        std::vector<Body> bodies{body};
        auto fallback = body;
        fallback.gun = Types::GunClass::Rifle;
        fallback.breed = DogBreed::Shepherd;
        if (!IsDog(body) && GetAiSet(body.set))
        {
            fallback.set = Types::ActorMovementSet::LowReady;
            bodies.push_back(fallback);
            fallback.set = Types::ActorMovementSet::Players;
        }
        bodies.push_back(fallback);
        for (const auto& each : bodies)
            if (const auto animation = GetLocomotionAnimation(each, stance, gait, heading); has(animation))
                return std::string(animation);
        return {};
    }

    std::optional<LocomotionAnimation> FindLocomotion(std::string_view animation)
    {
        // each name once, as the first gun, stance, gait and heading it is of
        static const auto index = [] {
            std::map<std::string_view, LocomotionAnimation, std::less<>> names;
            for (std::size_t gun = 0; gun < LOCOMOTION.size(); ++gun)
                for (const auto stance :
                     {Types::ActorStance::Stand, Types::ActorStance::Crouch, Types::ActorStance::Prone})
                    for (const auto gait : {Gait::Idle, Gait::Walk, Gait::Run, Gait::Sprint})
                        for (const auto heading : {Heading::Forward, Heading::Back, Heading::Left, Heading::Right})
                        {
                            const LocomotionAnimation found{static_cast<Types::GunClass>(gun), stance, gait, heading};
                            names.emplace(GetLocomotionAnimation(found.gun, stance, gait, heading), found);
                        }
            // the AI's
            for (const auto set :
                 {Types::ActorMovementSet::LowReady, Types::ActorMovementSet::Cqb, Types::ActorMovementSet::RunAndGun})
                for (const auto stance :
                     {Types::ActorStance::Stand, Types::ActorStance::Crouch, Types::ActorStance::Prone})
                    for (const auto gait : {Gait::Idle, Gait::Walk, Gait::Run, Gait::Sprint})
                        for (const auto heading : {Heading::Forward, Heading::Back, Heading::Left, Heading::Right})
                        {
                            Body body;
                            body.set = set;
                            names.emplace(GetLocomotionAnimation(body, stance, gait, heading),
                                          LocomotionAnimation{.stance = stance, .gait = gait, .heading = heading,
                                                              .set = set});
                        }
            // the dogs', standing and going forward
            for (std::size_t breed = 0; breed < DOGS.size(); ++breed)
                for (const auto gait : {Gait::Idle, Gait::Walk, Gait::Run, Gait::Sprint})
                {
                    const auto dog = static_cast<DogBreed>(breed);
                    names.emplace(GetDogAnimation(dog, gait), LocomotionAnimation{.gait = gait, .dog = dog});
                }
            // the killstreaks' motions: the moving ones go, the others stay
            for (const auto motion : Components::KillstreakLogic::MOTIONS)
                names.emplace(motion, LocomotionAnimation{.gait = Components::KillstreakLogic::GetMotionGait(motion),
                                                          .vehicle = true});
            return names;
        }();
        const auto found = index.find(animation);
        return found != index.end() ? std::optional(found->second) : std::nullopt;
    }

    bool HasWayToGo(const Types::Actor& actor)
    {
        return (actor.movement == Types::ActorMovement::Path && actor.path.size() >= 2) ||
               (actor.movement == Types::ActorMovement::Keyframes && actor.keys.size() >= 2);
    }

    std::optional<LocomotionAnimation> FindLocomotion(const Types::Actor& actor, std::string_view animation)
    {
        auto found = FindLocomotion(animation);
        if (found && !found->dog && found->set != Types::ActorMovementSet::Players &&
            actor.movementSet == Types::ActorMovementSet::Players)
            return std::nullopt;
        // an actor with nowhere to go has no speed to pick a gait by: its walks, runs and sprints play as they are, in
        // place (a killstreak's motions aren't animations)
        if (found && found->gait != Gait::Idle && !found->vehicle && !HasWayToGo(actor))
            return std::nullopt;
        return found;
    }

    float GetAnimationSpeed(std::string_view animation, const AnimationLookup& animations)
    {
        if (const auto info = animations(std::string(animation));
            info && info->motionPerPlay && info->duration > 0.0f &&
            glm::length(*info->motionPerPlay) >= MIN_MOTION_PER_PLAY)
            return glm::length(*info->motionPerPlay) / info->duration;
        const auto locomotion = FindLocomotion(animation);
        if (locomotion && locomotion->vehicle)
            return 0.0f;
        if (locomotion && locomotion->dog)
            return GetDogSet(*locomotion->dog).speeds[static_cast<std::size_t>(locomotion->gait)];
        if (const auto ai = locomotion ? GetAiSet(locomotion->set) : std::nullopt)
            return AI_GUESSED_SPEEDS[*ai][GetStanceColumn(locomotion->stance)][static_cast<std::size_t>(locomotion->gait)];
        if (locomotion)
            return GUESSED_SPEEDS[GetStanceColumn(locomotion->stance)][static_cast<std::size_t>(locomotion->gait)];
        // (another, such as a look of the campaign's: as far as the catalog has a play move it)
        if (const auto info = animations(std::string(animation));
            info && info->catalogMotion && info->duration > 0.0f &&
            glm::length(*info->catalogMotion) >= MIN_MOTION_PER_PLAY)
            return glm::length(*info->catalogMotion) / info->duration;
        return 0.0f;
    }

    namespace
    {
        // ChooseLocomotion for a body, with the animations it picks from and their speeds looked up once
        class LocomotionChooser
        {
           public:
            LocomotionChooser(const Body& body, const AnimationLookup& animations) : body(body), animations(animations)
            {
            }

            // a dog's (and a killstreak's) stands and goes forward alone
            bool IsDog() const
            {
                return ActorLogic::IsDog(body) || ActorLogic::IsVehicle(body);
            }

            // the animation of a gait and heading in a stance (not Auto), empty when the game has none
            const std::string& Find(Types::ActorStance stance, Gait gait, Heading heading)
            {
                auto& found =
                    names[GetStanceColumn(stance)][static_cast<std::size_t>(gait)][static_cast<std::size_t>(heading)];
                if (!found)
                    found = FindLocomotionAnimation(body, stance, gait, heading, [&](std::string_view name) {
                        return animations(std::string(name)).has_value();
                    });
                return *found;
            }

            float GetSpeed(const std::string& animation)
            {
                if (const auto known = speeds.find(animation); known != speeds.end())
                    return known->second;
                return speeds[animation] = GetAnimationSpeed(animation, animations);
            }

            // what the legs play at a speed and angle in a stance (Auto stands): the gait and heading, and the
            // animation as a name looked up here; a gait given (a stretch's) is played whatever the speed (a sprint
            // only standing and going forward, else running)
            struct Choice
            {
                const std::string* animation = nullptr;
                Gait gait = Gait::Idle;
                Heading heading = Heading::Forward;
            };
            Choice Choose(Types::ActorStance stance, float speed, float angle, std::optional<Gait> given = std::nullopt)
            {
                if (stance == Types::ActorStance::Auto || IsDog())
                    stance = Types::ActorStance::Stand;
                Choice choice{.heading = IsDog() ? Heading::Forward : GetHeading(angle)};
                if (speed >= IDLE_SPEED && given && *given != Gait::Idle && stance != Types::ActorStance::Prone)
                {
                    choice.gait = *given;
                    if (choice.gait == Gait::Sprint && !IsDog() &&
                        (stance != Types::ActorStance::Stand || choice.heading != Heading::Forward))
                        choice.gait = Gait::Run;
                }
                else if (speed >= IDLE_SPEED)
                {
                    choice.gait = Gait::Walk;
                    if (stance != Types::ActorStance::Prone)
                    {
                        const auto walk = GetSpeed(Find(stance, Gait::Walk, choice.heading));
                        const auto run = GetSpeed(Find(stance, Gait::Run, choice.heading));
                        if (speed > (walk + run) / 2.0f)
                            choice.gait = Gait::Run;
                        if (choice.gait == Gait::Run && stance == Types::ActorStance::Stand &&
                            choice.heading == Heading::Forward)
                        {
                            const auto& sprint = Find(stance, Gait::Sprint, Heading::Forward);
                            if (sprint != Find(stance, Gait::Run, Heading::Forward) &&
                                speed > (run + GetSpeed(sprint)) / 2.0f)
                                choice.gait = Gait::Sprint;
                        }
                    }
                }
                choice.animation = &Find(stance, choice.gait, choice.heading);
                return choice;
            }

            // how fast an animation plays at a speed: the speed against its own, within the rates (1 for an idle)
            float GetRate(const std::string& animation, float speed)
            {
                const auto own = GetSpeed(animation);
                return own > 0.0f ? std::clamp(speed / own, MIN_LOCOMOTION_RATE, MAX_LOCOMOTION_RATE) : 1.0f;
            }

           private:
            Body body;
            const AnimationLookup& animations;
            std::array<std::array<std::array<std::optional<std::string>, 4>, 4>, 3> names;
            std::map<std::string, float, std::less<>> speeds;
        };
    }  // namespace

    LocomotionChoice ChooseLocomotion(const Body& body, Types::ActorStance stance, float speed, float angle,
                                      const AnimationLookup& animations)
    {
        LocomotionChooser chooser(body, animations);
        const auto choice = chooser.Choose(stance, speed, angle);
        LocomotionChoice chosen{*choice.animation, choice.gait, choice.heading};
        if (choice.gait != Gait::Idle)
            chosen.rate = chooser.GetRate(chosen.animation, speed);
        return chosen;
    }

    namespace
    {
        // the time a clip shows at a tick, as Sample has it: seconds into its animation, going on while the clip
        // after it blends in
        float GetShownTime(const std::vector<Types::ActorClip>& clips, std::size_t index, std::uint32_t tick)
        {
            const auto& clip = clips[index];
            auto limit = clip.length;
            if (index + 1 < clips.size() && tick >= clips[index + 1].start)
                limit += clips[index + 1].blendIn;
            const auto elapsed = tick >= clip.start ? std::min(tick - clip.start, limit) : 0u;
            return clip.offset + elapsed / 1000.0f * std::max(clip.speed, 0.0f);
        }

        // the angle (as ClipWalk::angle) a clip's direction walks it at; none for Auto (as its animation goes)
        std::optional<float> GetDirectionAngle(Types::ActorWalkDirection direction)
        {
            switch (direction)
            {
                case Types::ActorWalkDirection::Forward:
                    return 0.0f;
                case Types::ActorWalkDirection::Back:
                    return 180.0f;
                case Types::ActorWalkDirection::Left:
                    return 90.0f;
                case Types::ActorWalkDirection::Right:
                    return -90.0f;
                case Types::ActorWalkDirection::Auto:
                    break;
            }
            return std::nullopt;
        }

        // How a clip walks with Locomotion Off, from its animation (as looked up)
        ClipWalk GetOwnWalk(const Types::ActorClip& clip, const std::optional<AnimationInfo>& animation)
        {
            if (!clip.moves)
                return {};
            const auto motion = animation && animation->motionPerPlay && animation->duration > 0.0f
                                    ? animation->motionPerPlay
                                    : std::nullopt;
            const bool measuredMoves = motion && glm::length(*motion) >= MIN_MOTION_PER_PLAY;

            ClipWalk walk;
            if (clip.walkSpeed > 0.0f)
                walk.speed = clip.walkSpeed;
            else if (measuredMoves)
                walk.speed = glm::length(*motion) / animation->duration;
            if (walk.speed <= 0.0f)
                return {};
            walk.angle = GetDirectionAngle(clip.direction)
                             .value_or(measuredMoves ? glm::degrees(std::atan2(motion->y, motion->x)) : 0.0f);
            return walk;
        }

        // What the legs and the path walked need to know of each of an actor's clips, looked up once
        struct ClipFacts
        {
            std::optional<AnimationInfo> animation;
            Types::AnimationKind kind = Types::AnimationKind::Other;
            // with Locomotion Auto, what a clip of the players' movement animations stands for
            std::optional<LocomotionAnimation> locomotion;
            bool legs = false;  // the legs play under it (an upper body clip) or are its body (a locomotion clip)
            std::vector<std::pair<float, Types::ActorStance>> poses;  // its anim_pose notetracks: time, stance
            std::optional<ClipWalk> walk;  // how it walks the actor along its path, if it does (see GetClipWalk)
            bool walksToEnd = false;  // as long as the clip is, else not once its animation has played if it doesn't
                                      // loop
            RootMotionUse rootMotionUse = RootMotionUse::None;  // see GetRootMotionUse
            bool rootMotion = false;  // it moves the actor by its animation's root motion, which the game has measured
            // that root motion: its animation's, or for a transition that turns the actor as far as it is to turn (see
            // TransitionLogic::GetTurnMotion)
            std::shared_ptr<const RootMotion> track;
            // it walks at the speeds of the actor's stretches (see GetStretchSpeed), not walk's
            bool stretched = false;
        };

        // the clips' facts, and what the walk along the path needs of its stretches: how fast each goes, how long it
        // waits at its first point, the gait and stance its locomotion plays there; and the fastest of them
        struct StretchFacts
        {
            std::vector<float> speeds;
            std::vector<std::uint32_t> waits;
            std::vector<Gait> gaits;
            std::vector<Types::ActorStance> stances;
            std::vector<std::string> looks;  // each stretch's look (see GetLook), empty for none
            float fastest = 0.0f;
            // each stretch's join as the walk goes through it (see HasJoin): its blend (0 for none, for one at a point
            // the actor stands at, or a turn or wait of the path's plan is at), where it sits, and whether the speed
            // eases through it (not a join from before's)
            std::vector<std::uint32_t> joins;
            std::vector<Types::ActorJoinAt> joinsAt;
            std::vector<bool> eases;
            // the turns and waits' stops of the path's plan at points, by start: from then on the walk goes as the
            // stretch from the point (but after a join from before)
            std::vector<std::pair<std::uint32_t, std::size_t>> takeOvers;
        };
        struct ActorFacts : std::vector<ClipFacts>
        {
            using std::vector<ClipFacts>::vector;
            StretchFacts stretches;
        };

        ActorFacts GetClipFacts(const Types::Actor& actor, const AnimationLookup& animations)
        {
            const auto& clips = actor.clips;
            const bool locomotion = actor.locomotion == Types::ActorLocomotion::Auto;
            ActorFacts facts(clips.size());
            // (with locomotion: a path from before gaits walks at pathSpeed)
            const bool stretches = locomotion && !actor.stretches.empty();
            if (stretches)
            {
                for (const auto& stretch : actor.stretches)
                {
                    facts.stretches.speeds.push_back(GetStretchSpeed(actor, stretch, animations));
                    facts.stretches.waits.push_back(stretch.wait);
                    facts.stretches.gaits.push_back(ToGait(stretch.gait));
                    facts.stretches.stances.push_back(GetStretchStance(stretch));
                    facts.stretches.looks.emplace_back(GetLook(actor, stretch));
                }
                facts.stretches.fastest = std::ranges::max(facts.stretches.speeds);
                // (a turn or wait of the path's plan at a point: the walk after it takes over from it)
                const auto planned = [&](std::size_t point) {
                    return std::ranges::any_of(clips, [&](const Types::ActorClip& clip) {
                        return (clip.chain == Types::ActorChain::Turn || clip.chain == Types::ActorChain::Stop) &&
                               clip.corner == point;
                    });
                };
                for (std::size_t i = 0; i < actor.stretches.size(); ++i)
                {
                    const auto& stretch = actor.stretches[i];
                    const bool joined = HasJoin(actor, i) && stretch.wait == 0 && !planned(i);
                    facts.stretches.joins.push_back(joined ? GetJoinBlend(actor, i, animations) : 0u);
                    facts.stretches.joinsAt.push_back(stretch.joinAsBefore ? Types::ActorJoinAt::After
                                                                           : stretch.blendAt);
                    facts.stretches.eases.push_back(!stretch.joinAsBefore);
                }
                for (const auto& clip : clips)
                    if ((clip.chain == Types::ActorChain::Turn || clip.chain == Types::ActorChain::Stop) &&
                        clip.corner && *clip.corner < actor.stretches.size() &&
                        !actor.stretches[*clip.corner].joinAsBefore)
                        facts.stretches.takeOvers.emplace_back(clip.start, *clip.corner);
                std::ranges::sort(facts.stretches.takeOvers);
            }
            for (std::size_t k = 0; k < clips.size(); ++k)
            {
                const auto& clip = clips[k];
                auto& fact = facts[k];
                fact.animation = animations(clip.animation);
                fact.kind = fact.animation ? fact.animation->kind : Types::AnimationKind::Other;
                // (a clip the user picked plays as picked)
                if (locomotion && fact.kind != Types::AnimationKind::Torso && !clip.picked)
                    fact.locomotion = FindLocomotion(actor, clip.animation);
                fact.legs = fact.kind == Types::AnimationKind::Torso || fact.locomotion;
                if (fact.animation)
                    for (const auto& notetrack : fact.animation->notetracks)
                        if (const auto stance = GetNotetrackStance(notetrack.name))
                            fact.poses.emplace_back(notetrack.time, *stance);

                if (fact.locomotion)
                {
                    // walks at the actor's speed (its stretches' on a path with gaits: the first's for the UI), unless
                    // it stands for an idle (and isn't given a speed)
                    fact.stretched = stretches && clip.walkSpeed <= 0.0f && !fact.locomotion->vehicle;
                    const auto speed = clip.walkSpeed > 0.0f ? clip.walkSpeed
                                       : fact.stretched      ? facts.stretches.speeds.front()
                                                             : actor.pathSpeed;
                    if (clip.moves && speed > 0.0f && (fact.locomotion->gait != Gait::Idle || clip.walkSpeed > 0.0f))
                    {
                        fact.walk = ClipWalk{
                            speed,
                            GetDirectionAngle(clip.direction).value_or(GetHeadingAngle(fact.locomotion->heading))};
                        fact.walksToEnd = true;
                    }
                    else
                        fact.stretched = false;
                    continue;
                }
                if (locomotion && fact.kind == Types::AnimationKind::Torso && clip.walkSpeed <= 0.0f)
                {
                    // walks on with the walk it interrupts: that of the locomotion clip before its run of upper body
                    // clips
                    auto before = k;
                    while (before > 0 && facts[before - 1].kind == Types::AnimationKind::Torso)
                        --before;
                    if (clip.moves && before > 0 && facts[before - 1].locomotion && facts[before - 1].walk)
                    {
                        auto walk = *facts[before - 1].walk;
                        if (const auto angle = GetDirectionAngle(clip.direction))
                            walk.angle = angle;
                        fact.walk = walk;
                        fact.walksToEnd = true;
                        fact.stretched = facts[before - 1].stretched;
                    }
                    continue;
                }
                if (const auto walk = GetOwnWalk(clip, fact.animation); walk.angle)
                    fact.walk = walk;
            }

            // which move it by their root motion: those that don't walk; on an actor standing still, which has no path
            // to walk, those that walk too but the looping ones (walks and runs, which walk in place as before); not
            // those of its locomotion or of the upper body alone, nor those of animations whose root stays where it
            // is (within half a unit and degree, as idles' do)
            const auto movement = actor.movement;
            for (std::size_t k = 0; k < clips.size(); ++k)
            {
                auto& fact = facts[k];
                const auto& animation = fact.animation;
                const bool walks = fact.walk && (movement != Types::ActorMovement::Still || clips[k].loop);
                // (a transition turning the actor turns it whether or not its animation does: MP's turns are turned in
                // code)
                const auto& turn = clips[k].turn;
                if (!animation || (!animation->rootMotion && !animation->delta && !turn) || fact.locomotion ||
                    fact.kind == Types::AnimationKind::Torso || movement == Types::ActorMovement::Attached || walks)
                    continue;
                if (!turn && animation->rootMotion &&
                    std::ranges::all_of(animation->rootMotion->samples, [](glm::vec3 sample) {
                        return glm::length(glm::vec2(sample)) < 0.5f && std::abs(sample.z) < 0.5f;
                    }))
                    continue;
                fact.rootMotionUse = clips[k].rootMotion ? RootMotionUse::Moves : RootMotionUse::Stays;
                // (a start, setting off from a stop, turns as its animation does)
                const bool start = clips[k].chain == Types::ActorChain::Start ||
                                   (animation->from && animation->from->movement == Types::AnimationMovement::Stop &&
                                    animation->to && animation->to->movement != Types::AnimationMovement::Stop &&
                                    animation->to->movement != Types::AnimationMovement::Unknown);
                fact.track = turn ? std::make_shared<const RootMotion>(TransitionLogic::GetTurnMotion(
                                        animation->rootMotion.get(), animation->notetracks, *turn, start))
                                  : animation->rootMotion;
                fact.rootMotion = fact.rootMotionUse == RootMotionUse::Moves && fact.track &&
                                  fact.track->samples.size() >= 2 && animation->duration > 0.0f;
            }
            return facts;
        }

        // the speed an actor walks its path at, for its corners: its fastest walking clip's, else its locomotion's (a
        // walk's without)
        float GetPathSpeed(const Types::Actor& actor, const ActorFacts& facts)
        {
            float speed = 0.0f;
            for (std::size_t k = 0; k < facts.size(); ++k)
                if (facts[k].walk)
                    speed = std::max(speed, (facts[k].stretched ? facts.stretches.fastest : facts[k].walk->speed) *
                                                std::max(actor.clips[k].speed, 0.0f));
            if (speed <= 0.0f && facts.stretches.fastest > 0.0f)
                speed = facts.stretches.fastest;
            if (speed <= 0.0f)
                speed = actor.locomotion == Types::ActorLocomotion::Auto ? actor.pathSpeed : SPEED_PRESETS[0].speed;
            return speed;
        }

        // whether a clip is one of a path's transitions (TransitionLogic::PlanPath), which the path goes on from where
        // they put the actor on it
        bool IsPathTransition(const Types::ActorClip& clip)
        {
            return clip.chain == Types::ActorChain::Start || clip.chain == Types::ActorChain::Turn ||
                   clip.chain == Types::ActorChain::Stop;
        }

        RoundedPath RoundActorPath(const Types::Actor& actor, const ActorFacts& facts)
        {
            // (a turn of its own turns it at its corner)
            std::vector<std::size_t> sharp;
            for (const auto& clip : actor.clips)
                if ((clip.chain == Types::ActorChain::Turn || clip.chain == Types::ActorChain::Stop) && clip.corner)
                    sharp.push_back(*clip.corner);
            return RoundPath(actor.path, GetCornerRadius(actor, GetPathSpeed(actor, facts)), sharp);
        }

        // Where the legs are under a clip of an upper body animation (or a locomotion clip) at a tick: as the actor's
        // stance has them, or (Auto) as the last anim_pose notetrack the clip's animation has passed in its play says
        // (in a play after the first, the last of the play before goes on), else its name
        Types::ActorStance GetClipStance(const Types::Actor& actor, const ActorFacts& facts, std::size_t index,
                                         std::uint32_t tick)
        {
            if (actor.stance != Types::ActorStance::Auto)
                return actor.stance;
            const auto& clip = actor.clips[index];
            const auto& animation = facts[index].animation;
            const auto named = GetStance(clip.animation, animation);
            if (!animation || animation->duration <= 0.0f)
                return named;
            const auto& poses = facts[index].poses;
            if (poses.empty())
                return named;
            const auto plays = GetShownTime(actor.clips, index, tick) / animation->duration;
            const auto play = clip.loop ? std::floor(plays) : 0.0f;
            const auto within = clip.loop ? plays - play : std::min(plays, 1.0f);
            std::optional<Types::ActorStance> posed;
            for (const auto& [time, stance] : poses)
                if (time <= within)
                    posed = stance;
            return posed ? *posed : play >= 1.0f ? poses.back().second : named;
        }

        // the steps the path walked (and the locomotion's speed) is summed in: the samples change only by clip edges
        // and blends, so steps of 10 ms are exact enough
        constexpr std::uint32_t WALK_STEP = 10;
        // how near the end of its path an actor that a stop (or turn) of its own puts there arrives: those land it as
        // near as a millisecond of their move
        constexpr float ARRIVAL_TOLERANCE = 2.0f;
        // points of a path closer together than this are one point given twice: a stop
        constexpr float STOP_DISTANCE = 0.01f;

        // A killstreak's flights along its path, if it speeds up and slows down (a helicopter, the RC-XD; see
        // VehicleLogic::GetProfile): its clips as their legs, those that walk it at their speeds
        std::optional<Components::VehicleLogic::Profile> GetFlightProfile(const Types::Actor& actor,
                                                                          const ActorFacts& facts,
                                                                          const RoundedPath& path)
        {
            if (actor.movement != Types::ActorMovement::Path || actor.clips.empty())
                return std::nullopt;
            const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models);
            if (!killstreak || Components::KillstreakLogic::Get(*killstreak).acceleration <= 0.0f)
                return std::nullopt;
            const auto& info = Components::KillstreakLogic::Get(*killstreak);
            std::vector<Components::VehicleLogic::Leg> legs;
            for (std::size_t k = 0; k < actor.clips.size(); ++k)
            {
                const auto& clip = actor.clips[k];
                Components::VehicleLogic::Leg leg{.start = clip.start, .end = clip.End(), .blend = clip.blendIn};
                leg.dies = Components::KillstreakLogic::IsDeathMotion(clip.animation);
                if (facts[k].walk && facts[k].walk->speed * clip.speed > 0.0f)
                    leg.speed = facts[k].walk->speed * clip.speed;
                legs.push_back(leg);
            }
            return Components::VehicleLogic::GetProfile(legs, path.length,
                                                        Components::KillstreakLogic::GetTopSpeed(info));
        }
        std::optional<Components::VehicleLogic::Profile> GetFlightProfile(const Types::Actor& actor,
                                                                          const ActorFacts& facts)
        {
            if (actor.movement != Types::ActorMovement::Path ||
                !Components::KillstreakLogic::FindByModels(actor.models))
                return std::nullopt;
            return GetFlightProfile(actor, facts, RoundActorPath(actor, facts));
        }

        // What the clips shown at a tick walk: the speed along the path of all of them, and the speed and direction
        // of those the legs play under or are the body of, weighed as they are shown, and their weight. A clip walks
        // while it plays (not held at its end: a looping clip holds after its length too, and the clip before one
        // blending in goes on for the blend; one that doesn't loop holds after a play, unless it walks to its end).
        // Those that walk at the actor's stretches' speeds (see GetStretchSpeed) aren't in the speeds: stretched is how
        // much of the stretch's speed they add (their clips' speeds, as shown), which depends on where along the path
        // the actor is (see PathWalker).
        struct Stepping
        {
            float speed = 0.0f;
            glm::vec2 direction{0.0f};
            float legsSpeed = 0.0f;
            glm::vec2 legsDirection{0.0f};
            float legsWeight = 0.0f;
            float stretched = 0.0f;
            float legsStretched = 0.0f;
        };
        Stepping GetStepping(const Types::Actor& actor, const ActorFacts& facts, std::uint32_t tick)
        {
            Stepping stepping;
            std::array<Types::ActorAnimationSample, 2> samples{};
            const auto count = Sample(actor, tick, samples);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto& sample = samples[i];
                const auto& clip = *sample.clip;
                const auto& fact = facts[static_cast<std::size_t>(sample.clip - actor.clips.data())];
                if (fact.legs)
                    stepping.legsWeight += sample.weight;
                if (!fact.walk || sample.weight <= 0.0f || tick < clip.start)
                    continue;
                const auto elapsed = tick - clip.start;
                const auto limit = i == 0 ? clip.length : clip.length + samples[0].clip->blendIn;
                const auto duration = fact.animation ? fact.animation->duration : 0.0f;
                if (elapsed >= limit || (!fact.walksToEnd && !clip.loop && duration > 0.0f && sample.time >= duration))
                    continue;
                const auto angle = glm::radians(*fact.walk->angle);
                const auto way = sample.weight * glm::vec2(std::cos(angle), std::sin(angle));
                stepping.direction += way;
                if (fact.legs)
                    stepping.legsDirection += way;
                if (fact.stretched)
                {
                    const auto share = clip.speed * sample.weight;
                    stepping.stretched += share;
                    if (fact.legs)
                        stepping.legsStretched += share;
                    continue;
                }
                const auto speed = fact.walk->speed * clip.speed * sample.weight;
                stepping.speed += speed;
                if (fact.legs)
                    stepping.legsSpeed += speed;
            }
            return stepping;
        }

        // eases in and out from 0 to 1
        float Ease(float amount)
        {
            amount = std::clamp(amount, 0.0f, 1.0f);
            return amount * amount * (3.0f - 2.0f * amount);
        }

        // Walks an actor's path step by step from its first clip's start (as GetWalk sums it): how far it has gone at
        // the start of each step and how fast it goes through it, with its stretches' speeds where it is (see
        // StretchFacts), easing from one stretch's to the next one's through their join's blend (see HasJoin), and
        // standing at a stretch's first point for its wait once it gets there.
        class PathWalker
        {
           public:
            PathWalker(const Types::Actor& actor, const ActorFacts& facts, const RoundedPath* path)
                : actor(actor), facts(facts)
            {
                // (a killstreak's flights at their own speeds)
                flights = path ? GetFlightProfile(actor, facts, *path) : GetFlightProfile(actor, facts);
                if (flights || facts.stretches.speeds.empty() || !path)
                    return;
                // (a wait the path's plan stops, stands and sets off at: its own, not the walker's)
                for (const auto& clip : actor.clips)
                    if (clip.chain == Types::ActorChain::Stop && clip.corner)
                        waited.push_back(*clip.corner);
                // where each stretch starts (its point's distance along the path), in order
                for (std::size_t i = 0; i + 1 < actor.path.size() && i < facts.stretches.speeds.size(); ++i)
                    if (const auto distance = GetPointDistance(*path, i))
                        starts.emplace_back(*distance, i);
                std::ranges::sort(starts);
                joinStarts.resize(facts.stretches.speeds.size());
            }

            // the stretch (index into facts.stretches) at a distance along the path
            std::optional<std::size_t> StretchAt(float distance) const
            {
                if (starts.empty())
                    return std::nullopt;
                std::size_t found = starts.front().second;
                for (const auto& [start, index] : starts)
                    if (start <= distance + 1e-3f)
                        found = index;
                return std::min(found, facts.stretches.speeds.size() - 1);
            }

            // goes on through a step of `ticks` from tick t: returns how far (along the path) it went, holding at a
            // point whose stretch waits until its wait is over
            float Step(std::uint32_t t, std::uint32_t ticks)
            {
                const auto stepping = GetStepping(actor, facts, t);
                lastStepping = stepping;
                if (flights)
                {
                    const auto gone = flights->Distance(t + ticks) - walked;
                    walked += gone;
                    return gone;
                }
                Advance(t, stepping);
                if (t < waitUntil)
                {
                    waiting = true;
                    return 0.0f;
                }
                waiting = false;
                const auto speed = stepping.speed + stepping.stretched * stretchedSpeed;
                auto gone = speed * static_cast<float>(ticks) / 1000.0f;
                // (a point it reaches in the step whose stretch waits: it stands there)
                if (stepping.stretched > 0.0f)
                    for (const auto& [start, index] : starts)
                        if (index < facts.stretches.waits.size() && facts.stretches.waits[index] > 0 &&
                            std::ranges::find(waited, index) == waited.end() && start >= walked - 1e-3f &&
                            start <= walked + gone)
                        {
                            waited.push_back(index);
                            gone = std::max(start - walked, 0.0f);
                            waitUntil = t + ticks + facts.stretches.waits[index];
                            break;
                        }
                walked += gone;
                return gone;
            }

            float Walked() const
            {
                return walked;
            }
            bool Waiting() const
            {
                return waiting;
            }
            const Stepping& LastStepping() const
            {
                return lastStepping;
            }
            // whether it walks a killstreak's flights (see GetFlightProfile)
            bool Flies() const
            {
                return flights.has_value();
            }
            // as of the last step: the stretch the walk goes as (the one at the distance walked, or the next once
            // its join's blend has started or a turn or wait of the path's plan at its point), none without stretches;
            // and how fast its stretched part goes there, units a second at 1x (eased through a join's blend)
            std::optional<std::size_t> Stretch() const
            {
                return current;
            }
            float StretchedSpeed() const
            {
                return stretchedSpeed;
            }
            // the speed of the legs (of what they play under or are the body of) as of the last step
            float LegsSpeed(const Stepping& stepping) const
            {
                return stepping.legsSpeed + stepping.legsStretched * stretchedSpeed;
            }
            // the tick each stretch's join's blend started at, none for one that hasn't (or has no join)
            const std::vector<std::optional<std::uint32_t>>& JoinStarts() const
            {
                return joinStarts;
            }

           private:
            // where the walk is in its stretches and their joins at the start of a step at tick t
            void Advance(std::uint32_t t, const Stepping& stepping)
            {
                if (starts.empty())
                {
                    current.reset();
                    stretchedSpeed = 0.0f;
                    return;
                }
                const auto& joins = facts.stretches.joins;
                const auto& speeds = facts.stretches.speeds;
                const auto speedNow = [&] {
                    return easing && t < easing->start + easing->length
                               ? glm::mix(easing->from, speeds[easing->to],
                                          Ease(static_cast<float>(t - easing->start) / easing->length))
                               : speeds[*current];
                };
                auto stretch = *StretchAt(walked);
                // (past a turn or a wait of the path's plan at a point: as the stretch from it)
                for (const auto& [tick, point] : facts.stretches.takeOvers)
                    if (tick <= t)
                        stretch = std::max(stretch, point);
                if (current)
                    stretch = std::max(stretch, *current);
                // (a join passed without its blend started, as a join from before's: there, as it passes, at once)
                for (std::size_t j = current ? *current + 1 : 0; j <= stretch && j < joins.size(); ++j)
                    if (joins[j] > 0 && !joinStarts[j])
                        joinStarts[j] = t;
                current = stretch;
                if (easing && easing->to < *current)
                    easing.reset();
                // the joins ahead whose blends start now: as far before their point as the part of the blend before it
                // goes at the speeds it eases through (centred: half of it, on average a fifth of the way from the one
                // speed to the other; before: all of it, halfway); not past a point without one
                for (const auto& [start, j] : starts)
                {
                    if (j <= *current || j >= joins.size())
                        continue;
                    if (joins[j] == 0 || !facts.stretches.eases[j])
                        break;
                    const auto from = speedNow();
                    const auto blend = static_cast<float>(joins[j]) / 1000.0f;
                    float lead = 0.0f;
                    if (facts.stretches.joinsAt[j] == Types::ActorJoinAt::Centred)
                        lead = blend / 2.0f * (from + (speeds[j] - from) * 0.1875f);
                    else if (facts.stretches.joinsAt[j] == Types::ActorJoinAt::Before)
                        lead = blend * (from + speeds[j]) / 2.0f;
                    if (start - walked > lead * stepping.stretched + 1e-3f)
                        break;
                    joinStarts[j] = t;
                    easing = Easing{from, j, t, joins[j]};
                    current = j;
                }
                stretchedSpeed = speedNow();
            }

            // a join's ease of the speed: from the speed when its blend started to its stretch's
            struct Easing
            {
                float from = 0.0f;
                std::size_t to = 0;
                std::uint32_t start = 0;
                std::uint32_t length = 1;
            };

            const Types::Actor& actor;
            const ActorFacts& facts;
            std::vector<std::pair<float, std::size_t>> starts;
            std::vector<std::size_t> waited;
            float walked = 0.0f;
            std::uint32_t waitUntil = 0;
            bool waiting = false;
            Stepping lastStepping;
            std::optional<std::size_t> current;
            float stretchedSpeed = 0.0f;
            std::optional<Easing> easing;
            std::vector<std::optional<std::uint32_t>> joinStarts;
            std::optional<Components::VehicleLogic::Profile> flights;
        };

        // the placement between an actor's keys (as EvaluatePlacement has it)
        Types::ActorPlacement GetKeyedPlacement(const std::vector<Types::ActorKey>& keys, std::uint32_t tick)
        {
            if (tick <= keys.front().tick)
                return {keys.front().position, keys.front().yaw};
            if (tick >= keys.back().tick)
                return {keys.back().position, keys.back().yaw};
            const auto next = std::ranges::find_if(keys, [&](const Types::ActorKey& key) { return key.tick > tick; });
            const auto index = static_cast<std::size_t>(next - keys.begin());
            const auto& from = keys[index - 1];
            const auto& to = keys[index];
            const auto t = static_cast<float>(tick - from.tick) / static_cast<float>(to.tick - from.tick);
            // through the keys around them, so moves don't stop at every key
            const auto& before = keys[index >= 2 ? index - 2 : index - 1];
            const auto& after = keys[std::min(index + 1, keys.size() - 1)];
            const auto position = glm::catmullRom(before.position, from.position, to.position, after.position, t);
            const auto yaw = from.yaw + std::remainder(to.yaw - from.yaw, 360.0f) * t;
            return {position, yaw};
        }

        // how fast the locomotion goes at a tick, and which way against the way the actor faces (as ClipWalk::angle)
        struct Motion
        {
            float speed = 0.0f;
            float angle = 0.0f;
            // walking a stretch of its path: the stretch (as it goes as it, see PathWalker::Stretch), its gait (while
            // it goes) and stance
            std::optional<std::size_t> stretch;
            std::optional<Gait> gait;
            std::optional<Types::ActorStance> stance;
            const std::string* look = nullptr;  // the stretch's look, if it has one
        };

        float GetDegrees(glm::vec2 direction)
        {
            return glm::length(direction) > 1e-4f ? glm::degrees(std::atan2(direction.y, direction.x)) : 0.0f;
        }

        // How fast an actor's locomotion goes over the timeline, in the steps GetWalk sums its path in from its first
        // clip's start to a tick: on a path the speed of the clips the legs play under or are the body of, none once
        // the path is walked to its end; with keys, how fast it goes between them. And from that, how far into a
        // movement animation its rates have played it by then.
        class LocomotionTrack
        {
           public:
            LocomotionTrack(const Types::Actor& actor, const ActorFacts& facts, std::uint32_t tick)
                : actor(actor), facts(facts), start(actor.clips.front().start), tick(tick)
            {
                const bool path = actor.movement == Types::ActorMovement::Path;
                rounded = RoundActorPath(actor, facts);
                pathLength = rounded.length;
                walker.emplace(actor, facts, &rounded);
                for (auto t = start; t < tick; t += WALK_STEP)
                {
                    if (!path)
                    {
                        speeds.push_back(GetKeyedMotion(t).speed);
                        continue;
                    }
                    const auto walked = walker->Walked();
                    const auto ticks = std::min(WALK_STEP, tick - t);
                    const auto gone = walker->Step(t, ticks);
                    walkedAt.push_back(walked);
                    walking.push_back(gone * 1000.0f / static_cast<float>(std::max(ticks, 1u)));
                    waitingAt.push_back(walker->Waiting());
                    stretchAt.push_back(walker->Stretch());
                    stretchedAt.push_back(walker->StretchedSpeed());
                    speeds.push_back(
                        walker->Waiting() ? 0.0f : GetLegsSpeed(walker->LastStepping(), walked, stretchedAt.back()));
                }
            }

            // at a tick up to the one it was made for
            Motion At(std::uint32_t t) const
            {
                if (actor.movement != Types::ActorMovement::Path)
                    return GetKeyedMotion(t);
                // the path walked by then, as GetWalk sums it, and the stretch it goes as
                float walked = 0.0f;
                bool waiting = false;
                auto stretch = walker->StretchAt(0.0f);
                auto stretched = stretch ? facts.stretches.speeds[*stretch] : 0.0f;
                if (t > start && !walkedAt.empty())
                {
                    const auto step = std::min<std::size_t>((t - start - 1) / WALK_STEP, walkedAt.size() - 1);
                    walked =
                        walkedAt[step] + walking[step] * static_cast<float>(t - start - step * WALK_STEP) / 1000.0f;
                    waiting = waitingAt[step];
                    stretch = stretchAt[step];
                    stretched = stretchedAt[step];
                }
                const auto stepping = GetStepping(actor, facts, t);
                Motion motion{waiting ? 0.0f : GetLegsSpeed(stepping, walked, stretched),
                              GetDegrees(stepping.legsDirection)};
                // (on a stretch with a gait: that gait while it goes, in the stretch's stance)
                if (stretch && stepping.legsStretched > 0.0f)
                {
                    motion.stretch = stretch;
                    motion.stance = facts.stretches.stances[*stretch];
                    if (motion.speed >= IDLE_SPEED)
                        motion.gait = facts.stretches.gaits[*stretch];
                    if (!facts.stretches.looks[*stretch].empty())
                        motion.look = &facts.stretches.looks[*stretch];
                }
                return motion;
            }

            // when each stretch's join's blend started, as the walk went (see PathWalker::JoinStarts)
            const std::vector<std::optional<std::uint32_t>>& JoinStarts() const
            {
                return walker->JoinStarts();
            }

            // seconds into an animation of a speed (units a second) its rates have played it by the tick (from a tick
            // on, or the first clip's start; up to a tick before it), at the speed against its own (within the rates)
            float GetTime(float animationSpeed, std::uint32_t from = 0,
                          std::optional<std::uint32_t> to = std::nullopt) const
            {
                const auto until = std::min(to.value_or(tick), tick);
                float time = 0.0f;
                for (std::size_t step = 0; step < speeds.size(); ++step)
                {
                    const auto rate = animationSpeed > 0.0f ? std::clamp(speeds[step] / animationSpeed,
                                                                         MIN_LOCOMOTION_RATE, MAX_LOCOMOTION_RATE)
                                                            : 1.0f;
                    const auto t = std::max(start + static_cast<std::uint32_t>(step) * WALK_STEP, from);
                    const auto stepEnd = std::min(start + static_cast<std::uint32_t>(step + 1) * WALK_STEP, until);
                    if (stepEnd > t)
                        time += rate * static_cast<float>(stepEnd - t) / 1000.0f;
                }
                return time;
            }

           private:
            float GetLegsSpeed(const Stepping& stepping, float walked, float stretched) const
            {
                if (stepping.legsWeight <= 0.0f || walked >= pathLength - 1e-3f)
                    return 0.0f;
                return (stepping.legsSpeed + stepping.legsStretched * stretched) / stepping.legsWeight;
            }

            Motion GetKeyedMotion(std::uint32_t t) const
            {
                const auto& keys = actor.keys;
                if (keys.size() < 2)
                    return {};
                // over a few ticks around it
                constexpr std::uint32_t AROUND = 5;
                const auto from = t > AROUND ? t - AROUND : 0u, to = t + AROUND;
                const auto way =
                    glm::vec2(GetKeyedPlacement(keys, to).position - GetKeyedPlacement(keys, from).position);
                const auto speed = glm::length(way) / (static_cast<float>(to - from) / 1000.0f);
                return {speed, std::remainder(GetDegrees(way) - GetKeyedPlacement(keys, t).yaw, 360.0f)};
            }

            const Types::Actor& actor;
            const ActorFacts& facts;
            std::uint32_t start = 0;
            std::uint32_t tick = 0;
            float pathLength = 0.0f;
            RoundedPath rounded;
            std::optional<PathWalker> walker;
            std::vector<float> speeds;    // the locomotion's at each step
            std::vector<float> walkedAt;  // the path walked by each step, and how fast it is walked there
            std::vector<float> walking;
            std::vector<bool> waitingAt;  // standing at a point for its stretch's wait
            // the stretch the walk goes as at each step, and how fast its stretched part goes (see PathWalker)
            std::vector<std::optional<std::size_t>> stretchAt;
            std::vector<float> stretchedAt;
        };

        // A mix of animations for the legs: the locomotion's (idles, movement) by name and the full body clip they go
        // on with by its index, their weights adding up to 1
        struct LegsPart
        {
            std::ptrdiff_t clip = -1;
            std::string animation;
            Gait gait = Gait::Idle;
            float weight = 0.0f;
        };
        using LegsMix = std::vector<LegsPart>;

        // a mix blending to one animation, by an amount from 0 to 1
        LegsMix BlendTo(const LegsMix& mix, const LegsPart& to, float amount)
        {
            LegsMix blended;
            for (auto part : mix)
            {
                part.weight *= 1.0f - amount;
                blended.push_back(std::move(part));
            }
            const auto same = std::ranges::find_if(
                blended, [&](const LegsPart& part) { return part.clip == to.clip && part.animation == to.animation; });
            if (same != blended.end())
                same->weight += amount;
            else
                blended.push_back({to.clip, to.animation, to.gait, amount});
            std::erase_if(blended, [](const LegsPart& part) { return part.weight <= 1e-6f; });
            return blended;
        }

        // how far back from a tick the changes of the legs blending into each other are looked for; a chain of them
        // longer than that starts as if the animation before it had been there all along
        constexpr std::uint32_t STANCE_LOOKBACK = 10 * LEGS_BLEND_STANCE;

        // what the legs play at a tick of a stretch: an animation of the locomotion, in a stance
        struct LegsTarget
        {
            const std::string* animation = nullptr;
            Types::ActorStance stance = Types::ActorStance::Stand;
            Gait gait = Gait::Idle;
            std::optional<std::size_t> stretch;  // of the path, walking it

            bool operator==(const LegsTarget& other) const
            {
                return *animation == *other.animation && stance == other.stance;
            }
        };

        // the ticks the legs take to blend from one animation to another, as the game's (BG_SetNewAnimation): stance
        // changes hold, to an animation that moves, from one, between ones that don't; walking on from one stretch of
        // the path into the next through its join, over the join's blend
        std::uint32_t GetLegsBlend(const LegsTarget& from, const LegsTarget& to, const StretchFacts& stretches)
        {
            if (from.gait != Gait::Idle && to.gait != Gait::Idle && from.stretch && to.stretch &&
                *to.stretch > *from.stretch && *to.stretch < stretches.joins.size() && stretches.joins[*to.stretch] > 0)
                return stretches.joins[*to.stretch];
            if (from.stance != to.stance)
                return LEGS_BLEND_STANCE;
            if (to.gait != Gait::Idle)
                return LEGS_BLEND_TO_MOVING;
            return from.gait != Gait::Idle ? LEGS_BLEND_AFTER_MOVING : LEGS_BLEND;
        }

        // What the legs play at a tick through a stretch of clips they play under (upper body ones) or are the body of
        // (locomotion ones), first to last, one after the other, as the game blends its legs from what they played to
        // what they play now, each blend from the mix there was as it started: from the full body clip before the
        // stretch (if any, before an upper body clip), once the first clip has blended in over it (handOver), to the
        // locomotion's animation; to another where that changes: the stance, and with locomotion the gait or heading.
        struct StretchLegs
        {
            LegsMix mix;
            std::optional<std::size_t> fullBody;
            std::uint32_t handOver = 0;
            std::uint32_t handOverTime = 0;
        };
        StretchLegs GetStretchLegs(const Types::Actor& actor, const ActorFacts& facts, std::size_t first,
                                   std::size_t last, std::uint32_t tick, LocomotionChooser& chooser,
                                   const LocomotionTrack* track)
        {
            const auto& clips = actor.clips;
            // the stance at a tick: that of the stretch's clip shown then (the first before it starts)
            const auto stanceAt = [&](std::uint32_t t) {
                auto index = first;
                for (auto k = first; k <= last; ++k)
                    if (clips[k].start <= t)
                        index = k;
                return GetClipStance(actor, facts, index, t);
            };
            const auto targetAt = [&](std::uint32_t t) {
                const auto motion = track ? track->At(t) : Motion{};
                auto stance = chooser.IsDog() ? Types::ActorStance::Stand : stanceAt(t);
                // (walking a stretch of its path: in the stretch's stance, where its own is Auto)
                if (motion.stance && actor.stance == Types::ActorStance::Auto && !chooser.IsDog())
                    stance = *motion.stance;
                auto choice = chooser.Choose(stance, motion.speed, motion.angle, motion.gait);
                // (a stretch's look in place of its gait's own, going forward at that gait)
                if (motion.look && motion.gait && choice.gait == *motion.gait && choice.heading == Heading::Forward)
                    choice.animation = motion.look;
                return LegsTarget{choice.animation,
                                  stance == Types::ActorStance::Auto ? Types::ActorStance::Stand : stance, choice.gait,
                                  motion.stretch};
            };
            const auto partOf = [](const LegsTarget& target) {
                return LegsPart{-1, *target.animation, target.gait, 1.0f};
            };

            StretchLegs legs;
            const auto& start = clips[first];
            if (facts[first].kind == Types::AnimationKind::Torso && first > 0 &&
                facts[first - 1].kind == Types::AnimationKind::FullBody)
                legs.fullBody = first - 1;
            // the full body clip goes on until the first clip has blended in over it, then hands over to the
            // locomotion: in the game's least time to one that moves, else from a walk if the clip walks, else from a
            // stand
            const auto handOver = start.start + (legs.fullBody ? start.blendIn : 0);
            legs.handOver = handOver;
            if (tick < handOver)
            {
                legs.mix = legs.fullBody ? LegsMix{{static_cast<std::ptrdiff_t>(*legs.fullBody), {}, Gait::Idle, 1.0f}}
                                         : LegsMix{partOf(targetAt(tick))};
                return legs;
            }
            const auto handOverTarget = targetAt(handOver);
            legs.handOverTime = LEGS_BLEND;
            if (handOverTarget.gait != Gait::Idle)
                legs.handOverTime = LEGS_BLEND_TO_MOVING;
            else if (legs.fullBody)
            {
                const auto& animation = facts[*legs.fullBody].animation;
                if (animation && animation->motionPerPlay &&
                    glm::length(*animation->motionPerPlay) >= MIN_MOTION_PER_PLAY)
                    legs.handOverTime = LEGS_BLEND_AFTER_MOVING;
            }

            // the ticks the legs change at after the hand over (not long before the tick): where what they play is
            // another than at the tick before, looked for in steps and then tick by tick (a change and back within a
            // step goes unseen)
            struct Change
            {
                std::uint32_t tick = 0;
                LegsTarget from, to;
            };
            std::vector<Change> changes;
            const auto from = std::max(handOver, tick > STANCE_LOOKBACK ? tick - STANCE_LOOKBACK : 0u);
            auto previous = targetAt(from);
            for (auto t = from; t < tick;)
            {
                const auto next = std::min(t + WALK_STEP, tick);
                if (!(targetAt(next) == previous))
                    for (auto u = t + 1; u <= next; ++u)
                        if (const auto now = targetAt(u); !(now == previous))
                        {
                            changes.push_back({u, previous, now});
                            previous = now;
                        }
                t = next;
            }

            // each blend from the mix there was as it started
            struct Blend
            {
                LegsMix from;
                std::optional<LegsPart> to;
                std::uint32_t start = 0;
                std::uint32_t time = 1;
            };
            const auto mixAt = [](const Blend& blend, std::uint32_t t) {
                if (!blend.to)
                    return blend.from;
                const auto amount = std::clamp(
                    (static_cast<double>(t) - static_cast<double>(blend.start)) / static_cast<double>(blend.time), 0.0,
                    1.0);
                return BlendTo(blend.from, *blend.to, static_cast<float>(amount));
            };
            Blend blend;
            if (legs.fullBody && from == handOver)
                blend = {{{static_cast<std::ptrdiff_t>(*legs.fullBody), {}, Gait::Idle, 1.0f}},
                         partOf(handOverTarget),
                         handOver,
                         legs.handOverTime};
            else
                blend.from = {partOf(targetAt(from))};
            for (const auto& change : changes)
                blend = {mixAt(blend, change.tick), partOf(change.to), change.tick,
                         GetLegsBlend(change.from, change.to, facts.stretches)};
            legs.mix = mixAt(blend, tick);
            return legs;
        }

        // Where a look coming in through a join is to start on the next foot: as far
        // through the step after the foot the outgoing look came down on last (that foot's footfall in the incoming
        // look, on to its next) as the outgoing look is through its own, so the other foot comes down next; seconds
        // into the incoming look, none when either has no footfalls
        std::optional<float> MatchStep(const AnimationInfo& outgoing, float outgoingTime, const AnimationInfo& incoming)
        {
            const auto out = GetFootfalls(outgoing), in = GetFootfalls(incoming);
            if (out.size() < 2 || in.size() < 2 || outgoing.duration <= 0.0f || incoming.duration <= 0.0f)
                return std::nullopt;
            const auto wrap = [](float phase) { return phase - std::floor(phase); };
            const auto phase = wrap(outgoingTime / outgoing.duration);
            // the outgoing look's last footfall and the next, and how far it is from the one to the other
            std::size_t last = out.size() - 1;
            for (std::size_t i = 0; i < out.size(); ++i)
                if (out[i].time <= phase)
                    last = i;
            const auto from = out[last].time;
            const auto to = out[(last + 1) % out.size()].time;
            const auto step = wrap(to - from);
            const auto through = step > 0.0f ? std::clamp(wrap(phase - from) / step, 0.0f, 1.0f) : 0.0f;
            // the same foot's in the incoming look, on to its next
            const auto same = std::ranges::find(in, out[last].left, &Footfall::left);
            if (same == in.end())
                return std::nullopt;
            const auto index = static_cast<std::size_t>(same - in.begin());
            const auto inStep = wrap(in[(index + 1) % in.size()].time - same->time);
            return wrap(same->time + through * inStep) * incoming.duration;
        }
    }  // namespace

    std::vector<std::string> GetLegsLibrary(const Types::Actor& actor, const Body& body,
                                            const AnimationLookup& animations)
    {
        std::vector<std::string> library;
        const auto has = [&](std::string_view name) { return animations(std::string(name)).has_value(); };
        const auto add = [&](std::string animation) {
            if (!animation.empty() && std::ranges::find(library, animation) == library.end())
                library.push_back(std::move(animation));
        };
        constexpr std::array STANCES{Types::ActorStance::Stand, Types::ActorStance::Crouch, Types::ActorStance::Prone};
        const auto facts = GetClipFacts(actor, animations);
        const bool moves =
            actor.movement == Types::ActorMovement::Path || actor.movement == Types::ActorMovement::Keyframes;
        // (a killstreak's motions are no animations)
        if (IsVehicle(body))
            return library;
        if (IsDog(body))
        {
            // its idle, and its walk, run and sprint when it moves: as its locomotion, and no legs under upper body
            // clips
            if (actor.locomotion == Types::ActorLocomotion::Auto &&
                std::ranges::any_of(facts, [](const ClipFacts& fact) { return fact.locomotion.has_value(); }))
                for (const auto gait : {Gait::Idle, Gait::Walk, Gait::Run, Gait::Sprint})
                    if (gait == Gait::Idle || moves)
                        add(FindLocomotionAnimation(body, Types::ActorStance::Stand, gait, Heading::Forward, has));
            // (and its stretches' looks)
            if (actor.locomotion == Types::ActorLocomotion::Auto && moves)
                for (const auto& look : facts.stretches.looks)
                    add(look);
            return library;
        }
        if (actor.locomotion != Types::ActorLocomotion::Auto)
        {
            if (std::ranges::any_of(facts,
                                    [](const ClipFacts& fact) { return fact.kind == Types::AnimationKind::Torso; }))
                for (const auto stance : STANCES)
                    add(FindStanceIdle(body.gun, stance, has));
            return library;
        }
        if (std::ranges::none_of(facts, &ClipFacts::legs))
            return library;

        // the stances it may take: its own, else standing and those of its clips' names and anim_pose notetracks
        std::array<bool, 3> stances{};
        if (actor.stance != Types::ActorStance::Auto)
            stances[GetStanceColumn(actor.stance)] = true;
        else
        {
            stances[0] = true;
            for (std::size_t k = 0; k < facts.size(); ++k)
            {
                if (!facts[k].legs)
                    continue;
                stances[GetStanceColumn(GetStance(actor.clips[k].animation, facts[k].animation))] = true;
                for (const auto& pose : facts[k].poses)
                    stances[GetStanceColumn(pose.second)] = true;
            }
            // and its stretches'
            for (const auto stance : facts.stretches.stances)
                stances[GetStanceColumn(stance)] = true;
        }
        for (const auto stance : STANCES)
        {
            if (!stances[GetStanceColumn(stance)])
                continue;
            add(FindLocomotionAnimation(body, stance, Gait::Idle, Heading::Forward, has));
            if (moves)
                for (const auto gait : {Gait::Walk, Gait::Run, Gait::Sprint})
                    for (const auto heading : {Heading::Forward, Heading::Back, Heading::Left, Heading::Right})
                        add(FindLocomotionAnimation(body, stance, gait, heading, has));
        }
        // (and its stretches' looks)
        if (moves)
            for (const auto& look : facts.stretches.looks)
                add(look);
        return library;
    }

    Layers GetLayers(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations, const Body& body)
    {
        Layers layers;
        layers.face = GetFace(actor, tick, animations);
        layers.clipCount = Sample(actor, tick, layers.clips);
        // (a killstreak has no legs: its motions are no animations)
        if (layers.clipCount == 0 || IsVehicle(body))
            return layers;
        const auto& clips = actor.clips;
        const auto facts = GetClipFacts(actor, animations);
        std::optional<std::size_t> legged;
        for (std::size_t i = 0; i < layers.clipCount; ++i)
        {
            const auto index = static_cast<std::size_t>(layers.clips[i].clip - clips.data());
            layers.upperBody[i] = facts[index].kind == Types::AnimationKind::Torso;
            layers.locomotion[i] = facts[index].locomotion.has_value();
            // (a dog has no legs under upper body clips, only its locomotion)
            if (facts[index].legs && (!IsDog(body) || facts[index].locomotion))
                legged = index;
        }
        // (a game without the players' animations, or the dogs', has no legs for them)
        if (!legged ||
            FindLocomotionAnimation(body, Types::ActorStance::Stand, Gait::Idle, Heading::Forward,
                                    [&](std::string_view name) { return animations(std::string(name)).has_value(); })
                .empty())
            return layers;

        // the stretch of upper body and locomotion clips the one shown is in (two shown are next to each other, so in
        // the same stretch)
        auto first = *legged, last = *legged;
        while (first > 0 && facts[first - 1].legs)
            --first;
        while (last + 1 < clips.size() && facts[last + 1].legs)
            ++last;
        LocomotionChooser chooser(body, animations);
        // with locomotion, how fast it goes along its path while its clips walk it, or between its keys
        std::optional<LocomotionTrack> track;
        if (actor.locomotion == Types::ActorLocomotion::Auto &&
            ((actor.movement == Types::ActorMovement::Path &&
              std::ranges::any_of(facts, [](const ClipFacts& fact) { return fact.legs && fact.walk; })) ||
             (actor.movement == Types::ActorMovement::Keyframes && actor.keys.size() >= 2)))
            track.emplace(actor, facts, tick);
        const auto stretch = GetStretchLegs(actor, facts, first, last, tick, chooser, track ? &*track : nullptr);

        // the idles from the stretch's start, looping; the movement animations as far as their rates have played them
        // (after a path's transition, from the stretch's start, as the game starts its run again after a turn); the
        // full body clip on from where it was shown as the first clip blended in (see Sample), until it has handed over
        const auto idleTime = static_cast<float>(tick - std::min(tick, clips[first].start)) / 1000.0f;
        const auto movingFrom = first > 0 && IsPathTransition(clips[first - 1]) ? clips[first].start : 0u;
        const auto motion = track ? track->At(tick) : Motion{};
        // (a look that came in through a join on the next foot, see MatchStep: from where that put it as the join's
        // blend started; the look before it as far as it had got then, the same way)
        std::function<float(const std::string&, std::uint32_t, int)> lookTime = [&](const std::string& animation,
                                                                                    std::uint32_t at, int depth) {
            const auto speed = chooser.GetSpeed(animation);
            std::optional<std::size_t> latest;
            if (depth < 8 && track && !actor.stretches.empty())
            {
                const auto& starts = track->JoinStarts();
                for (std::size_t j = 1; j < starts.size() && j < actor.stretches.size(); ++j)
                    if (starts[j] && *starts[j] <= at && *starts[j] >= movingFrom && facts.stretches.joins[j] > 0 &&
                        facts.stretches.eases[j] && actor.stretches[j].matchFeet &&
                        actor.stretches[j].animation == animation && actor.stretches[j - 1].animation != animation &&
                        (!latest || *starts[j] >= *starts[*latest]))
                        latest = j;
            }
            if (!latest)
                return track->GetTime(speed, movingFrom, at);
            const auto start = *track->JoinStarts()[*latest];
            const auto& outgoing = actor.stretches[*latest - 1].animation;
            const auto outgoingInfo = animations(outgoing), incomingInfo = animations(animation);
            const auto matched = outgoingInfo && incomingInfo
                                     ? MatchStep(*outgoingInfo, lookTime(outgoing, start, depth + 1), *incomingInfo)
                                     : std::nullopt;
            return matched.value_or(track->GetTime(speed, movingFrom, start)) + track->GetTime(speed, start, at);
        };
        float total = 0.0f;
        for (const auto& part : stretch.mix)
        {
            if (part.clip < 0 && part.animation.empty())
                continue;
            LegsSample sample{.animation = part.animation, .time = idleTime, .weight = part.weight};
            if (part.clip >= 0)
            {
                const auto& clip = clips[static_cast<std::size_t>(part.clip)];
                sample.clip = &clip;
                sample.animation.clear();
                const auto shownFor = clip.length + clips[first].blendIn;
                const auto since = [&](std::uint32_t t) { return t - std::min(t, clip.start); };
                const auto elapsed = tick < stretch.handOver
                                         ? std::min(since(tick), shownFor)
                                         : std::min(since(stretch.handOver), shownFor) +
                                               std::min(tick - stretch.handOver, stretch.handOverTime);
                sample.time = clip.offset + elapsed / 1000.0f * std::max(clip.speed, 0.0f);
                sample.rate = std::max(clip.speed, 0.0f);
            }
            else if (part.gait != Gait::Idle)
            {
                sample.time = track ? lookTime(part.animation, tick, 0) : idleTime;
                sample.rate = chooser.GetRate(part.animation, motion.speed);
            }
            total += sample.weight;
            layers.legs.push_back(std::move(sample));
        }
        for (auto& sample : layers.legs)
            sample.weight /= total;
        return layers;
    }

    TreeWeights WeighLayers(const Layers& layers, const SpineLookup& spines)
    {
        TreeWeights weights;
        float upper = 0.0f, full = 0.0f, locomotion = 0.0f;
        for (std::size_t i = 0; i < layers.clipCount; ++i)
        {
            (layers.upperBody[i] ? upper : full) += layers.clips[i].weight;
            if (layers.locomotion[i])
                locomotion += layers.clips[i].weight;
        }
        // (a locomotion clip without legs, left out for the harness, is a full body clip like any other)
        const bool legs = !layers.legs.empty() && upper + locomotion > 0.0f;
        // the legs: the full body clips and the legs under the upper body clips, as much as the full body clips and a
        // hundredth of the upper body ones together; the locomotion clips' bodies are the legs
        const auto legsWeight = full + LEGS_WEIGHT * upper;

        const auto add = [&](const Types::ActorClip* clip, const std::string& animation, float time, float weight) {
            const auto same = std::ranges::find_if(weights.animations, [&](const TreeWeight& entry) {
                return clip ? entry.clip == clip : !entry.clip && entry.animation == animation;
            });
            if (same != weights.animations.end())
                same->weight += weight;
            else
                weights.animations.push_back({clip, animation, time, weight});
        };
        const auto addSpine = [&](PoseLogic::SpineKind kind, float weight) {
            if (kind == PoseLogic::SpineKind::Lined)
                weights.lined += weight;
            else if (kind == PoseLogic::SpineKind::AtRest)
                weights.resting += weight;
        };

        for (std::size_t i = 0; i < layers.clipCount; ++i)
        {
            const auto& sample = layers.clips[i];
            if (legs && layers.locomotion[i])
                continue;
            const auto spine = spines(sample.clip->animation);
            if (legs && !layers.upperBody[i])
            {
                // of the legs; the stabilizer is its own for its own share alone, not the one under the upper body
                add(sample.clip, {}, sample.time, legsWeight * sample.weight);
                addSpine(spine, legsWeight * sample.weight);
                continue;
            }
            add(sample.clip, {}, sample.time, sample.weight);
            if (!legs || spine != PoseLogic::SpineKind::AtRest)
            {
                addSpine(spine, sample.weight);
                continue;
            }
            // Under an upper body clip that leaves the stabilizer alone, the legs move it, as on players; legs that
            // don't (made for SP bodies) have it aimed ahead for the clip as the players' legs hold it, each by its
            // share (a clip going on as the legs until they hand over leaves it at rest). Under legs that hand over to
            // an idle moving it, it is weighed as part of the legs, so it blends evenly to the idle's.
            float still = 0.0f;
            for (const auto& part : layers.legs)
                if (spines(part.clip ? part.clip->animation : part.animation) != PoseLogic::SpineKind::Moves)
                    still += part.weight;
            const auto share = sample.weight * (still >= 1.0f - 1e-6f ? 1.0f : LEGS_WEIGHT);
            for (const auto& part : layers.legs)
            {
                if (spines(part.clip ? part.clip->animation : part.animation) == PoseLogic::SpineKind::Moves)
                    continue;
                if (part.clip)
                {
                    weights.resting += share * part.weight;
                    continue;
                }
                const auto same =
                    std::ranges::find(weights.aimed, part.animation, &std::pair<std::string, float>::first);
                if (same != weights.aimed.end())
                    same->second += share * part.weight;
                else
                    weights.aimed.emplace_back(part.animation, share * part.weight);
            }
        }
        // (as the locomotion clips' bodies, legs made for SP bodies, the AI's movement sets', line the stabilizer up on
        // MP bodies and spine rigs as SP clips do; under the upper body clips it is as they have it above)
        if (legs)
            for (const auto& part : layers.legs)
            {
                add(part.clip, part.animation, part.time, legsWeight * (upper + locomotion) * part.weight);
                if (spines(part.clip ? part.clip->animation : part.animation) == PoseLogic::SpineKind::Lined)
                    weights.lined += legsWeight * locomotion * part.weight;
            }

        // the face, as much as the clips together (whatever the legs weigh: it moves bones they don't)
        float clips = 0.0f;
        for (std::size_t i = 0; i < layers.clipCount; ++i)
            clips += layers.clips[i].weight;
        for (const auto& sample : layers.face)
        {
            const auto weight = clips * sample.weight;
            if (weight <= 0.0f)
                continue;
            const auto same = std::ranges::find(weights.faces, sample.animation, &FaceWeight::animation);
            if (same == weights.faces.end())
                weights.faces.push_back({sample.animation, sample.time, sample.loop, weight});
            else
            {
                if (weight > same->weight)
                {
                    same->time = sample.time;
                    same->loop = sample.loop;
                }
                same->weight += weight;
            }
        }
        return weights;
    }

    bool IsDualWieldAnimation(std::string_view animation)
    {
        return (animation.starts_with("pb_") || animation.starts_with("pt_")) &&
               animation.find("_dw_") != std::string_view::npos;
    }

    bool GripsLeftGun(const TreeWeights& weights)
    {
        float total = 0.0f, dualWield = 0.0f;
        for (const auto& entry : weights.animations)
        {
            total += entry.weight;
            if (IsDualWieldAnimation(entry.clip ? entry.clip->animation : entry.animation))
                dualWield += entry.weight;
        }
        return total <= 0.0f || dualWield < total * (1.0f - LEFT_GUN_FREE_SHARE);
    }

    bool IsProneShown(const TreeWeights& weights)
    {
        float total = 0.0f, prone = 0.0f;
        for (const auto& entry : weights.animations)
        {
            total += entry.weight;
            if (GetNameStance(entry.clip ? entry.clip->animation : entry.animation) == Types::ActorStance::Prone)
                prone += entry.weight;
        }
        return prone > total * 0.5f;
    }

    float GetCornerRadius(float speed)
    {
        return std::clamp(speed * CORNER_TIME, MIN_CORNER_RADIUS, MAX_CORNER_RADIUS);
    }

    float GetCornerRadius(const Types::Actor& actor, float speed)
    {
        if (const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models))
            return Components::VehicleLogic::GetTurnRadius(Components::KillstreakLogic::Get(*killstreak), speed);
        return GetCornerRadius(speed);
    }

    namespace
    {
        // the stretch is shorter than this: nothing (a point given twice), or no telling which way it goes
        constexpr float TINY = 1e-4f;

        // where a piece of a rounded path is a distance along it, and the way it goes there
        glm::vec3 GetPieceAt(const RoundedPath::Piece& piece, float along, glm::vec3& tangent)
        {
            if (piece.radius <= 0.0f)
            {
                tangent = piece.direction;
                return piece.start + piece.direction * along;
            }
            const auto angle = along / piece.radius;
            tangent = std::cos(angle) * piece.direction + std::sin(angle) * piece.normal;
            return piece.start +
                   piece.radius * (std::sin(angle) * piece.direction + (1.0f - std::cos(angle)) * piece.normal);
        }

        // how far a rounded corner's arc reaches along the stretches on either side of its point
        float GetCornerReach(const RoundedPath::Piece& arc)
        {
            return arc.radius * std::tan(arc.length / arc.radius / 2.0f);
        }

        // a point the path goes straight on through: the way to it and on from it the same, or (a stretch too short to
        // tell its way by) the point on the line between the ones either side
        bool IsStraightOn(glm::vec3 from, glm::vec3 point, glm::vec3 next)
        {
            const auto in = point - from, out = next - point;
            const auto inLength = glm::length(in), outLength = glm::length(out);
            if (inLength <= TINY || outLength <= TINY || glm::dot(in, out) <= 0.0f)
                return false;
            if (std::acos(std::clamp(glm::dot(in / inLength, out / outLength), -1.0f, 1.0f)) < 1e-3f)
                return true;
            const auto line = next - from;
            return glm::length(glm::cross(in, line)) < 1e-3f * glm::length(line);
        }
    }  // namespace

    RoundedPath RoundPath(const std::vector<glm::vec3>& path, float radius, std::span<const std::size_t> sharp)
    {
        RoundedPath rounded;
        if (path.empty())
            return rounded;
        rounded.origin = path.front();

        // the points, each with the index of its last copy in the path (one given twice in a row is a stop; the first
        // point is where the path starts anyway)
        struct Point
        {
            glm::vec3 position;
            std::size_t last = 0;
            bool stop = false;
        };
        std::vector<Point> points;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            if (!points.empty() && glm::distance(points.back().position, path[i]) < STOP_DISTANCE)
            {
                points.back().last = i;
                points.back().stop = points.size() > 1;
                continue;
            }
            points.push_back({path[i], i});
        }

        const auto add = [&](RoundedPath::Piece piece) {
            if (piece.length <= TINY)
                return;
            piece.distance = rounded.length;
            rounded.length += piece.length;
            rounded.pieces.push_back(piece);
        };
        // a straight piece to a point (the index of the point it goes to)
        const auto addStraight = [&](glm::vec3 from, glm::vec3 to, std::size_t point) {
            const auto length = glm::distance(from, to);
            if (length > TINY)
                add({.start = from, .direction = (to - from) / length, .length = length, .point = point});
        };
        // the way the path goes where it has got to
        const auto heading = [&]() -> std::optional<glm::vec3> {
            if (rounded.pieces.empty())
                return std::nullopt;
            glm::vec3 tangent;
            GetPieceAt(rounded.pieces.back(), rounded.pieces.back().length, tangent);
            return glm::normalize(tangent);
        };

        // where the stretch to the next point starts (past the arc of the corner before it), and the point it is from
        // (or where the curve leaving a stop ended), which half of it is for each corner; all of it for the corner at
        // one end when the other is a stop or a point it goes straight on through (no corner of its own)
        auto from = points.front().position;
        auto anchor = from;
        bool afterStraight = false;
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            const auto point = points[i].position;
            const bool fromStraight = std::exchange(afterStraight, false);
            const auto first = i > 0 ? points[i - 1].last + 1 : 0;
            if (i + 1 == points.size())
            {
                addStraight(from, point, first);
                break;
            }
            const auto next = points[i + 1].position;

            if (points[i].stop)
            {
                // from a stop, curving from the way it came onto the way to the next point, half as wide as a corner
                // walked through (it sets off from standing)
                addStraight(from, point, first);
                from = anchor = point;
                const auto way = heading();
                if (!way)
                    continue;
                const auto toNext = next - point;
                const auto ahead = glm::dot(toNext, *way);
                const auto across = toNext - ahead * *way;
                const auto aside = glm::length(across);
                glm::vec3 normal;
                if (aside > TINY)
                    normal = across / aside;
                else if (ahead >= 0.0f)
                    continue;  // straight on
                else
                {
                    // straight back: round to the left
                    normal = glm::vec3(-way->y, way->x, 0.0f);
                    if (glm::length(normal) <= TINY)
                        continue;
                    normal = glm::normalize(normal);
                }
                // no wider than leaves the next point outside its circle (or on it: then the curve ends there, as where
                // a path was cut on a curve has it)
                auto curve = radius / 2.0f;
                if (aside > TINY)
                    curve = std::min(curve, (ahead * ahead + aside * aside) / (2.0f * aside));
                if (curve <= TINY)
                    continue;
                // round until the line on to the next point leaves it at a tangent: in the plane of the way it came
                // (a) and across it (b), a sin t + (r - b) cos t = r, the line going on from there
                const double a = ahead, b = aside, r = curve;
                const auto reach = std::hypot(a, r - b);
                const auto shift = std::atan2(r - b, a);
                const auto lean = std::asin(std::clamp(r / reach, -1.0, 1.0));
                std::optional<double> turn;
                for (const auto candidate : {lean - shift, glm::pi<double>() - lean - shift})
                {
                    auto t = std::fmod(candidate, glm::two_pi<double>());
                    if (t < 0.0)
                        t += glm::two_pi<double>();
                    const glm::dvec2 on(r * std::sin(t), r * (1.0 - std::cos(t))), tangent(std::cos(t), std::sin(t));
                    if (glm::dot(glm::dvec2(a, b) - on, tangent) >= -1e-3 && (!turn || t < *turn))
                        turn = t;
                }
                if (!turn)
                    continue;
                const RoundedPath::Piece curveOut{.start = point,
                                                  .direction = *way,
                                                  .normal = normal,
                                                  .radius = curve,
                                                  .length = curve * static_cast<float>(*turn),
                                                  .point = points[i].last,
                                                  .stop = true};
                add(curveOut);
                glm::vec3 tangent;
                from = anchor = GetPieceAt(curveOut, curveOut.length, tangent);
                continue;
            }

            // a corner a turn of the actor's turns it at: none
            if (std::ranges::find(sharp, points[i].last) != sharp.end())
            {
                addStraight(from, point, first);
                from = anchor = point;
                continue;
            }

            // a corner walked through: an arc meeting both stretches at a tangent, reaching at most half of each
            const auto in = point - anchor, out = next - point;
            const auto inLength = glm::length(in), outLength = glm::length(out);
            if (inLength <= TINY || outLength <= TINY)
            {
                addStraight(from, point, first);
                from = anchor = point;
                continue;
            }
            const auto u = in / inLength, v = out / outLength;
            const auto angle = std::acos(std::clamp(glm::dot(u, v), -1.0f, 1.0f));
            // (straight on, or straight back: turns on the spot)
            const bool straightOn = IsStraightOn(anchor, point, next);
            if (straightOn || angle > glm::pi<float>() - 1e-3f)
            {
                addStraight(from, point, first);
                from = anchor = point;
                afterStraight = straightOn;
                continue;
            }
            const auto half = std::tan(angle / 2.0f);
            const bool wholeOut = points[i + 1].stop ||
                                  (i + 2 < points.size() && std::ranges::find(sharp, points[i + 1].last) == sharp.end() &&
                                   IsStraightOn(point, next, points[i + 2].position));
            const auto reach = std::min(
                {radius * half, fromStraight ? inLength : inLength / 2.0f, wholeOut ? outLength : outLength / 2.0f});
            const auto arcRadius = reach / half;
            const auto arcStart = point - u * reach;
            addStraight(from, arcStart, first);
            add({.start = arcStart,
                 .direction = u,
                 .normal = glm::normalize(v - u * glm::dot(u, v)),
                 .radius = arcRadius,
                 .length = arcRadius * angle,
                 .point = points[i].last});
            from = point + v * reach;
            anchor = point;
        }
        return rounded;
    }

    RoundedPath GetRoundedPath(const Types::Actor& actor, const AnimationLookup& animations)
    {
        return RoundActorPath(actor, GetClipFacts(actor, animations));
    }

    float GetPathLength(const Types::Actor& actor, const AnimationLookup& animations)
    {
        return GetRoundedPath(actor, animations).length;
    }

    std::vector<glm::vec3> GetPathLine(const RoundedPath& path)
    {
        std::vector<glm::vec3> line{path.origin};
        for (const auto& piece : path.pieces)
        {
            // every 5 degrees of an arc
            const auto steps =
                piece.radius > 0.0f
                    ? std::max(1, static_cast<int>(std::ceil(piece.length / piece.radius / glm::radians(5.0f))))
                    : 1;
            glm::vec3 tangent;
            for (int step = 1; step <= steps; ++step)
                line.push_back(GetPieceAt(piece, piece.length * static_cast<float>(step) / steps, tangent));
        }
        return line;
    }

    ClipWalk GetClipWalk(const Types::ActorClip& clip, const AnimationLookup& animations)
    {
        return GetOwnWalk(clip, animations(clip.animation));
    }

    ClipWalk GetClipWalk(const Types::Actor& actor, const Types::ActorClip& clip, const AnimationLookup& animations)
    {
        // (a copy of one of its clips by its id)
        auto index = static_cast<std::size_t>(&clip - actor.clips.data());
        if (index >= actor.clips.size())
            index = static_cast<std::size_t>(std::ranges::find(actor.clips, clip.id, &Types::ActorClip::id) -
                                             actor.clips.begin());
        if (index >= actor.clips.size())
            return GetClipWalk(clip, animations);
        return GetClipFacts(actor, animations)[index].walk.value_or(ClipWalk{});
    }

    namespace
    {
        // How far the clips have walked the path by each of some ticks (in order), as GetWalk sums it: in its steps
        // from the first clip's start
        std::vector<float> GetWalkedDistances(const Types::Actor& actor, const ActorFacts& facts,
                                              const std::vector<std::uint32_t>& ticks)
        {
            std::vector<float> distances(ticks.size(), 0.0f);
            if (actor.clips.empty() ||
                std::ranges::none_of(facts, [](const ClipFacts& fact) { return fact.walk.has_value(); }))
                return distances;
            // (a killstreak's flights: as far as their speeds have taken it)
            if (const auto flights = GetFlightProfile(actor, facts))
            {
                for (std::size_t i = 0; i < ticks.size(); ++i)
                    distances[i] = flights->Distance(ticks[i]);
                return distances;
            }
            const auto start = actor.clips.front().start;
            std::size_t next = 0;
            while (next < ticks.size() && ticks[next] <= start)
                ++next;
            // (with stretches, at the speed of the stretch it is on)
            const bool stretched = !facts.stretches.speeds.empty() && actor.movement == Types::ActorMovement::Path;
            const auto rounded = stretched ? RoundActorPath(actor, facts) : RoundedPath{};
            PathWalker walker(actor, facts, stretched ? &rounded : nullptr);
            for (auto t = start; next < ticks.size(); t += WALK_STEP)
            {
                const auto walked = walker.Walked();
                const auto gone = walker.Step(t, WALK_STEP);
                // the ticks within the step: what was walked before it and the part of it up to them
                for (; next < ticks.size() && ticks[next] <= t + WALK_STEP; ++next)
                    distances[next] = walked + gone * static_cast<float>(ticks[next] - t) / WALK_STEP;
            }
            return distances;
        }

        // How far keys have moved an actor over the ground by each of some ticks (in order), from the first: in steps
        // like the path walked
        std::vector<float> GetKeyedTravel(const std::vector<Types::ActorKey>& keys,
                                          const std::vector<std::uint32_t>& ticks)
        {
            std::vector<float> travel(ticks.size(), 0.0f);
            if (keys.size() < 2 || ticks.empty())
                return travel;
            const auto at = [&](std::uint32_t t) { return glm::vec2(GetKeyedPlacement(keys, t).position); };
            float gone = 0.0f;
            std::size_t next = 1;
            auto here = at(ticks.front());
            for (auto t = ticks.front(); next < ticks.size(); t += WALK_STEP)
            {
                for (; next < ticks.size() && ticks[next] <= t + WALK_STEP; ++next)
                    travel[next] = gone + glm::distance(here, at(ticks[next]));
                const auto there = at(t + WALK_STEP);
                gone += glm::distance(here, there);
                here = there;
            }
            return travel;
        }

        // The angle the clips walk at a tick (see Walk): the clip shown turns to its own if it walks, else keeps the
        // one it starts at, from the angle at its start over its blend in; before the first clip, the first's
        float GetWalkAngle(const Types::Actor& actor, const ActorFacts& facts, std::uint32_t tick)
        {
            const auto& clips = actor.clips;
            if (clips.empty())
                return 0.0f;
            const auto own = [&](std::size_t k, float otherwise) {
                return facts[k].walk && facts[k].walk->angle ? *facts[k].walk->angle : otherwise;
            };
            // how far a clip has blended in (the first doesn't blend)
            const auto blended = [&](std::size_t k, std::uint32_t t) {
                const auto& clip = clips[k];
                if (k == 0 || clip.blendIn == 0 || t >= clip.start + clip.blendIn)
                    return 1.0f;
                return t <= clip.start ? 0.0f : static_cast<float>(t - clip.start) / clip.blendIn;
            };
            const auto turn = [](float from, float to, float amount) {
                return from + std::remainder(to - from, 360.0f) * amount;
            };
            auto from = own(0, 0.0f), to = from;
            for (std::size_t k = 1; k < clips.size() && clips[k].start <= tick; ++k)
            {
                from = turn(from, to, blended(k - 1, clips[k].start));
                to = own(k, from);
            }
            const auto shown = static_cast<std::size_t>(
                std::ranges::find_if(clips.rbegin(), clips.rend(),
                                     [&](const Types::ActorClip& clip) { return clip.start <= tick; })
                    .base() -
                clips.begin());
            return turn(from, to, shown == 0 ? 1.0f : blended(shown - 1, tick));
        }
    }  // namespace

    Walk GetWalk(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations)
    {
        if (actor.clips.empty())
            return {};
        const auto facts = GetClipFacts(actor, animations);
        return {GetWalkedDistances(actor, facts, {tick}).front(), GetWalkAngle(actor, facts, tick)};
    }

    float GetWalkedDistance(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations)
    {
        return GetWalk(actor, tick, animations).distance;
    }

    Types::ActorPlacement PlaceOnPath(const RoundedPath& path, float distance)
    {
        if (path.pieces.empty())
            return {path.origin, 0.0f};
        distance = std::clamp(distance, 0.0f, path.length);
        const auto after = std::ranges::upper_bound(path.pieces, distance, {}, &RoundedPath::Piece::distance);
        const auto& piece = after == path.pieces.begin() ? path.pieces.front() : *std::prev(after);
        glm::vec3 tangent;
        const auto position = GetPieceAt(piece, std::clamp(distance - piece.distance, 0.0f, piece.length), tangent);
        return {position, glm::degrees(std::atan2(tangent.y, tangent.x))};
    }

    float ProjectOnPath(const RoundedPath& path, glm::vec2 point, float from, float to)
    {
        from = std::clamp(from, 0.0f, path.length);
        to = std::clamp(to, from, path.length);
        float best = from, nearest = FLT_MAX;
        const auto consider = [&](const RoundedPath::Piece& piece, float along) {
            glm::vec3 tangent;
            const auto at = glm::vec2(GetPieceAt(piece, along, tangent));
            if (const auto distance = glm::distance(at, point); distance < nearest)
            {
                nearest = distance;
                best = piece.distance + along;
            }
        };
        for (const auto& piece : path.pieces)
        {
            const auto lo = std::max(from - piece.distance, 0.0f), hi = std::min(to - piece.distance, piece.length);
            if (hi < lo)
                continue;
            if (piece.radius <= 0.0f)
            {
                consider(piece, std::clamp(glm::dot(point - glm::vec2(piece.start), glm::vec2(piece.direction)), lo, hi));
                continue;
            }
            // on an arc: the angle of the point about its centre, from where it starts
            const auto center = glm::vec2(piece.start + piece.normal * piece.radius);
            const auto fromCenter = point - center, startFromCenter = glm::vec2(piece.start) - center;
            const auto cross = startFromCenter.x * fromCenter.y - startFromCenter.y * fromCenter.x;
            const auto turn = glm::vec2(piece.direction).x * glm::vec2(piece.normal).y -
                              glm::vec2(piece.direction).y * glm::vec2(piece.normal).x;
            auto angle = std::atan2(cross, glm::dot(startFromCenter, fromCenter));
            if (turn < 0.0f)
                angle = -angle;
            consider(piece, std::clamp(angle * piece.radius, lo, hi));
            consider(piece, lo);
            consider(piece, hi);
        }
        return best;
    }

    std::optional<float> GetPointDistance(const RoundedPath& path, std::size_t point)
    {
        for (std::size_t k = 0; k < path.pieces.size(); ++k)
        {
            const auto& piece = path.pieces[k];
            if (piece.point <= point)
                continue;
            // (or the arc right after its own, with no straight between: the next corner's, or a stop's it runs into)
            if (piece.radius <= 0.0f || (k > 0 && path.pieces[k - 1].radius > 0.0f && path.pieces[k - 1].point == point))
                return piece.distance;
        }
        return std::nullopt;
    }

    glm::vec3 GetRootMotion(const RootMotion& motion, float time)
    {
        const auto& samples = motion.samples;
        if (samples.empty())
            return glm::vec3(0.0f);
        if (samples.size() == 1)
            return samples.front();
        const auto at = std::clamp(time, 0.0f, 1.0f) * static_cast<float>(samples.size() - 1);
        const auto index = std::min(static_cast<std::size_t>(at), samples.size() - 2);
        return glm::mix(samples[index], samples[index + 1], at - static_cast<float>(index));
    }

    namespace
    {
        // Where one placement is from another: x forward and y to the left of it, turned to the left of the way it
        // faces (degrees)
        struct Relative
        {
            glm::dvec2 position{0.0};
            double yaw = 0.0;
        };

        glm::dvec2 Turn(glm::dvec2 vector, double degrees)
        {
            const auto angle = glm::radians(degrees);
            return {vector.x * std::cos(angle) - vector.y * std::sin(angle),
                    vector.x * std::sin(angle) + vector.y * std::cos(angle)};
        }

        // b from where a puts it
        Relative Then(const Relative& a, const Relative& b)
        {
            return {a.position + Turn(b.position, a.yaw), a.yaw + b.yaw};
        }

        Relative Undo(const Relative& a)
        {
            return {Turn(-a.position, -a.yaw), -a.yaw};
        }

        // as far as a goes, so many times over
        Relative Repeat(const Relative& a, double times)
        {
            if (times <= 0.0)
                return {};
            const auto angle = glm::radians(a.yaw);
            // the turned copies of its position add up as a geometric series
            const auto z = std::polar(1.0, angle);
            const auto sum = std::abs(1.0 - z) < 1e-9 ? std::complex<double>(times)
                                                      : (1.0 - std::polar(1.0, angle * times)) / (1.0 - z);
            const auto position = sum * std::complex<double>(a.position.x, a.position.y);
            return {{position.real(), position.imag()}, a.yaw * times};
        }

        Relative FromSample(glm::vec3 sample)
        {
            return {{sample.x, sample.y}, sample.z};
        }
    }  // namespace

    RootMotion GuessRootMotion(glm::vec2 motion, float turn)
    {
        constexpr std::size_t SAMPLES = 33;
        RootMotion guess;
        for (std::size_t i = 0; i < SAMPLES; ++i)
        {
            const auto time = static_cast<float>(i) / (SAMPLES - 1);
            guess.samples.emplace_back(motion.x * time, motion.y * time, turn * time);
        }
        return guess;
    }

    glm::vec3 GetRootMotionBetween(const RootMotion& motion, float from, float to)
    {
        const auto between = Then(Undo(FromSample(GetRootMotion(motion, from))), FromSample(GetRootMotion(motion, to)));
        return {static_cast<float>(between.position.x), static_cast<float>(between.position.y),
                static_cast<float>(between.yaw)};
    }

    namespace
    {

        // How an animation moves the root from one time to another (seconds into it, before it loops or holds): a
        // looping one by a play's each play
        Relative GetPlayedMotion(const RootMotion& motion, float duration, bool loop, float from, float to)
        {
            const auto at = [&](float seconds) {
                const auto plays = std::max(static_cast<double>(seconds), 0.0) / duration;
                if (!loop)
                    return FromSample(GetRootMotion(motion, static_cast<float>(std::min(plays, 1.0))));
                const auto whole = std::floor(plays);
                return Then(Repeat(FromSample(motion.samples.back()), whole),
                            FromSample(GetRootMotion(motion, static_cast<float>(plays - whole))));
            };
            return Then(Undo(at(from)), at(to));
        }

        // How one of an actor's clips (that moves it by root motion) has moved it from where it started by a tick: by
        // its animation's root motion, weighed as the clip is shown while it blends in over the clip before it and
        // while the clip after it blends in over it (see Sample), as the game sums the motion of a tree's animations
        // by their weights; in steps over the blends
        Relative GetClipRootMotion(const Types::Actor& actor, const ActorFacts& facts, std::size_t index,
                                   std::uint32_t tick)
        {
            const auto& clips = actor.clips;
            const auto& clip = clips[index];
            if (tick <= clip.start)
                return {};
            const auto& animation = *facts[index].animation;
            const auto motion = [&](std::uint32_t from, std::uint32_t to) {
                return GetPlayedMotion(*facts[index].track, animation.duration, clip.loop,
                                       GetShownTime(clips, index, from), GetShownTime(clips, index, to));
            };
            const auto* next = index + 1 < clips.size() ? &clips[index + 1] : nullptr;
            const auto weight = [&](double t) {
                if (next && t >= next->start)
                    return next->blendIn > 0 && t < next->start + next->blendIn
                               ? 1.0 - (t - next->start) / next->blendIn
                               : 0.0;
                if (index > 0 && clip.blendIn > 0 && t < clip.start + clip.blendIn)
                    return (t - clip.start) / clip.blendIn;
                return 1.0;
            };
            // where the weight changes how it changes: blending in, the next clip starting and blended in
            std::vector<std::uint32_t> edges{clip.start, tick};
            if (index > 0 && clip.blendIn > 0)
                edges.push_back(clip.start + clip.blendIn);
            if (next)
            {
                edges.push_back(next->start);
                edges.push_back(next->start + next->blendIn);
            }
            std::ranges::sort(edges);
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
            Relative moved;
            for (std::size_t e = 0; e + 1 < edges.size() && edges[e] < tick; ++e)
            {
                const auto from = std::max(edges[e], clip.start), to = std::min(edges[e + 1], tick);
                if (from >= to)
                    continue;
                const auto middle = (static_cast<double>(from) + static_cast<double>(to)) / 2.0;
                const auto first = weight(from + 0.5), last = weight(to - 0.5);
                if (first >= 1.0 && last >= 1.0)
                {
                    moved = Then(moved, motion(from, to));
                    continue;
                }
                if (weight(middle) <= 0.0)
                    continue;
                for (auto t = from; t < to; t += WALK_STEP)
                {
                    const auto stepEnd = std::min(t + WALK_STEP, to);
                    const auto step = motion(t, stepEnd);
                    const auto amount = weight((static_cast<double>(t) + static_cast<double>(stepEnd)) / 2.0);
                    // (a transition turning the actor turns it all the way, as the game turns its AI in code)
                    moved = Then(moved, {step.position * amount, clip.turn ? step.yaw : step.yaw * amount});
                }
            }
            return moved;
        }

        // the tick after which a clip's animation moves no more: it holds its last frame (as one that doesn't loop
        // does once played) or isn't shown any more, the clip after it having blended in (see Sample)
        std::uint32_t GetRootMotionEnd(const Types::Actor& actor, const ActorFacts& facts, std::size_t index)
        {
            const auto& clip = actor.clips[index];
            auto end = clip.End();
            if (index + 1 < actor.clips.size())
            {
                const auto& next = actor.clips[index + 1];
                end = std::max(next.start, clip.End() + next.blendIn);
            }
            const auto duration = facts[index].animation->duration;
            if (!clip.loop && clip.speed > 0.0f)
                end = std::min(end, clip.start + static_cast<std::uint32_t>(std::ceil(
                                                     std::max(duration - clip.offset, 0.0f) / clip.speed * 1000.0f)));
            return end;
        }

        // how much of an offset is left once the actor has gone so far on the way it goes after it, going back onto
        // that way over the distance: all of it until it goes on, smoothly less, none from there; and how fast it gets
        // less as the actor goes on
        float GetMerge(float gone, float distance, float* rate = nullptr)
        {
            const auto amount = std::clamp(gone / distance, 0.0f, 1.0f);
            if (rate)
                *rate = -6.0f * amount * (1.0f - amount) / distance;
            return 1.0f - amount * amount * (3.0f - 2.0f * amount);
        }

        // how far a walk goes to come back from an offset: the merge distance, more for a longer way back
        float GetMergeDistance(glm::vec2 offset)
        {
            return std::max(MERGE_DISTANCE, 2.5f * glm::length(offset));
        }

        // the same after a path's transition, which leaves the actor a little to the side of its path as it sets off or
        // turns (as the game's AI goes on from where it lands): back onto it more gently
        float GetTransitionMergeDistance(glm::vec2 offset)
        {
            return std::max(2.0f * MERGE_DISTANCE, 6.0f * glm::length(offset));
        }

        // Where an actor's clips' root motion has moved it at a tick from where its movement (its position, keys or
        // path) has it: in the world, and turned; how fast the offset closes as it goes on along its path (units of
        // it per unit gone along the path); and how far the actor has got along the way its movement takes it by then
        // (along its path: as far as it walked, and the root motion along the path of the clips that moved it; the
        // ground its keys go over)
        struct RootOffset
        {
            glm::vec2 position{0.0f};
            float yaw = 0.0f;
            glm::vec2 drift{0.0f};
            float gone = 0.0f;
        };
        // the way an actor's movement has it face at a tick, gone so far by then
        using BaseYaw = std::function<float(std::uint32_t tick, float gone)>;
        // how far it has gone by each of some ticks, in order
        using Travel = std::function<std::vector<float>(const std::vector<std::uint32_t>& ticks)>;

        RootOffset GetRootOffset(const Types::Actor& actor, const ActorFacts& facts, std::uint32_t tick,
                                 const BaseYaw& baseYaw, const Travel& travel)
        {
            const auto& clips = actor.clips;
            // the clips that move it, started by then
            std::vector<std::size_t> moving;
            for (std::size_t k = 0; k < clips.size() && clips[k].start < tick; ++k)
                if (facts[k].rootMotion)
                    moving.push_back(k);

            // the ticks to know how far it has gone by: the tick, each clip's start, and where each one stops moving it
            // by the tick and by each later one's start
            std::vector<std::uint32_t> ends, ticks{tick};
            for (std::size_t i = 0; i < moving.size(); ++i)
            {
                const auto start = clips[moving[i]].start;
                ends.push_back(GetRootMotionEnd(actor, facts, moving[i]));
                ticks.push_back(start);
                ticks.push_back(std::min(ends[i], tick));
                for (std::size_t j = 0; j < i; ++j)
                    ticks.push_back(std::min(ends[j], start));
            }
            std::ranges::sort(ticks);
            ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());
            const auto travelled = travel(ticks);
            const auto goneBy = [&](std::uint32_t t) {
                return travelled[static_cast<std::size_t>(std::ranges::lower_bound(ticks, t) - ticks.begin())];
            };

            RootOffset offset;
            offset.gone = goneBy(tick);
            // each clip turned the way the actor faced as it started: as its movement had it, turned by the clips
            // before (what is left of their turn)
            std::vector<double> facing(moving.size());
            for (std::size_t i = 0; i < moving.size(); ++i)
            {
                const auto start = clips[moving[i]].start;
                auto yaw = static_cast<double>(baseYaw(start, goneBy(start)));
                for (std::size_t j = 0; j < i; ++j)
                {
                    const auto motion = GetClipRootMotion(actor, facts, moving[j], start);
                    const auto world = glm::vec2(Turn(motion.position, facing[j]));
                    yaw += motion.yaw *
                           GetMerge(goneBy(start) - goneBy(std::min(ends[j], start)), GetMergeDistance(world));
                }
                facing[i] = yaw;
            }
            for (std::size_t i = 0; i < moving.size(); ++i)
            {
                const auto motion = GetClipRootMotion(actor, facts, moving[i], tick);
                const auto world = glm::vec2(Turn(motion.position, facing[i]));
                float rate = 0.0f;
                const auto left =
                    GetMerge(offset.gone - goneBy(std::min(ends[i], tick)), GetMergeDistance(world), &rate);
                offset.position += world * left;
                offset.yaw += static_cast<float>(motion.yaw) * left;
                offset.drift += world * rate;
            }
            return offset;
        }

        // The same for an actor walking a path (see GetRootOffset), where what a clip moved it along the path goes on
        // from there: once a clip has stopped moving it, the actor is that much further along its path (or back), and
        // only the rest, what it moved it off the path, goes away as it walks on. A stumble while running runs on from
        // where it came to, instead of slowing down to get back to where it would have been.
        // A path's transitions (see TransitionLogic::PlanPath) go on from where they put the actor on the path: the
        // point of it nearest (a turn's past its corner), and what they turned it that the path turns too is the path's
        // turn; where the walk after one starts while it still moves the actor, the walk goes on from there, the rest
        // of its move added. The first, setting off from where the actor stood, turns it from the way it faced before
        // it walked (its turn off the path's way at its start). `moved` gets how much further along the path the
        // transitions put the actor, and when (as each hands over or stops moving it), up to the tick.
        RootOffset GetPathRootOffset(const Types::Actor& actor, const ActorFacts& facts, const RoundedPath& path,
                                     std::uint32_t tick, std::vector<std::pair<std::uint32_t, float>>* moved = nullptr)
        {
            const auto& clips = actor.clips;
            std::vector<std::size_t> moving;
            for (std::size_t k = 0; k < clips.size() && clips[k].start < tick; ++k)
                if (facts[k].rootMotion)
                    moving.push_back(k);

            // the ticks to know how far it walked by: the tick, each clip's start and where each one stops moving it,
            // and where the walk after a path's transition takes over from it
            std::vector<std::uint32_t> ends, ticks{tick};
            std::vector<std::optional<std::uint32_t>> handOvers;
            for (const auto k : moving)
            {
                const auto& clip = clips[k];
                ends.push_back(GetRootMotionEnd(actor, facts, k));
                ticks.push_back(clip.start);
                ticks.push_back(std::min(ends.back(), tick));
                std::optional<std::uint32_t> handOver;
                if (IsPathTransition(clip) && k + 1 < clips.size() && facts[k + 1].walk &&
                    clips[k + 1].start > clip.start && clips[k + 1].start < ends.back())
                {
                    handOver = clips[k + 1].start;
                    ticks.push_back(std::min(*handOver, tick));
                }
                handOvers.push_back(handOver);
            }
            std::ranges::sort(ticks);
            ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());
            const auto walked = GetWalkedDistances(actor, facts, ticks);
            const auto walkedBy = [&](std::uint32_t t) {
                return walked[static_cast<std::size_t>(std::ranges::lower_bound(ticks, t) - ticks.begin())];
            };

            // the transition setting off from where the actor stood (nothing walking or moving it before it), which
            // faces the way it turns from, as the actor did before it walked
            std::optional<std::size_t> setOff;
            for (std::size_t k = 0; k < clips.size(); ++k)
            {
                if (clips[k].chain == Types::ActorChain::Start && clips[k].turn)
                {
                    setOff = k;
                    break;
                }
                if (facts[k].walk || facts[k].rootMotion)
                    break;
            }
            const auto bias = [&](std::size_t k) {
                return setOff && *setOff == k ? -*clips[k].turn : 0.0f;
            };

            // the clips in the order they start, hand over and stop moving it (stopping first at the same tick): as
            // one starts, the way it faces then; as one stops, how far it moved the actor along the path joins how far
            // along it the actor is, and what it moved it off the path stays, to go away as it walks on from there
            enum class Kind
            {
                Stop,
                HandOver,
                Start,
            };
            struct Event
            {
                std::uint32_t tick;
                Kind kind;
                std::size_t index;  // into moving
            };
            std::vector<Event> events;
            for (std::size_t i = 0; i < moving.size(); ++i)
            {
                events.push_back({clips[moving[i]].start, Kind::Start, i});
                if (handOvers[i] && *handOvers[i] <= tick)
                    events.push_back({*handOvers[i], Kind::HandOver, i});
                if (ends[i] <= tick)
                    events.push_back({ends[i], Kind::Stop, i});
            }
            std::ranges::stable_sort(events, [](const Event& a, const Event& b) {
                return a.tick != b.tick ? a.tick < b.tick : a.kind < b.kind;
            });
            // what a clip left off the path as it stopped (or handed over), going away as the actor walks on
            struct Left
            {
                glm::vec2 off{0.0f};  // in the world
                float yaw = 0.0f;
                float walked = 0.0f;    // where the actor had walked to then
                float distance = 0.0f;  // the walk that takes the actor back onto the path
            };
            std::vector<std::vector<Left>> left(moving.size());
            std::vector<bool> started(moving.size()), stopped(moving.size());
            // where a transition was in its move as the walk after it took over: its move and turn so far
            std::vector<std::optional<std::pair<glm::vec2, float>>> handedOver(moving.size());
            std::vector<double> facing(moving.size());
            // the way the path went where a one-off (not one of the path's transitions) started: it goes on facing as
            // it did then, its own turn added, wherever the walk going on under its blend in takes it along the path
            // (a death starting at a corner doesn't turn with the corner, as the game's AI stops facing its path as it
            // dies)
            std::vector<std::optional<float>> heading(moving.size());
            float along = 0.0f;
            const auto headingAt = [&](float distance) { return PlaceOnPath(path, distance).yaw; };
            // what a clip moves the actor by and turns it by at a tick, the actor as far along its path as then
            const auto worldOf = [&](std::size_t i, std::uint32_t t, float at) {
                const auto motion = GetClipRootMotion(actor, facts, moving[i], t);
                const auto held =
                    heading[i] ? static_cast<float>(std::remainder(*heading[i] - headingAt(at), 360.0f)) : 0.0f;
                return std::pair(glm::vec2(Turn(motion.position, facing[i])),
                                 static_cast<float>(motion.yaw) + bias(moving[i]) + held);
            };
            // what a clip moves the actor off its path and turns it by at a tick, with what is left of what it did
            const auto contribution = [&](std::size_t i, std::uint32_t t) {
                std::pair<glm::vec2, float> sum{glm::vec2(0.0f), 0.0f};
                const auto walkedThen = walkedBy(t);
                for (const auto& each : left[i])
                {
                    const auto merge = GetMerge(walkedThen - each.walked, each.distance);
                    sum.first += each.off * merge;
                    sum.second += each.yaw * merge;
                }
                if (started[i] && !stopped[i])
                {
                    const auto [world, turned] = worldOf(i, t, walkedThen + along);
                    sum.first += handedOver[i] ? world - handedOver[i]->first : world;
                    sum.second += handedOver[i] ? turned - handedOver[i]->second : turned;
                }
                return sum;
            };
            for (const auto& event : events)
            {
                const auto i = event.index;
                const auto& clip = clips[moving[i]];
                const auto at = walkedBy(event.tick) + along;
                if (event.kind == Kind::Start)
                {
                    // as its path and walk have it face, turned by what the clips before it turned it (what is left)
                    auto yaw = static_cast<double>(headingAt(at) - GetWalkAngle(actor, facts, event.tick));
                    for (std::size_t j = 0; j < moving.size(); ++j)
                        yaw += contribution(j, event.tick).second;
                    // (setting off, the way the actor faced before it walked)
                    yaw += bias(moving[i]);
                    facing[i] = yaw;
                    if (!IsPathTransition(clip))
                        heading[i] = headingAt(at);
                    started[i] = true;
                    continue;
                }
                const auto [world, turned] = worldOf(i, event.tick, at);
                const auto here = PlaceOnPath(path, at);
                // what the path turns between where the actor was on it and where a clip's move puts it, which the
                // actor doesn't: it keeps facing the way it faced, and turns with the path as it walks on (a death
                // carried round a corner of its path lies as it fell)
                const auto pathTurn = [&](float to) {
                    return static_cast<float>(std::remainder(headingAt(to) - here.yaw, 360.0f));
                };
                if (event.kind == Kind::Stop && handedOver[i])
                {
                    // the rest of its move after the walk took over goes on along the way the actor goes
                    const auto rest = world - handedOver[i]->first;
                    const auto way = glm::vec2(std::cos(glm::radians(here.yaw)), std::sin(glm::radians(here.yaw)));
                    const auto ahead = glm::dot(rest, way);
                    const auto off = glm::vec2(here.position) + rest - glm::vec2(PlaceOnPath(path, at + ahead).position);
                    along += ahead;
                    if (moved)
                        moved->emplace_back(event.tick, ahead);
                    left[i].push_back({off, turned - handedOver[i]->second - pathTurn(at + ahead), walkedBy(event.tick),
                                       GetTransitionMergeDistance(off)});
                    stopped[i] = true;
                    continue;
                }
                if (IsPathTransition(clip))
                {
                    // the point of the path nearest where it put the actor (a turn's past its corner), and the path's turn
                    // between there and where it started of what it turned the actor
                    auto from = at - 1.0f;
                    if (clip.chain == Types::ActorChain::Turn && clip.corner)
                        if (const auto corner = GetPointDistance(path, *clip.corner))
                            from = *corner;
                    const auto point = glm::vec2(here.position) + world;
                    const auto onPath = ProjectOnPath(path, point, from, std::max(from, at) + glm::length(world) + 50.0f);
                    const auto off = point - glm::vec2(PlaceOnPath(path, onPath).position);
                    const auto yaw = turned - pathTurn(onPath);
                    along += onPath - at;
                    if (moved)
                        moved->emplace_back(event.tick, onPath - at);
                    left[i].push_back({off, yaw, walkedBy(event.tick), GetTransitionMergeDistance(off)});
                    if (event.kind == Kind::HandOver)
                        handedOver[i] = std::pair(world, turned);
                    else
                        stopped[i] = true;
                    continue;
                }
                const auto way = glm::vec2(std::cos(glm::radians(here.yaw)), std::sin(glm::radians(here.yaw)));
                const auto ahead = glm::dot(world, way);
                const auto off = glm::vec2(here.position) + world - glm::vec2(PlaceOnPath(path, at + ahead).position);
                along += ahead;
                left[i].push_back({off, turned - pathTurn(at + ahead), walkedBy(event.tick), GetMergeDistance(off)});
                stopped[i] = true;
            }

            RootOffset offset;
            const auto walkedNow = walkedBy(tick);
            offset.gone = walkedNow + along;
            for (std::size_t i = 0; i < moving.size(); ++i)
            {
                for (const auto& each : left[i])
                {
                    float rate = 0.0f;
                    const auto merge = GetMerge(walkedNow - each.walked, each.distance, &rate);
                    offset.position += each.off * merge;
                    offset.yaw += each.yaw * merge;
                    offset.drift += each.off * rate;
                }
                if (stopped[i])
                    continue;
                const auto [world, turned] = worldOf(i, tick, offset.gone);
                offset.position += handedOver[i] ? world - handedOver[i]->first : world;
                offset.yaw += handedOver[i] ? turned - handedOver[i]->second : turned;
            }
            // (before it sets off, the way it faced)
            if (setOff && std::ranges::find(moving, *setOff) == moving.end())
                offset.yaw += bias(*setOff);
            return offset;
        }

        // where an actor stands at a tick as its movement has it, the offset of its clips' root motion added (see
        // EvaluatePlacement), and that offset
        struct Placed
        {
            Types::ActorPlacement placement;
            RootOffset offset;
        };
        // A killstreak's death started by a tick (the latest), if it is one dying
        const Types::ActorClip* FindVehicleDeath(const Types::Actor& actor, std::uint32_t tick)
        {
            const Types::ActorClip* found = nullptr;
            if (!Components::KillstreakLogic::FindByModels(actor.models))
                return found;
            for (const auto& clip : actor.clips)
                if (clip.start < tick && Components::KillstreakLogic::IsDeathMotion(clip.animation) &&
                    (!found || clip.start > found->start))
                    found = &clip;
            return found;
        }

        Placed Place(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations)
        {
            Placed placed;
            // the game works out where the bone an attached one is on is
            if (actor.movement == Types::ActorMovement::Attached)
            {
                placed.placement = {actor.position, actor.yaw};
                return placed;
            }
            // a killstreak dying: on from where it was as its death started, at the speed and heading it had, then as
            // its death goes (Follows path off)
            if (const auto dying = FindVehicleDeath(actor, tick))
            {
                constexpr std::uint32_t BEFORE = 20;
                const auto start = dying->start;
                const auto at = Place(actor, start, animations).placement;
                const auto before = Place(actor, start > BEFORE ? start - BEFORE : 0, animations).placement;
                const auto velocity =
                    start > BEFORE ? (at.position - before.position) / (BEFORE / 1000.0f) : glm::vec3(0.0f);
                // (a Spin out spinning and flying on as its clip's Spin and Speed rows say, else as the killstreak does)
                auto info = Components::KillstreakLogic::Get(*Components::KillstreakLogic::FindByModels(actor.models));
                info.spinRate = Components::KillstreakLogic::GetSpinRate(info, *dying);
                info.crashSpeed = Components::KillstreakLogic::GetCrashSpeed(info, *dying);
                const auto motion = Components::VehicleLogic::GetDeathMotion(
                    info, dying->animation, velocity, static_cast<float>(tick - start) / 1000.0f * dying->speed);
                placed.placement = {at.position + motion.offset, std::remainder(at.yaw + motion.yaw, 360.0f)};
                return placed;
            }
            const auto facts = GetClipFacts(actor, animations);
            if (actor.movement == Types::ActorMovement::Path && !actor.path.empty())
            {
                // as far along its path as it walked, facing away from the path as its clips walk (a strafe left goes
                // along it facing to its right), facing the way it goes back onto it after root motion moved it off
                const auto path = RoundActorPath(actor, facts);
                placed.offset = GetPathRootOffset(actor, facts, path, tick);
                auto& placement = placed.placement;
                placement = PlaceOnPath(path, placed.offset.gone);
                auto way = placement.yaw;
                if (glm::length(placed.offset.drift) > 1e-6f)
                {
                    const auto going =
                        glm::vec2(std::cos(glm::radians(way)), std::sin(glm::radians(way))) + placed.offset.drift;
                    if (glm::length(going) > 1e-4f)
                        way = glm::degrees(std::atan2(going.y, going.x));
                }
                placement.position += glm::vec3(placed.offset.position, 0.0f);
                // (with a path's transitions, as high as the path where they put it: they move it along a sloping
                // path before the walk after them does)
                if (std::ranges::any_of(actor.clips, IsPathTransition))
                {
                    const auto reach = glm::length(placed.offset.position) + 50.0f;
                    const auto onPath =
                        ProjectOnPath(path, glm::vec2(placement.position), std::max(placed.offset.gone - reach, 0.0f),
                                      placed.offset.gone + reach);
                    placement.position.z = PlaceOnPath(path, onPath).position.z;
                }
                placement.yaw = std::remainder(way - GetWalkAngle(actor, facts, tick) + placed.offset.yaw, 360.0f);
                return placed;
            }
            if (actor.movement == Types::ActorMovement::Keyframes && !actor.keys.empty())
            {
                placed.offset = GetRootOffset(
                    actor, facts, tick, [&](std::uint32_t t, float) { return GetKeyedPlacement(actor.keys, t).yaw; },
                    [&](const std::vector<std::uint32_t>& ticks) { return GetKeyedTravel(actor.keys, ticks); });
                placed.placement = GetKeyedPlacement(actor.keys, tick);
            }
            else
            {
                placed.offset = GetRootOffset(
                    actor, facts, tick, [&](std::uint32_t, float) { return actor.yaw; },
                    [](const std::vector<std::uint32_t>& ticks) { return std::vector<float>(ticks.size(), 0.0f); });
                placed.placement = {actor.position, actor.yaw};
            }
            placed.placement.position += glm::vec3(placed.offset.position, 0.0f);
            if (placed.offset.yaw != 0.0f)
                placed.placement.yaw = std::remainder(placed.placement.yaw + placed.offset.yaw, 360.0f);
            return placed;
        }
    }  // namespace

    RootMotionUse GetRootMotionUse(const Types::Actor& actor, const Types::ActorClip& clip,
                                   const AnimationLookup& animations)
    {
        // (a copy of one of its clips by its id)
        auto index = static_cast<std::size_t>(&clip - actor.clips.data());
        if (index >= actor.clips.size())
            index = static_cast<std::size_t>(std::ranges::find(actor.clips, clip.id, &Types::ActorClip::id) -
                                             actor.clips.begin());
        if (index >= actor.clips.size())
            return RootMotionUse::None;
        return GetClipFacts(actor, animations)[index].rootMotionUse;
    }

    float GetPathProgress(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations)
    {
        const auto facts = GetClipFacts(actor, animations);
        if (actor.movement != Types::ActorMovement::Path || actor.path.empty())
            return GetWalkedDistances(actor, facts, {tick}).front();
        return GetPathRootOffset(actor, facts, RoundActorPath(actor, facts), tick).gone;
    }

    PathTiming GetPathTiming(const Types::Actor& actor, const AnimationLookup& animations)
    {
        PathTiming timing;
        timing.reached.resize(actor.path.size());
        timing.left.resize(actor.path.size());
        timing.joins.resize(actor.path.size());
        if (actor.movement != Types::ActorMovement::Path || actor.clips.empty() || actor.path.size() < 2)
            return timing;
        const auto facts = GetClipFacts(actor, animations);
        const auto rounded = RoundActorPath(actor, facts);
        if (rounded.pieces.empty())
            return timing;
        std::vector<std::optional<float>> distances;
        for (std::size_t i = 0; i < actor.path.size(); ++i)
            distances.push_back(i + 1 == actor.path.size() ? std::optional(rounded.length)
                                                           : GetPointDistance(rounded, i));
        // as far as its clips walk it (and a while after, as the clip after the walk takes over)
        std::uint32_t end = 0;
        for (const auto& clip : actor.clips)
            end = std::max(end, clip.End());
        // where it is on its path, as the placement has it (see GetPathProgress): as far as it walked, and as much
        // further as its path's transitions (its start, turns and stop) put it from when each stops moving it
        std::vector<std::pair<std::uint32_t, float>> moved;
        if (std::ranges::any_of(actor.clips, IsPathTransition))
            GetPathRootOffset(actor, facts, rounded, end + 2000, &moved);
        std::size_t next = 0;
        float transitions = 0.0f;
        const auto movedBy = [&](std::uint32_t t) {
            for (; next < moved.size() && moved[next].first <= t; ++next)
                transitions += moved[next].second;
            return transitions;
        };
        PathWalker walker(actor, facts, facts.stretches.speeds.empty() ? nullptr : &rounded);
        auto before = movedBy(actor.clips.front().start);
        for (auto t = actor.clips.front().start; t < end + 2000; t += WALK_STEP)
        {
            walker.Step(t, WALK_STEP);
            const auto after = walker.Walked() + movedBy(t + WALK_STEP);
            const auto gone = after - before;
            if (gone <= 0.0f)
                continue;
            if (!timing.start)
                timing.start = t;
            for (std::size_t i = 0; i < distances.size(); ++i)
            {
                // (stopped by a transition at the end or at a wait: near it)
                const bool stopped =
                    i + 1 == distances.size() || std::ranges::any_of(actor.clips, [&](const Types::ActorClip& clip) {
                        return clip.chain == Types::ActorChain::Stop && clip.corner == i;
                    });
                // (a killstreak's flight comes to rest exactly there, slowly at the end)
                const auto tolerance = walker.Flies() ? 0.01f : stopped ? ARRIVAL_TOLERANCE : 1e-3f;
                if (!timing.reached[i] && distances[i] && *distances[i] <= after + tolerance)
                {
                    const auto into = std::clamp((*distances[i] - before) / gone, 0.0f, 1.0f);
                    timing.reached[i] = t + static_cast<std::uint32_t>(into * WALK_STEP);
                    const auto* stretch = GetStretch(actor, i);
                    timing.left[i] = *timing.reached[i] + (stretch && i + 1 < distances.size() ? stretch->wait : 0u);
                }
            }
            before = after;
            if (timing.reached.back())
                break;
        }
        // (the joins' blends as the walk went through them)
        timing.joins.resize(actor.path.size());
        const auto& joinStarts = walker.JoinStarts();
        for (std::size_t j = 0; j < joinStarts.size() && j < timing.joins.size(); ++j)
            if (joinStarts[j] && facts.stretches.joins[j] > 0)
                timing.joins[j] = std::pair(*joinStarts[j], *joinStarts[j] + facts.stretches.joins[j]);
        // (setting off with a start of its own: from when that starts)
        for (const auto& clip : actor.clips)
            if (clip.chain == Types::ActorChain::Start)
            {
                if (timing.start && clip.start < *timing.start)
                {
                    timing.start = clip.start;
                    timing.reached.front() = clip.start;
                    timing.left.front() = clip.start;
                }
                break;
            }
        timing.arrival = timing.reached.back();
        return timing;
    }

    std::vector<WalkPiece> GetWalkPieces(const Types::Actor& actor, const PathTiming& timing,
                                         const AnimationLookup& animations)
    {
        std::vector<WalkPiece> pieces;
        if (actor.movement != Types::ActorMovement::Path || actor.stretches.empty() || actor.path.size() < 2 ||
            timing.reached.size() != actor.path.size() || timing.joins.size() != actor.path.size())
            return pieces;
        const auto& clips = actor.clips;
        const auto facts = GetClipFacts(actor, animations);
        // (the last stretch walked: the last point's goes nowhere)
        const auto lastStretch = std::min(actor.path.size() - 2, actor.stretches.size() - 1);
        const auto planned = [&](std::size_t point) {
            return std::ranges::any_of(clips, [&](const Types::ActorClip& clip) {
                return (clip.chain == Types::ActorChain::Turn || clip.chain == Types::ActorChain::Stop) &&
                       clip.corner == point;
            });
        };
        for (std::size_t k = 0; k < clips.size(); ++k)
        {
            if (!facts[k].stretched || !facts[k].walk)
                continue;
            const auto& clip = clips[k];
            const auto shownEnd = k + 1 < clips.size() ? std::min(clip.End(), clips[k + 1].start) : clip.End();
            if (shownEnd <= clip.start)
                continue;
            // the stretch it starts on: after a turn or a wait of the path's plan, the point's; else the last it has
            // left by then
            std::optional<std::size_t> from;
            for (auto j = k; j-- > 0 && !from;)
            {
                if ((clips[j].chain == Types::ActorChain::Turn || clips[j].chain == Types::ActorChain::Stop) &&
                    clips[j].corner)
                    from = *clips[j].corner;
                else if (facts[j].stretched && facts[j].walk)
                    break;
            }
            if (!from)
            {
                from = 0;
                for (std::size_t i = 1; i <= lastStretch; ++i)
                    if (timing.left[i] && *timing.left[i] <= clip.start)
                        from = i;
            }
            const auto first = std::min(*from, lastStretch);
            pieces.push_back(
                {clip.id, first, first, clip.start, shownEnd, clip.blendIn, false, actor.stretches[first].animation});
            // on through the points it reaches while it shows: a piece more at each join
            for (auto i = first + 1; i <= lastStretch; ++i)
            {
                if (planned(i) || !timing.reached[i] || *timing.reached[i] >= shownEnd)
                    break;
                if (const auto& join = timing.joins[i];
                    join && join->first > pieces.back().start && join->first < shownEnd)
                {
                    pieces.back().end = join->first;
                    pieces.push_back({clip.id, i, i, join->first, shownEnd, join->second - join->first, true,
                                      actor.stretches[i].animation});
                }
                else
                    pieces.back().last = i;
            }
        }
        return pieces;
    }

    std::optional<float> SolvePace(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations,
                                   const std::function<void(Types::Actor&)>& fit)
    {
        if (actor.stretches.empty())
            return std::nullopt;
        auto trial = actor;
        // (it stands at its waits as long, however fast it goes between them)
        std::uint32_t waits = 0;
        for (std::size_t i = 0; i + 1 < trial.stretches.size(); ++i)
            waits += trial.stretches[i].wait;
        bool solved = false;
        for (int i = 0; i < 4; ++i)
        {
            const auto timing = GetPathTiming(trial, animations);
            if (!timing.start || !timing.arrival || tick <= *timing.start + waits + 100)
                break;
            solved = true;
            if (std::abs(static_cast<float>(*timing.arrival) - static_cast<float>(tick)) < 20.0f)
                break;
            const auto moving = static_cast<float>(*timing.arrival - *timing.start) - static_cast<float>(waits);
            const auto wanted = static_cast<float>(tick - *timing.start) - static_cast<float>(waits);
            trial.pace = std::clamp(trial.pace * moving / wanted, MIN_PACE_FROM_BEFORE, MAX_PACE_FROM_BEFORE);
            // (fitted again, with its transitions, at that pace)
            fit(trial);
        }
        return solved ? std::optional(trial.pace) : std::nullopt;
    }

    Types::ActorPlacement EvaluatePlacement(const Types::Actor& actor, std::uint32_t tick,
                                            const AnimationLookup& animations)
    {
        return Place(actor, tick, animations).placement;
    }

    void ApplyPlacement(Types::Actor& actor, std::uint32_t tick, glm::vec3 position, float yaw,
                        const AnimationLookup& animations)
    {
        switch (actor.movement)
        {
            case Types::ActorMovement::Keyframes:
            {
                // the key that puts it there with the root motion its clips have moved it by then, which turns with the
                // keys before it: found again with the key in place, until it stays put
                auto key = std::ranges::find(actor.keys, tick, &Types::ActorKey::tick);
                if (key == actor.keys.end())
                {
                    actor.keys.push_back({tick, position, yaw});
                    std::ranges::stable_sort(actor.keys, {}, &Types::ActorKey::tick);
                    key = std::ranges::find(actor.keys, tick, &Types::ActorKey::tick);
                }
                for (int round = 0; round < 4; ++round)
                {
                    const auto offset = Place(actor, tick, animations).offset;
                    *key = {tick, position - glm::vec3(offset.position, 0.0f),
                            offset.yaw != 0.0f ? std::remainder(yaw - offset.yaw, 360.0f) : yaw};
                }
                break;
            }
            case Types::ActorMovement::Path:
            {
                const auto offset = position - EvaluatePlacement(actor, tick, animations).position;
                for (auto& point : actor.path)
                    point += offset;
                break;
            }
            case Types::ActorMovement::Still:
            {
                // the root motion its clips have moved it by then turns with it
                const auto offset = Place(actor, tick, animations).offset;
                const auto standing = offset.yaw != 0.0f ? std::remainder(yaw - offset.yaw, 360.0f) : yaw;
                const auto moved = glm::vec2(Turn(offset.position, standing - actor.yaw));
                actor.position = position - glm::vec3(moved, 0.0f);
                actor.yaw = standing;
                break;
            }
            case Types::ActorMovement::Attached:  // moved by its offset from the bone instead
                break;
        }
    }

    std::optional<float> RayHitsSphere(glm::vec3 origin, glm::vec3 direction, glm::vec3 center, float radius)
    {
        const float along = glm::dot(center - origin, direction);
        const float distance = glm::distance(origin + direction * along, center);
        if (distance > radius || along + radius < 0.0f)
            return std::nullopt;
        return std::max(0.0f, along - std::sqrt(radius * radius - distance * distance));
    }

    std::optional<float> RayHitsActor(glm::vec3 origin, glm::vec3 direction, glm::vec3 feet)
    {
        // a capsule from the feet to about the head of a standing actor (a crouching one is lower, but is still
        // picked by its upper body's space)
        constexpr float RADIUS = 16.0f, HEIGHT = 72.0f;
        const auto bottom = feet + glm::vec3(0, 0, RADIUS);
        const auto axis = glm::vec3(0, 0, HEIGHT - 2 * RADIUS);

        // the closest points of the ray and the capsule's segment
        const auto w = origin - bottom;
        const float b = glm::dot(direction, axis), c = glm::dot(axis, axis);
        const float d = glm::dot(direction, w), e = glm::dot(axis, w);
        const float denominator = c - b * b;  // the direction is a unit vector
        // (a ray along the capsule: the end nearest its origin)
        const float s = std::clamp(denominator > 1e-3f * c ? (e - b * d) / denominator : e / c, 0.0f, 1.0f);
        const auto onSegment = bottom + axis * s;
        // the ray's closest point to that point of the segment
        const float along = glm::dot(onSegment - origin, direction);
        if (along < 0.0f)
            return std::nullopt;
        const auto closest = origin + direction * along;
        const float distance = glm::distance(closest, onSegment);
        if (distance > RADIUS)
            return std::nullopt;
        // back up to where the ray enters the sphere around that point
        return std::max(0.0f, along - std::sqrt(RADIUS * RADIUS - distance * distance));
    }

    bool IsVisible(const Types::Actor& actor, std::uint32_t tick)
    {
        if (actor.hidden)
            return false;
        std::optional<bool> visible;
        for (const auto& event : actor.events)
        {
            if (event.type != Types::ActorEventType::Show && event.type != Types::ActorEventType::Hide)
                continue;
            const bool shows = event.type == Types::ActorEventType::Show;
            if (event.tick > tick)
            {
                // before its first Show it is hidden, and before its first Hide it is shown
                if (!visible)
                    visible = !shows;
                break;
            }
            visible = shows;
        }
        return visible.value_or(true);
    }

    bool EffectStays(const Types::Actor& actor, const Types::ActorEvent& event)
    {
        return std::ranges::any_of(actor.events, [&](const Types::ActorEvent& hide) {
            return hide.type == Types::ActorEventType::Hide && hide.tick >= event.tick &&
                   hide.tick - event.tick <= EFFECT_STAYS;
        });
    }

    LookState GetLook(const Types::Actor& actor, std::uint32_t tick)
    {
        LookState state;
        for (const auto& event : actor.events)
        {
            if (event.tick > tick)
                break;
            if (event.type != Types::ActorEventType::Look)
                continue;
            state.from = state.to;
            state.to = &event;
        }
        // looking ahead is looking at nothing
        auto target = [](const Types::ActorEvent* event) {
            return event && event->lookTarget != Types::ActorLookTarget::Ahead ? event : nullptr;
        };
        if (!state.to)
            return state;
        state.turned = state.to->lookTurn == 0
                           ? 1.0f
                           : std::clamp((tick - state.to->tick) / static_cast<float>(state.to->lookTurn), 0.0f, 1.0f);
        state.from = target(state.from);
        state.to = target(state.to);
        return state;
    }

    bool EffectLoops(const EffectInfo& info)
    {
        return info.loopingElements > 0 && info.loopingLife >= MIN_LOOPING_LIFE;
    }

    std::uint32_t GetEffectLength(const EffectInfo& info)
    {
        if (!EffectLoops(info))
            return EFFECT_ONCE_LENGTH;
        return info.loopingLife >= EffectInfo::FOREVER ? EFFECT_STOPPED_LENGTH : info.loopingLife;
    }

    bool PlaysLooping(const Types::ActorEffect& effect, const EffectInfo& info)
    {
        return effect.plays == Types::EffectPlays::Loop ||
               (effect.plays == Types::EffectPlays::Auto && EffectLoops(info));
    }

    std::uint32_t GetEffectPlayedEnd(const Types::ActorEffect& effect, const EffectInfo& info)
    {
        if (PlaysLooping(effect, info))
            return effect.end;
        if (EffectLoops(info) && info.loopingLife >= EffectInfo::FOREVER)
            return effect.end;
        return std::min(effect.end, effect.start + GetEffectLength(info));
    }

    std::optional<std::pair<std::uint32_t, std::uint32_t>> GetBar(const Types::Actor& actor)
    {
        if (actor.effect)
            return std::pair(actor.effect->start, actor.effect->end);
        if (actor.light)
            return std::pair(actor.light->start, actor.light->end);
        if (actor.placedProp)
            return std::pair(actor.placedProp->start, actor.placedProp->end);
        return std::nullopt;
    }

    void MoveBar(Types::Actor& actor, std::uint32_t start)
    {
        const auto bar = GetBar(actor);
        if (!bar)
            return;
        auto& first = actor.effect ? actor.effect->start : actor.light ? actor.light->start : actor.placedProp->start;
        auto& last = actor.effect ? actor.effect->end : actor.light ? actor.light->end : actor.placedProp->end;
        first = start;
        last = start + (bar->second - bar->first);
    }

    glm::vec2 GetFacing(glm::vec3 direction)
    {
        if (glm::length(direction) < 1e-4f)
            return {-90.0f, 0.0f};
        direction = glm::normalize(direction);
        const auto yaw = std::abs(direction.x) + std::abs(direction.y) < 1e-4f
                             ? 0.0f
                             : glm::degrees(std::atan2(direction.y, direction.x));
        return {-glm::degrees(std::asin(std::clamp(direction.z, -1.0f, 1.0f))), yaw};
    }

    Types::Actor MakeEffect(std::string_view effect, const EffectInfo& info, glm::vec3 point, glm::vec3 normal,
                            std::uint32_t tick)
    {
        // (a unit off the surface, so it isn't drawn inside it)
        const auto out = glm::length(normal) > 1e-4f ? glm::normalize(normal) : glm::vec3(0, 0, 1);
        const auto facing = GetFacing(out);
        Types::Actor actor;
        actor.position = point + out;
        actor.yaw = facing.y;
        actor.effect = Types::ActorEffect{.name = std::string(effect),
                                          .pitch = facing.x,
                                          .start = tick,
                                          .end = tick + GetEffectLength(info)};
        return actor;
    }

    Types::Actor MakeProp(std::string_view model, glm::vec3 point, float yaw, std::uint32_t tick, std::uint32_t end)
    {
        Types::Actor actor;
        actor.position = point;
        actor.yaw = yaw;
        actor.placedProp = Types::ActorPlacedProp{
            .model = std::string(model), .start = tick, .end = std::max(end, tick + PROP_LENGTH)};
        return actor;
    }

    std::string GetPropBaseName(std::string_view model)
    {
        auto name = std::string(model);
        // (p_..., t5_..., and their p_glo_ family of everyday things)
        for (const auto prefix : {"t5_", "p_", "glo_", "rus_", "jun_", "mp_"})
            if (name.starts_with(prefix) && name.size() > std::string_view(prefix).size())
                name.erase(0, std::string_view(prefix).size());
        std::ranges::replace(name, '_', ' ');
        if (!name.empty())
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        return name.empty() ? "Prop" : name;
    }

    std::string GetEffectBaseName(std::string_view effect)
    {
        auto name = std::string(effect.substr(effect.find_last_of('/') + 1));
        // (fx_mp_..., fx_dest_...)
        for (const auto prefix : {"fx_", "mp_", "dest_"})
            if (name.starts_with(prefix) && name.size() > std::string_view(prefix).size())
                name.erase(0, std::string_view(prefix).size());
        std::ranges::replace(name, '_', ' ');
        if (!name.empty())
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        return name.empty() ? "Effect" : name;
    }

    const std::vector<std::string_view>& GetEffectCategories()
    {
        static const std::vector<std::string_view> categories{
            "The map's own", "Environment", "Destructibles", "Explosions", "Impacts", "Vehicles",
            "Weapons",       "Bodies",      "Trails",        "Other"};
        return categories;
    }

    std::string_view GetEffectCategory(std::string_view effect)
    {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> FOLDERS{{
            {"maps", "The map's own"},
            {"env", "Environment"},
            {"destructibles", "Destructibles"},
            {"explosions", "Explosions"},
            {"impacts", "Impacts"},
            {"vehicle", "Vehicles"},
            {"weapon", "Weapons"},
            {"bio", "Bodies"},
            {"trail", "Trails"},
        }};
        const auto slash = effect.find('/');
        const auto folder = slash == std::string_view::npos ? std::string_view() : effect.substr(0, slash);
        for (const auto& [name, category] : FOLDERS)
            if (folder == name)
                return category;
        return "Other";
    }

    std::string DescribeEffect(const EffectInfo& info)
    {
        if (!EffectLoops(info))
            return "Plays once";
        if (info.loopingLife >= EffectInfo::FOREVER)
            return "Loops till it is stopped";
        return std::format("Loops for {:.1f} s, then ends by itself", info.loopingLife / 1000.0f);
    }

    std::vector<EffectPlay> GetEffectPlays(const Types::Actor& actor, const EffectInfo& info)
    {
        std::vector<EffectPlay> plays;
        if (!actor.effect || actor.effect->end <= actor.effect->start)
            return plays;
        const auto& effect = *actor.effect;
        const bool loops = EffectLoops(info);
        const bool cut = effect.ends == Types::EffectEnds::Cut;
        if (!PlaysLooping(effect, info))
        {
            // one play: a looping one kept till its looping life is over or the bar ends, one that plays once let go
            const auto end = GetEffectPlayedEnd(effect, info);
            plays.push_back({effect.start, loops ? end : effect.start + EFFECT_ONCE_LENGTH, loops,
                             loops && cut && end == effect.end});
            return plays;
        }
        // played again as each play ends: a looping one after its looping life, one that plays once every so often
        const auto period = loops ? (info.loopingLife >= EffectInfo::FOREVER ? effect.end - effect.start
                                                                             : info.loopingLife)
                                  : std::max<std::uint32_t>(effect.every, 100);
        for (auto begin = effect.start; begin < effect.end; begin += period)
        {
            if (loops)
            {
                const auto end = std::min(effect.end, begin + period);
                plays.push_back({begin, end, true, cut && end == effect.end});
            }
            else
                plays.push_back({begin, begin + EFFECT_ONCE_LENGTH, false, false});
            if (effect.end - begin <= period)
                break;
        }
        return plays;
    }

    std::vector<EffectPlay> GetEffectPlays(const Types::Actor& actor, const EffectInfo& info, std::uint32_t tick)
    {
        auto plays = GetEffectPlays(actor, info);
        std::erase_if(plays, [&](const EffectPlay& play) { return tick < play.begin || tick >= play.end; });
        return plays;
    }

    std::optional<HeadTurn> GetHeadTurn(std::optional<glm::quat> from, std::optional<glm::quat> to, float turned)
    {
        turned = std::clamp(turned, 0.0f, 1.0f);
        // from one thing to another, from the clip's own head to something, or from something back to the clip's
        if (from && to)
            return HeadTurn{glm::normalize(glm::slerp(*from, *to, turned)), 1.0f};
        if (to && turned > 0.0f)
            return HeadTurn{*to, turned};
        if (from && turned < 1.0f)
            return HeadTurn{*from, 1.0f - turned};
        return std::nullopt;
    }

    glm::quat ApplyHeadTurn(const glm::quat& animated, const HeadTurn& turn)
    {
        const auto length = glm::length(animated);
        if (length < 1e-4f)
            return turn.rotation;
        return glm::normalize(glm::slerp(animated / length, turn.rotation, std::clamp(turn.weight, 0.0f, 1.0f))) *
               length;
    }

    std::vector<FiredEvent> GetFiredEvents(const Types::Actor& actor, std::uint32_t from, std::uint32_t to)
    {
        std::vector<FiredEvent> fired;
        if (to <= from)
            return fired;
        for (const auto& event : actor.events)
        {
            if (event.End() < from || event.tick >= to)
                continue;
            const auto shots = event.type == Types::ActorEventType::Fire ? event.shots : 1;
            for (std::uint32_t shot = 0; shot < shots; ++shot)
            {
                const auto at = event.tick + shot * event.interval;
                if (at >= from && at < to)
                    fired.push_back({&event, shot});
            }
        }
        return fired;
    }

    namespace
    {
        // seconds into its animation a clip is at a tick, as it plays it (not wrapped by loops)
        float GetClipTime(const Types::ActorClip& clip, std::uint32_t tick)
        {
            const auto elapsed = tick >= clip.start ? std::min(tick - clip.start, clip.length) : 0;
            return clip.offset + elapsed / 1000.0f * std::max(clip.speed, 0.0f);
        }
    }  // namespace

    std::vector<PassedNotetrack> GetPassedNotetracks(const Types::Actor& actor, std::uint32_t from, std::uint32_t to,
                                                     const AnimationLookup& animations)
    {
        std::vector<PassedNotetrack> passed;
        if (to <= from)
            return passed;
        for (std::size_t i = 0; i < actor.clips.size(); ++i)
        {
            // the ticks it is the clip playing: from its start to the next clip's
            const auto& clip = actor.clips[i];
            const auto next = i + 1 < actor.clips.size() ? actor.clips[i + 1].start : UINT32_MAX;
            if (next <= clip.start || to < clip.start || from + 1 >= next)
                continue;
            const auto animation = animations(clip.animation);
            if (!animation || animation->notetracks.empty() || animation->duration <= 0.0f)
                continue;

            // the stretch of the animation it plays meanwhile, in plays; a notetrack at its very start is passed as
            // the clip starts
            const bool starting = from < clip.start;
            const auto lowTick = starting ? clip.start : from;
            const auto highTick = std::min(to, next - 1);
            const auto duration = animation->duration;
            auto low = GetClipTime(clip, lowTick) / duration, high = GetClipTime(clip, highTick) / duration;
            if (!clip.loop)
            {
                low = std::min(low, 1.0f);
                high = std::min(high, 1.0f);
            }
            if (high <= low && !starting)
                continue;

            // each play passed (a few at most: the timeline plays events of up to a second at a time)
            const auto firstPlay = static_cast<std::int64_t>(std::floor(low));
            const auto lastPlay = std::min(static_cast<std::int64_t>(std::floor(high)), firstPlay + 8);
            const auto shotsAPlay =
                static_cast<std::int64_t>(std::ranges::count_if(animation->notetracks, [](const auto& notetrack) {
                    return IsShotNotetrack(notetrack.name);
                }));
            for (auto play = firstPlay; play <= lastPlay; ++play)
            {
                auto shot = std::max<std::int64_t>(play, 0) * shotsAPlay;
                for (const auto& notetrack : animation->notetracks)
                {
                    const bool isShot = IsShotNotetrack(notetrack.name);
                    const auto at = static_cast<float>(play) + notetrack.time;
                    if ((at > low || (starting && at == low)) && at <= high)
                        passed.push_back({&clip, std::string(notetrack.name), static_cast<std::uint32_t>(shot)});
                    if (isShot)
                        ++shot;
                }
                if (!clip.loop)
                    break;
            }
        }
        return passed;
    }

    bool IsShotNotetrack(std::string_view name)
    {
        return name == "fire" || name == "fire_spray";
    }

    std::uint32_t GetShotPlace(const Types::Actor& actor, const Types::ActorEvent& event, std::uint32_t shot)
    {
        std::uint32_t place = shot;
        for (const auto& other : actor.events)
            if (other.type == Types::ActorEventType::Fire && &other != &event &&
                (other.tick < event.tick || (other.tick == event.tick && other.id < event.id)))
                place += other.shots;
        return place;
    }

    bool IsLeftGunShot(std::uint32_t place)
    {
        return place % 2 == 1;
    }

    std::string_view GetNotetrackSound(std::string_view name)
    {
        constexpr std::string_view PREFIX = "sndnt#";
        return name.starts_with(PREFIX) ? name.substr(PREFIX.size()) : std::string_view();
    }

    namespace
    {
        // whether a part of a name between underscores and digits is one of those, or starts with one of the others
        bool HasNamePart(std::string_view name, std::initializer_list<std::string_view> parts,
                         std::initializer_list<std::string_view> starts)
        {
            const auto isSeparator = [](char c) { return c == '_' || (c >= '0' && c <= '9'); };
            for (std::size_t start = 0; start < name.size();)
            {
                auto end = start;
                while (end < name.size() && !isSeparator(name[end]))
                    ++end;
                const auto part = name.substr(start, end - start);
                if (std::ranges::find(parts, part) != parts.end() ||
                    std::ranges::any_of(starts, [&](std::string_view prefix) { return part.starts_with(prefix); }))
                    return true;
                start = end + 1;
            }
            return false;
        }

        // The ticks a clip passes those of its animation's notetracks a test picks, from one tick to another (both
        // included), as GetPassedNotetracks passes them: while it is the clip playing (up to the next clip's start),
        // as far as it has played its animation (each play of a looping clip, a few dozen at most)
        std::vector<std::uint32_t> GetNotetrackTicks(const Types::Actor& actor, std::size_t index,
                                                     const AnimationInfo& animation, std::uint32_t from,
                                                     std::uint32_t to, bool (*picks)(std::string_view))
        {
            std::vector<std::uint32_t> ticks;
            const auto& clip = actor.clips[index];
            const auto next = index + 1 < actor.clips.size() ? actor.clips[index + 1].start : UINT32_MAX;
            const auto duration = animation.duration;
            if (duration <= 0.0f || clip.speed <= 0.0f || next <= clip.start ||
                std::ranges::none_of(animation.notetracks,
                                     [&](const Types::ActorNotetrack& notetrack) { return picks(notetrack.name); }))
                return ticks;
            const auto low = std::max(from, clip.start);
            const auto high = std::min({to, next - 1, clip.End()});
            if (high < low)
                return ticks;
            const auto firstPlay =
                clip.loop ? static_cast<std::int64_t>(std::floor(GetClipTime(clip, low) / duration)) : 0;
            const auto lastPlay =
                clip.loop ? std::min(static_cast<std::int64_t>(std::floor(GetClipTime(clip, high) / duration)),
                                     firstPlay + 64)
                          : 0;
            for (auto play = firstPlay; play <= lastPlay; ++play)
                for (const auto& notetrack : animation.notetracks)
                {
                    const auto at = (static_cast<float>(play) + notetrack.time) * duration;
                    if (!picks(notetrack.name) || at < clip.offset)
                        continue;
                    const auto tick = clip.start + static_cast<std::uint32_t>(
                                                       std::ceil((at - clip.offset) / clip.speed * 1000.0f - 1e-3f));
                    if (tick >= low && tick <= high)
                        ticks.push_back(tick);
                }
            return ticks;
        }

        // What the automatic face is in for a while (its idle the rest of the time): the animation, how much it goes
        // over the others, and when
        struct FaceWindow
        {
            std::string_view animation;
            int priority = 0;
            std::uint32_t start = 0;
            std::uint32_t end = UINT32_MAX;  // (excluded)
        };

        // the automatic face's windows that end at or after one tick and start at most at another
        std::vector<FaceWindow> GetFaceWindows(const Types::Actor& actor, std::uint32_t from, std::uint32_t to,
                                               const AnimationLookup& animations)
        {
            std::vector<FaceWindow> windows;
            // (each as long as its animation plays once, of those the game has)
            const auto lengthOf = [&](std::string_view animation) -> std::optional<std::uint32_t> {
                const auto info = animations(std::string(animation));
                if (!info)
                    return std::nullopt;
                return std::max(1u, static_cast<std::uint32_t>(std::lround(std::max(info->duration, 0.0f) * 1000.0f)));
            };
            const auto firing = lengthOf(FACE_FIRING), pain = lengthOf(FACE_PAIN), melee = lengthOf(FACE_MELEE);
            const auto death = lengthOf(FACE_DEATH);
            const auto before = [&](std::uint32_t ticks) { return from > ticks ? from - ticks : 0u; };

            // shots of its Fire events and of the fire notetracks its clips shoot, the windows of those close together
            // one (from the first shot to the last one's end)
            std::vector<std::uint32_t> shots;
            for (const auto& event : actor.events)
            {
                if (event.type == Types::ActorEventType::Hit && pain && event.tick <= to && event.tick + *pain > from)
                    windows.push_back({FACE_PAIN, 2, event.tick, event.tick + *pain});
                if (event.type != Types::ActorEventType::Fire || !firing || event.tick > to ||
                    event.End() < before(FIRING_FACE))
                    continue;
                const auto earliest = before(FIRING_FACE);
                const auto first =
                    earliest > event.tick ? (earliest - event.tick + event.interval - 1) / event.interval : 0;
                const auto last = std::min(event.shots - 1, (to - event.tick) / event.interval);
                for (auto shot = first; shot <= last; ++shot)
                    shots.push_back(event.tick + shot * event.interval);
            }
            const auto& clips = actor.clips;
            for (std::size_t k = 0; k < clips.size(); ++k)
            {
                const auto& clip = clips[k];
                const auto info = animations(clip.animation);
                if (!info)
                    continue;
                if (firing && clip.notetrackShots && !actor.weapon.empty())
                    for (const auto tick : GetNotetrackTicks(actor, k, *info, before(FIRING_FACE), to, IsShotNotetrack))
                        shots.push_back(tick);
                if (melee)
                    for (const auto tick : GetNotetrackTicks(actor, k, *info, before(*melee), to, IsMeleeNotetrack))
                        windows.push_back({FACE_MELEE, 2, tick, tick + *melee});
                // dead while the clip of a death shows
                const auto end = k + 1 < clips.size() ? clips[k + 1].start : UINT32_MAX;
                if (death && clip.start <= to && end > from && end > clip.start && !clip.loop &&
                    info->death.value_or(IsDeathAnimation(clip.animation, clip.loop, info->notetracks)))
                    windows.push_back({FACE_DEATH, 3, clip.start, end});
            }
            std::ranges::sort(shots);
            for (std::size_t i = 0; i < shots.size();)
            {
                auto end = shots[i] + FIRING_FACE;
                auto j = i + 1;
                while (j < shots.size() && shots[j] <= end)
                    end = shots[j++] + FIRING_FACE;
                windows.push_back({FACE_FIRING, 1, shots[i], end});
                i = j;
            }
            return windows;
        }

        // the window the face is in at a tick, the one that goes over the others (of those, the latest), null for its
        // idle
        const FaceWindow* GetFaceWindow(const std::vector<FaceWindow>& windows, std::uint32_t tick)
        {
            const FaceWindow* found = nullptr;
            for (const auto& window : windows)
                if (window.start <= tick && tick < window.end &&
                    (!found || window.priority > found->priority ||
                     (window.priority == found->priority && window.start >= found->start)))
                    found = &window;
            return found;
        }

        // The Face row's clips at a tick: the last to start by then while it plays, blending in over its blend in from
        // the one before it if that one played up to its start (else from what the face plays without them), and
        // after it ends, blending out over FACE_BLEND
        std::vector<FaceSample> GetFaceClips(const Types::Actor& actor, std::uint32_t tick)
        {
            std::vector<FaceSample> samples;
            const auto& clips = actor.faceClips;
            const auto current = std::ranges::find_if(clips.rbegin(), clips.rend(),
                                                      [&](const Types::ActorClip& clip) { return clip.start <= tick; });
            if (current == clips.rend())
                return samples;
            const auto timeOf = [&](const Types::ActorClip& clip, std::uint32_t limit) {
                return clip.offset + std::min(tick - clip.start, limit) / 1000.0f * std::max(clip.speed, 0.0f);
            };
            const auto& clip = *current;
            if (tick >= clip.End())
            {
                const auto since = tick - clip.End();
                if (since < FACE_BLEND)
                    samples.push_back({&clip, clip.animation, timeOf(clip, clip.length + FACE_BLEND), clip.loop,
                                       1.0f - static_cast<float>(since) / FACE_BLEND});
                return samples;
            }
            const auto since = tick - clip.start;
            const auto weight =
                clip.blendIn > 0 && since < clip.blendIn ? static_cast<float>(since) / clip.blendIn : 1.0f;
            samples.push_back({&clip, clip.animation, timeOf(clip, clip.length), clip.loop, weight});
            const auto previous = std::next(current);
            if (weight < 1.0f && previous != clips.rend() && previous->End() >= clip.start)
                samples.push_back({&*previous, previous->animation, timeOf(*previous, previous->length + clip.blendIn),
                                   previous->loop, 1.0f - weight});
            return samples;
        }

        // What Face Auto has the face play at a tick: its windows' animations and its idle, each change blending from
        // the mix there was as it started (changes from FACE_LOOKBACK back on: before that, as if it had been so all
        // along)
        std::vector<FaceSample> GetAutomaticFace(const Types::Actor& actor, std::uint32_t tick,
                                                 const AnimationLookup& animations)
        {
            std::vector<FaceSample> samples;
            std::array<Types::ActorAnimationSample, 2> shown{};
            const auto count = Sample(actor, tick, shown);
            if (count == 0)
                return samples;
            const auto from = tick > FACE_LOOKBACK ? tick - FACE_LOOKBACK : 0u;
            const auto windows = GetFaceWindows(actor, from, tick, animations);

            // where it changes: where windows start and end
            std::vector<std::uint32_t> changes;
            for (const auto& window : windows)
                for (const auto at : {window.start, window.end})
                    if (at > from && at <= tick)
                        changes.push_back(at);
            std::ranges::sort(changes);
            changes.erase(std::ranges::unique(changes).begin(), changes.end());

            using Mix = std::vector<std::pair<const FaceWindow*, float>>;
            const auto mixAt = [](const Mix& mix, const FaceWindow* to, std::uint32_t start, std::uint32_t at) {
                const auto amount = std::clamp(static_cast<float>(at - start) / FACE_BLEND, 0.0f, 1.0f);
                Mix blended;
                for (const auto& [window, weight] : mix)
                    blended.emplace_back(window, weight * (1.0f - amount));
                const auto same = std::ranges::find(blended, to, &std::pair<const FaceWindow*, float>::first);
                if (same != blended.end())
                    same->second += amount;
                else
                    blended.emplace_back(to, amount);
                std::erase_if(blended, [](const auto& part) { return part.second <= 1e-6f; });
                return blended;
            };
            auto target = GetFaceWindow(windows, from);
            Mix blendFrom{{target, 1.0f}};
            auto blendStart = from;
            for (const auto at : changes)
            {
                const auto now = GetFaceWindow(windows, at);
                if (now == target)
                    continue;
                blendFrom = mixAt(blendFrom, target, blendStart, at);
                target = now;
                blendStart = at;
            }

            // the idle: alert or casual as the clips shown are, weighed as they are, looping from the first clip on
            const auto has = [&](std::string_view name) { return animations(std::string(name)).has_value(); };
            const auto idleTime = static_cast<float>(tick - std::min(tick, actor.clips.front().start)) / 1000.0f;
            const auto add = [&](std::string_view animation, float time, bool loop, float weight) {
                const auto same = std::ranges::find(samples, animation, &FaceSample::animation);
                if (same != samples.end())
                    same->weight += weight;
                else
                    samples.push_back({nullptr, std::string(animation), time, loop, weight});
            };
            for (const auto& [window, weight] : mixAt(blendFrom, target, blendStart, tick))
            {
                if (window)
                {
                    add(window->animation, static_cast<float>(tick - window->start) / 1000.0f, false, weight);
                    continue;
                }
                for (std::size_t i = 0; i < count; ++i)
                {
                    const bool alert = IsAlertAnimation(shown[i].clip->animation, animations(shown[i].clip->animation));
                    const auto idle = alert && has(FACE_ALERT) ? FACE_ALERT
                                      : has(FACE_CASUAL)       ? FACE_CASUAL
                                                               : FACE_ALERT;
                    if (has(idle))
                        add(idle, idleTime, true, weight * shown[i].weight);
                }
            }
            // (all of the face, of what the game has)
            float total = 0.0f;
            for (const auto& sample : samples)
                total += sample.weight;
            if (total <= 1e-6f)
                return {};
            for (auto& sample : samples)
                sample.weight /= total;
            return samples;
        }
    }  // namespace

    bool IsAlertAnimation(std::string_view animation, const std::optional<AnimationInfo>& info)
    {
        if (info && info->alert)
            return *info->alert;
        if (info && (info->family == Types::AnimationFamily::MpPlayer ||
                     std::ranges::any_of(info->notetracks, [](const Types::ActorNotetrack& notetrack) {
                         return IsShotNotetrack(notetrack.name);
                     })))
            return true;
        return HasNamePart(animation, {"aim", "ads", "alert", "cqb", "combat", "exposed"}, {"shoot", "fire", "firing"});
    }

    bool IsMeleeNotetrack(std::string_view name)
    {
        return name == "melee" || name.starts_with("melee_");
    }

    std::vector<FaceSample> GetFace(const Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations)
    {
        auto face = GetFaceClips(actor, tick);
        float clips = 0.0f;
        for (const auto& sample : face)
            clips += sample.weight;
        if (actor.face != Types::ActorFace::Auto || clips >= 1.0f - 1e-6f)
            return face;
        for (auto sample : GetAutomaticFace(actor, tick, animations))
        {
            sample.weight *= 1.0f - clips;
            face.push_back(std::move(sample));
        }
        return face;
    }

    std::vector<std::string> GetFaceLibrary(const Types::Actor& actor, const AnimationLookup& animations)
    {
        std::vector<std::string> library;
        if (actor.face == Types::ActorFace::Auto)
            for (const auto animation : FACE_ANIMATIONS)
                if (animations(std::string(animation)))
                    library.emplace_back(animation);
        for (const auto& clip : actor.faceClips)
            if (std::ranges::find(library, clip.animation) == library.end())
                library.push_back(clip.animation);
        return library;
    }

    std::vector<Reload> GetReloads(const Types::Actor& actor, std::uint32_t until, const AnimationLookup& animations,
                                   const ReloadMomentsLookup& moments)
    {
        std::vector<Reload> reloads;
        for (std::size_t i = 0; i < actor.clips.size(); ++i)
        {
            const auto& clip = actor.clips[i];
            if (clip.start > until || clip.speed <= 0.0f)
                continue;
            const auto animation = animations(clip.animation);
            if (!animation || animation->duration <= 0.0f)
                continue;
            const auto reloaded = moments(clip);
            if (!reloaded || reloaded->in <= reloaded->out)
                continue;

            // the clip plays it from its start until the next clip starts or it ends
            const auto end = std::min(clip.End(), i + 1 < actor.clips.size() ? actor.clips[i + 1].start : UINT32_MAX);
            const auto duration = animation->duration;
            const auto tickAt = [&](std::int64_t play, float time) {
                return GetTickInPlay(clip, duration, play, time);
            };
            const auto firstPlay = GetPlayAt(clip, duration, clip.start);
            // a looping clip's play of the animation at the tick (or where the clip stops playing it), and a few before
            // it; a clip that doesn't loop plays it once
            const auto lastTick = std::min<std::uint32_t>(until, end);
            const auto lastPlay = static_cast<std::int64_t>(
                std::floor(((lastTick - clip.start) / 1000.0 * clip.speed + clip.offset) / duration));
            const auto fromPlay = clip.loop ? std::max(firstPlay, lastPlay - RECENT_RELOAD_PLAYS) : firstPlay;
            const auto toPlay = clip.loop ? lastPlay + 1 : firstPlay;
            for (auto play = fromPlay; play <= toPlay; ++play)
            {
                const auto out = tickAt(play, reloaded->out);
                const auto in = tickAt(play, reloaded->in);
                if (!out || !in)
                    continue;
                if (*out >= end || *out > until)
                    break;
                Reload reload{&clip, *out, *in, end, reloaded->dropped, std::nullopt};
                if (reloaded->swap)
                    reload.swap = tickAt(play, *reloaded->swap);
                reloads.push_back(reload);
            }
        }
        return reloads;
    }

    std::int64_t GetPlayAt(const Types::ActorClip& clip, float duration, std::uint32_t tick)
    {
        if (!clip.loop || duration <= 0.0f)
            return 0;
        const auto playing = tick >= clip.start && tick < clip.End();
        const auto seconds = clip.offset + (playing ? tick - clip.start : 0) / 1000.0 * std::max(clip.speed, 0.0f);
        return static_cast<std::int64_t>(std::floor(seconds / duration));
    }

    float GetTimeInPlay(const Types::ActorClip& clip, float duration, std::int64_t play, std::uint32_t tick)
    {
        if (duration <= 0.0f)
            return 0.0f;
        const auto elapsed = (static_cast<double>(tick) - static_cast<double>(clip.start)) / 1000.0;
        return static_cast<float>((clip.offset + elapsed * std::max(clip.speed, 0.0f)) / duration -
                                  static_cast<double>(play));
    }

    std::optional<std::uint32_t> GetTickInPlay(const Types::ActorClip& clip, float duration, std::int64_t play,
                                               float time)
    {
        if (clip.speed <= 0.0f)
            return std::nullopt;
        const auto seconds = (static_cast<float>(play) + time) * duration - clip.offset;
        if (seconds < 0.0f)
            return std::nullopt;
        return clip.start + static_cast<std::uint32_t>(std::ceil(seconds / clip.speed * 1000.0f - 0.001f));
    }

    ReloadState GetReloadState(const Reload& reload, std::uint32_t tick)
    {
        const auto stop = std::min(reload.in, reload.end);
        if (tick < reload.out || tick >= stop)
            return {};
        if (reload.swap && *reload.swap < stop && tick >= *reload.swap)
            return {true, HeldMagazine::New};
        return {true, reload.dropped ? HeldMagazine::None : HeldMagazine::Old};
    }

    std::optional<std::uint32_t> GetFallStart(const Reload& reload)
    {
        if (reload.dropped)
            return reload.out;
        if (reload.swap && *reload.swap < std::min(reload.in, reload.end))
            return reload.swap;
        return std::nullopt;
    }

    glm::vec3 GetFallingMiddle(const MagazineFall& fall, float seconds)
    {
        return glm::vec3(fall.start * glm::vec4(fall.middle, 1.0f)) + fall.velocity * seconds -
               glm::vec3(0.0f, 0.0f, MAGAZINE_GRAVITY / 2.0f * seconds * seconds);
    }

    float GetLandingTime(const MagazineFall& fall)
    {
        const auto height =
            glm::vec3(fall.start * glm::vec4(fall.middle, 1.0f)).z - fall.ground - MAGAZINE_HALF_THICKNESS;
        const auto discriminant = fall.velocity.z * fall.velocity.z + 2.0f * MAGAZINE_GRAVITY * height;
        if (height <= 0.0f || discriminant <= 0.0f)
            return 0.0f;
        return (fall.velocity.z + std::sqrt(discriminant)) / MAGAZINE_GRAVITY;
    }

    glm::mat4 GetMagazineTransform(const MagazineFall& fall, float seconds)
    {
        seconds = std::max(seconds, 0.0f);
        const auto startRotation = glm::normalize(glm::quat_cast(glm::mat3(fall.start)));
        // turning in its own plane, about its thin side
        const auto thin = startRotation * glm::vec3(0, 1, 0);
        const auto rotationAt = [&](float time) {
            return glm::normalize(glm::angleAxis(glm::radians(fall.spin * time), thin) * startRotation);
        };

        const auto landing = GetLandingTime(fall);
        glm::quat rotation;
        glm::vec3 middle;
        if (seconds < landing)
        {
            rotation = rotationAt(seconds);
            middle = GetFallingMiddle(fall, seconds);
        }
        else
        {
            // then it settles on its flat side, with the least turn from how it landed
            const auto landed = rotationAt(landing);
            const auto side = landed * glm::vec3(0, 1, 0);
            const glm::vec3 up(0.0f, 0.0f, side.z >= 0.0f ? 1.0f : -1.0f);
            const auto axis = glm::cross(side, up);
            const auto turn =
                glm::length(axis) > 1e-5f
                    ? glm::angleAxis(std::acos(std::clamp(glm::dot(side, up), -1.0f, 1.0f)), glm::normalize(axis))
                    : glm::quat(1, 0, 0, 0);
            auto settled = std::clamp((seconds - landing) / MAGAZINE_SETTLE_TIME, 0.0f, 1.0f);
            settled = settled * settled * (3.0f - 2.0f * settled);
            rotation = glm::slerp(landed, glm::normalize(turn * landed), settled);
            middle = GetFallingMiddle(fall, landing);
            middle.z = fall.ground + MAGAZINE_HALF_THICKNESS;
        }
        return glm::translate(glm::mat4(1.0f), middle) * glm::mat4_cast(rotation) *
               glm::translate(glm::mat4(1.0f), -fall.middle);
    }

    std::vector<bool> FitBones(std::span<const std::uint32_t> bones, std::uint32_t maxBones)
    {
        std::vector<bool> kept(bones.size(), false);
        std::uint32_t total = 0;
        for (std::size_t i = 0; i < bones.size(); ++i)
        {
            if (maxBones != 0 && total + bones[i] > maxBones)
            {
                // without the first there is nothing to put the others on
                if (i == 0)
                    break;
                continue;
            }
            kept[i] = true;
            total += bones[i];
        }
        return kept;
    }

    std::uint32_t AddClip(Types::Actor& actor, Types::ActorClip clip)
    {
        // a clip that was playing there ends where the new one starts
        for (auto& other : actor.clips)
            if (other.start < clip.start && other.End() > clip.start)
                other.length = clip.start - other.start;
        clip.id = 0;
        actor.clips.push_back(std::move(clip));
        Normalize(actor);
        // the new clip got the highest id
        return std::ranges::max(actor.clips, {}, &Types::ActorClip::id).id;
    }

    std::uint32_t AddFaceClip(Types::Actor& actor, Types::ActorClip clip)
    {
        for (auto& other : actor.faceClips)
            if (other.start < clip.start && other.End() > clip.start)
                other.length = clip.start - other.start;
        clip.id = 0;
        actor.faceClips.push_back(std::move(clip));
        Normalize(actor);
        return std::ranges::max(actor.faceClips, {}, &Types::ActorClip::id).id;
    }

    std::uint32_t AddEvent(Types::Actor& actor, Types::ActorEvent event)
    {
        event.id = 0;
        actor.events.push_back(std::move(event));
        Normalize(actor);
        return std::ranges::max(actor.events, {}, &Types::ActorEvent::id).id;
    }

    std::uint32_t DuplicateClip(Types::Actor& actor, std::uint32_t clipId)
    {
        const auto clip = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id);
        if (clip == actor.clips.end())
            return 0;
        auto copy = *clip;
        copy.start = clip->End();
        return AddClip(actor, std::move(copy));
    }

    std::uint32_t DuplicateEvent(Types::Actor& actor, std::uint32_t eventId)
    {
        const auto event = std::ranges::find(actor.events, eventId, &Types::ActorEvent::id);
        if (event == actor.events.end())
            return 0;
        auto copy = *event;
        copy.tick = event->End() + 500;
        // (a Hit's death is its own)
        copy.death = 0;
        return AddEvent(actor, std::move(copy));
    }

    const Types::ActorClip* FindDeathClip(const Types::Actor& actor, const Types::ActorEvent& hit)
    {
        if (hit.type != Types::ActorEventType::Hit || hit.death == 0)
            return nullptr;
        const auto clip = std::ranges::find(actor.clips, hit.death, &Types::ActorClip::id);
        return clip != actor.clips.end() ? &*clip : nullptr;
    }

    void LinkDeaths(Types::Actor& actor)
    {
        for (auto& event : actor.events)
        {
            if (event.type != Types::ActorEventType::Hit || event.death != 0)
                continue;
            const auto clip = std::ranges::find_if(actor.clips, [&](const Types::ActorClip& clip) {
                return clip.start == event.tick && !clip.loop && clip.chain == Types::ActorChain::None;
            });
            if (clip != actor.clips.end() && std::ranges::none_of(actor.events, [&](const Types::ActorEvent& other) {
                    return other.type == Types::ActorEventType::Hit && other.death == clip->id;
                }))
                event.death = clip->id;
        }
    }

    void MoveEvent(Types::Actor& actor, std::uint32_t eventId, std::uint32_t tick)
    {
        const auto event = std::ranges::find(actor.events, eventId, &Types::ActorEvent::id);
        if (event == actor.events.end() || event->tick == tick)
            return;
        if (const auto death = FindDeathClip(actor, *event))
            actor.clips[death - actor.clips.data()].start = tick;
        // (an effect of a killstreak's death goes off that much later in it; before its end, from its start)
        if (const auto death = std::ranges::find(actor.clips, event->death, &Types::ActorClip::id);
            event->type != Types::ActorEventType::Hit && event->death != 0 && death != actor.clips.end())
        {
            if (event->afterDeathEnd && tick < death->End())
                event->afterDeathEnd = false;
            const auto from = event->afterDeathEnd ? death->End() : death->start;
            event->deathAt = tick > from ? tick - from : 0;
        }
        event->tick = tick;
        Normalize(actor);
    }

    Types::ActorClip TrimClipStart(const Types::ActorClip& clip, std::int64_t start, std::uint32_t minLength)
    {
        const auto end = static_cast<std::int64_t>(clip.End());
        start = std::clamp<std::int64_t>(start, 0, std::max<std::int64_t>(end - minLength, 0));
        auto trimmed = clip;
        // the animation stays where it was on the timeline
        trimmed.offset =
            std::max(0.0f, clip.offset + (start - static_cast<std::int64_t>(clip.start)) / 1000.0f * clip.speed);
        trimmed.start = static_cast<std::uint32_t>(start);
        trimmed.length = static_cast<std::uint32_t>(std::max<std::int64_t>(end - start, 1));
        return trimmed;
    }

    Types::ActorClip TrimClipEnd(const Types::ActorClip& clip, std::int64_t end, std::uint32_t minLength)
    {
        auto trimmed = clip;
        trimmed.length = static_cast<std::uint32_t>(
            std::max<std::int64_t>(end - static_cast<std::int64_t>(clip.start), std::max(minLength, 1u)));
        return trimmed;
    }

    std::size_t InsertWaypoint(Types::Actor& actor, std::size_t after,
                               std::optional<std::pair<Types::ActorGait, bool>> gait)
    {
        auto& path = actor.path;
        if (path.empty())
        {
            path.push_back(actor.position);
            return 0;
        }
        after = std::min(after, path.size() - 1);
        glm::vec3 point;
        if (after + 1 < path.size())
        {
            point = (path[after] + path[after + 1]) / 2.0f;
        }
        else
        {
            // a stride past the end, on the way the path was going
            constexpr float STRIDE = 64.0f;
            auto direction = path.size() >= 2 ? path[after] - path[after - 1] : glm::vec3(1, 0, 0);
            direction.z = 0.0f;
            direction = glm::length(direction) > 0.01f ? glm::normalize(direction) : glm::vec3(1, 0, 0);
            point = path[after] + direction * STRIDE;
        }
        path.insert(path.begin() + after + 1, point);
        // (going on as the stretch it was put in)
        if (!actor.stretches.empty())
        {
            FitStretches(actor);
            auto stretch = actor.stretches[std::min(after, actor.stretches.size() - 1)];
            stretch.wait = 0;
            actor.stretches.insert(actor.stretches.begin() + static_cast<std::ptrdiff_t>(after + 1), stretch);
            // (the stretch to it at the gait given: the animation of that gait, the look of another one gone)
            if (gait && after < actor.stretches.size())
            {
                auto& to = actor.stretches[after];
                if (to.gait != gait->first || to.crouched != gait->second)
                {
                    const auto same = std::ranges::find_if(actor.stretches, [&](const Types::ActorStretch& each) {
                        return each.gait == gait->first && each.crouched == gait->second && each.look.empty();
                    });
                    to.gait = gait->first;
                    to.crouched = gait->second;
                    to.look.clear();
                    const auto look = GetLook(actor, to);
                    to.animation = !look.empty()                   ? std::string(look)
                                   : same != actor.stretches.end() ? same->animation
                                                                   : std::string();
                }
            }
            FitStretches(actor);
        }
        return after + 1;
    }

    void RemoveWaypoint(Types::Actor& actor, std::size_t index)
    {
        if (index >= actor.path.size())
            return;
        actor.path.erase(actor.path.begin() + static_cast<std::ptrdiff_t>(index));
        // (the stretch before it goes on to the next point)
        if (index < actor.stretches.size())
            actor.stretches.erase(actor.stretches.begin() + static_cast<std::ptrdiff_t>(index));
        FitStretches(actor);
    }

    void AppendWaypoint(Types::Actor& actor, glm::vec3 point)
    {
        actor.path.push_back(point);
        FitStretches(actor);
    }

    void MoveWaypoint(Types::Actor& actor, std::size_t index, glm::vec3 to)
    {
        if (index >= actor.path.size())
            return;
        const auto was = actor.path[index];
        auto first = index, last = index;
        while (first > 0 && glm::distance(actor.path[first - 1], was) < STOP_DISTANCE)
            --first;
        while (last + 1 < actor.path.size() && glm::distance(actor.path[last + 1], was) < STOP_DISTANCE)
            ++last;
        for (auto i = first; i <= last; ++i)
            actor.path[i] = to;
    }

    void FitStretches(Types::Actor& actor)
    {
        auto& stretches = actor.stretches;
        if (stretches.empty())
            return;
        const auto count = std::max<std::size_t>(actor.path.size(), 1);
        if (stretches.size() > count)
            stretches.resize(count);
        // (the last point's goes as the last stretch walked: nothing walks it, and points added go on as it does)
        if (stretches.size() >= 2)
        {
            auto& last = stretches.back();
            const auto& walked = stretches[stretches.size() - 2];
            last.gait = walked.gait;
            last.crouched = walked.crouched;
            last.animation = walked.animation;
            last.pace = walked.pace;
        }
        while (stretches.size() < count)
        {
            auto last = stretches.back();
            last.wait = 0;
            stretches.push_back(std::move(last));
        }
    }

    namespace
    {
        // Makes room for a clip at a tick: the clip shown there (the last to start by then) is taken out if it starts
        // there, or cut there; returns it as it was while it still plays there
        std::optional<Types::ActorClip> MakeRoomAt(Types::Actor& actor, std::uint32_t tick)
        {
            const auto shown = std::ranges::find_if(actor.clips.rbegin(), actor.clips.rend(),
                                                    [&](const Types::ActorClip& clip) { return clip.start <= tick; });
            if (shown == actor.clips.rend())
                return std::nullopt;
            const auto clip = *shown;
            if (clip.start == tick)
                actor.clips.erase(std::next(shown).base());
            else if (clip.End() > tick)
                shown->length = tick - clip.start;
            return clip.End() > tick ? std::optional(clip) : std::nullopt;
        }

        // a death lies there for good: the clips that were to play after it go
        void DropClipsAfter(Types::Actor& actor, std::uint32_t tick)
        {
            std::erase_if(actor.clips, [&](const Types::ActorClip& clip) { return clip.start > tick; });
        }

        // where the first clip after a tick starts
        std::optional<std::uint32_t> GetNextStart(const Types::Actor& actor, std::uint32_t tick)
        {
            std::optional<std::uint32_t> next;
            for (const auto& clip : actor.clips)
                if (clip.start > tick)
                    next = std::min(next.value_or(UINT32_MAX), clip.start);
            return next;
        }

        std::uint32_t ToTicks(float seconds)
        {
            return std::max(1u, static_cast<std::uint32_t>(std::lround(seconds * 1000.0f)));
        }

        // a clip moved to start at a tick, keeping its end, its animation where it was on the timeline; one that
        // ended before then is as long as the gap
        Types::ActorClip MoveStart(const Types::ActorClip& clip, std::uint32_t start)
        {
            if (start + 1 <= clip.End())
                return TrimClipStart(clip, start, 1);
            auto moved = clip;
            moved.start = start;
            moved.length = 1;
            return moved;
        }
    }  // namespace

    std::uint32_t AddDeath(Types::Actor& actor, std::uint32_t tick, const std::string& animation, float duration)
    {
        MakeRoomAt(actor, tick);
        DropClipsAfter(actor, tick);
        const auto death = AddClip(actor, {.animation = animation, .start = tick, .blendIn = DEATH_BLEND});
        return MakeDeath(actor, death, duration);
    }

    namespace
    {
        // A killstreak's death: its Hit the game's blow (an explosion, sparks, with
        // its sound) instead of blood, and the game's effects and its Hide as events linked to it (those of the death
        // it was before go)
        void LinkVehicleDeath(Types::Actor& actor, std::uint32_t clipId, std::uint32_t hitId)
        {
            namespace Killstreaks = Components::KillstreakLogic;
            const auto killstreak = Killstreaks::FindByModels(actor.models);
            const auto clip = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id);
            if (!killstreak || clip == actor.clips.end() || !Killstreaks::IsDeathMotion(clip->animation))
                return;
            std::erase_if(actor.events, [&](const Types::ActorEvent& event) {
                return event.type != Types::ActorEventType::Hit && event.death == clipId;
            });
            const auto start = clip->start, end = clip->End();
            for (const auto& each :
                 Killstreaks::GetDeathEvents(Killstreaks::Get(*killstreak), clip->animation, clip->length))
            {
                if (each.type == Types::ActorEventType::Hit)
                {
                    if (const auto hit = std::ranges::find(actor.events, hitId, &Types::ActorEvent::id);
                        hit != actor.events.end())
                    {
                        hit->effect = each.effect;
                        hit->tag = each.tag;
                    }
                    continue;
                }
                AddEvent(actor, {.type = each.type,
                                 .tick = (each.afterEnd ? end : start) + each.at,
                                 .effect = std::string(each.effect),
                                 .tag = std::string(each.tag),
                                 .death = clipId,
                                 .deathAt = each.at,
                                 .afterDeathEnd = each.afterEnd});
            }
            Normalize(actor);
        }
    }  // namespace

    std::uint32_t MakeDeath(Types::Actor& actor, std::uint32_t clipId, float duration)
    {
        const auto clip = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id);
        if (clip == actor.clips.end())
            return 0;
        clip->loop = false;
        clip->moves = false;
        clip->rootMotion = true;
        clip->offset = 0.0f;
        if (duration > 0.0f)
            clip->length = ToTicks(duration / std::max(clip->speed, 0.01f));
        // (it lies there for good: what runs into it ends there, what would start after it goes)
        const auto start = clip->start;
        std::erase_if(actor.clips,
                      [&](const Types::ActorClip& each) { return each.id != clipId && each.start >= start; });
        for (auto& each : actor.clips)
            if (each.id != clipId && each.End() > start)
                each.length = std::max(start - each.start, 1u);
        // its Hit: one that dies with it, else one there that dies with nothing, else a fatal wound's blood
        const auto isHit = [](const Types::ActorEvent& event) { return event.type == Types::ActorEventType::Hit; };
        auto hit = std::ranges::find_if(
            actor.events, [&](const Types::ActorEvent& event) { return isHit(event) && event.death == clipId; });
        if (hit == actor.events.end())
            hit = std::ranges::find_if(actor.events, [&](const Types::ActorEvent& event) {
                return isHit(event) && event.death == 0 && event.tick == start;
            });
        std::uint32_t id = 0;
        if (hit == actor.events.end())
            id = AddEvent(actor, {.type = Types::ActorEventType::Hit,
                                  .tick = start,
                                  .effect = std::string(FATAL_BLOOD),
                                  .death = clipId});
        else
        {
            hit->death = clipId;
            id = hit->id;
            Normalize(actor);
        }
        LinkVehicleDeath(actor, clipId, id);
        return id;
    }

    bool IsDeathClip(const Types::ActorClip& clip, const AnimationLookup& animations)
    {
        return !clip.loop && IsDeath(clip.animation, animations(clip.animation));
    }

    std::uint32_t SwapAnimation(Types::Actor& actor, std::uint32_t clipId, const std::string& animation,
                                const AnimationLookup& animations)
    {
        const auto clip = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id);
        if (clip == actor.clips.end())
            return 0;
        const bool wasDeath = IsDeathClip(*clip, animations);
        const auto info = animations(animation);
        clip->animation = animation;
        clip->picked = true;
        if (info)
            clip->loop = info->loop;
        const auto duration = info ? info->duration : 0.0f;
        if (IsDeathClip(*clip, animations))
            return MakeDeath(actor, clipId, duration);
        // (the Hit goes with the death; a killstreak's death's effects too)
        if (wasDeath)
            std::erase_if(actor.events, [&](const Types::ActorEvent& event) { return event.death == clipId; });
        if (!clip->loop)
        {
            // a one-off: once, moved by its root motion where it plays
            clip->moves = false;
            clip->rootMotion = true;
            clip->offset = 0.0f;
            if (duration > 0.0f)
                clip->length = ToTicks(duration / std::max(clip->speed, 0.01f));
        }
        else if (info)
        {
            // a loop that walks, as walks and runs do: along the path
            const auto motion = info->motionPerPlay ? info->motionPerPlay : info->catalogMotion;
            if (motion && glm::length(*motion) >= MIN_MOTION_PER_PLAY)
                clip->moves = true;
        }
        Normalize(actor);
        return 0;
    }

    bool TakeDeathsOffPath(Types::Actor& actor, const AnimationLookup& animations)
    {
        bool changed = false;
        for (auto& clip : actor.clips)
            if (clip.moves && IsDeathClip(clip, animations))
            {
                clip.moves = false;
                changed = true;
            }
        return changed;
    }

    std::optional<std::uint32_t> GetAnchoredStart(const Types::ActorClip& clip, const PathTiming& timing)
    {
        std::optional<std::uint32_t> at;
        switch (clip.anchor)
        {
            case Types::ActorAnchor::End:
                at = timing.arrival;
                break;
            case Types::ActorAnchor::Point:
                if (clip.anchorPoint < timing.reached.size())
                    at = timing.reached[clip.anchorPoint];
                break;
            case Types::ActorAnchor::Time:
                break;
        }
        if (!at)
            return std::nullopt;
        return static_cast<std::uint32_t>(
            std::max<std::int64_t>(static_cast<std::int64_t>(*at) + clip.anchorOffset, 0));
    }

    namespace
    {
        // whether a clip goes on after a one-off with what was shown before it, as PlayAt leaves it: the same clip,
        // starting as the one-off ends, its animation where it would have been by then (or, joined by Auto
        // transitions, which move it on, anywhere)
        bool IsResumedBy(const Types::ActorClip& before, const Types::ActorClip& oneOff, const Types::ActorClip& after,
                         bool joined)
        {
            if (before.End() != oneOff.start || after.start != oneOff.End())
                return false;
            const auto expected = before.offset + (after.start - before.start) / 1000.0f * std::max(before.speed, 0.0f);
            auto same = after;
            same.id = before.id;
            same.start = before.start;
            same.length = before.length;
            same.offset = before.offset;
            return same == before && (joined || std::abs(after.offset - expected) < 0.02f);
        }
    }  // namespace

    std::vector<OneOff> TakeOutOneOffs(Types::Actor& actor, const AnimationLookup& animations)
    {
        std::vector<OneOff> oneOffs;
        if (actor.movement != Types::ActorMovement::Path || actor.path.size() < 2)
            return oneOffs;
        auto facts = GetClipFacts(actor, animations);
        // (from the walk's first clip on: the first that walks the path, of the locomotion's or one picked)
        const auto firstWalk = std::ranges::find_if(facts, [](const ClipFacts& fact) { return fact.walk.has_value(); });
        if (firstWalk == facts.end())
            return oneOffs;
        const auto isDeath = [&](std::size_t k) {
            return !actor.clips[k].loop && IsDeath(actor.clips[k].animation, facts[k].animation);
        };

        // the one-offs in what was shown before them (cut there), with it going on after them or what comes next
        // starting as they end: what was shown put back together, and the transitions Auto transitions joined them
        // with taken out with them (a clip that walks the path itself, such as a moving loop, is no one-off, nor a loop
        // after the walk, such as an idle the user put there: the walk stops into it)
        const auto walkFrom = static_cast<std::size_t>(firstWalk - facts.begin());
        std::size_t walkTo = walkFrom;
        for (std::size_t k = walkFrom; k < facts.size(); ++k)
            if (facts[k].walk)
                walkTo = k;
        std::vector<Types::ActorClip> kept;
        bool changed = false;
        for (std::size_t k = 0; k < actor.clips.size(); ++k)
        {
            const auto& clip = actor.clips[k];
            const bool walks = facts[k].walk && facts[k].kind != Types::AnimationKind::Torso && clip.loop;
            if (k > walkFrom && clip.chain == Types::ActorChain::None && !facts[k].locomotion && !walks &&
                !isDeath(k) && !(clip.loop && k > walkTo))
            {
                // (its joins: before it, and after it)
                std::size_t joinsBefore = 0;
                while (joinsBefore < kept.size() &&
                       kept[kept.size() - 1 - joinsBefore].chain == Types::ActorChain::Join)
                    ++joinsBefore;
                auto after = k + 1;
                while (after < actor.clips.size() && actor.clips[after].chain == Types::ActorChain::Join)
                    ++after;
                if (joinsBefore < kept.size())
                {
                    auto& before = kept[kept.size() - 1 - joinsBefore];
                    auto span = clip;
                    span.start = joinsBefore > 0 ? kept[kept.size() - joinsBefore].start : clip.start;
                    span.length = (after > k + 1 ? actor.clips[after - 1].End() : clip.End()) - span.start;
                    const auto* next = after < actor.clips.size() ? &actor.clips[after] : nullptr;
                    const bool resumed = next && IsResumedBy(before, span, *next, joinsBefore > 0 || after > k + 1);
                    if (before.End() == span.start &&
                        (resumed || !next || (next->start >= span.End() && next->chain != Types::ActorChain::Join)))
                    {
                        oneOffs.push_back({.clip = clip,
                                           .resumedId = resumed ? std::optional(next->id) : std::nullopt,
                                           .joined = joinsBefore > 0 || after > k + 1});
                        kept.resize(kept.size() - joinsBefore);
                        kept.back().length = (resumed ? next->End() : span.End()) - kept.back().start;
                        k = resumed ? after : after - 1;
                        changed = true;
                        continue;
                    }
                }
            }
            kept.push_back(clip);
        }
        if (changed)
        {
            actor.clips = std::move(kept);
            facts = GetClipFacts(actor, animations);
        }

        // the death after the walk (and the clips after it, which it lies through for good): nothing in its place, so
        // with nothing of the user's after the walk, the walk gets to the end and holds there until the death
        std::optional<std::size_t> lastWalk;
        for (std::size_t k = 0; k < facts.size(); ++k)
            if (facts[k].walk)
                lastWalk = k;
        for (auto k = lastWalk.value_or(actor.clips.size()) + 1; k < actor.clips.size(); ++k)
        {
            const auto& clip = actor.clips[k];
            if (clip.chain != Types::ActorChain::None || !isDeath(k))
                continue;
            OneOff death{.clip = clip, .death = true};
            for (const auto& event : actor.events)
                if (event.type == Types::ActorEventType::Hit && event.death == clip.id)
                    death.hits.push_back(event.id);
            actor.clips.erase(actor.clips.begin() + static_cast<std::ptrdiff_t>(k), actor.clips.end());
            oneOffs.push_back(std::move(death));
            changed = true;
            break;
        }
        if (changed)
            Normalize(actor);
        std::ranges::stable_sort(oneOffs, {}, [](const OneOff& oneOff) { return oneOff.clip.start; });
        return oneOffs;
    }

    std::vector<std::uint32_t> PutBackOneOffs(Types::Actor& actor, std::vector<OneOff> oneOffs,
                                              const AnimationLookup& animations)
    {
        std::vector<std::uint32_t> joined;
        if (oneOffs.empty())
            return joined;
        const auto timing = GetPathTiming(actor, animations);
        for (auto& oneOff : oneOffs)
            if (const auto at = GetAnchoredStart(oneOff.clip, timing))
                oneOff.clip.start = *at;
        std::ranges::stable_sort(oneOffs, {}, [](const OneOff& oneOff) { return oneOff.clip.start; });
        // (their ids again, which the selection and the Hits know them by: a transition the plan put in meanwhile with
        // one gets another, the plan's own being made anew each time; another clip with one, new ones)
        const auto free = [&](std::uint32_t id) {
            const auto taken = std::ranges::find(actor.clips, id, &Types::ActorClip::id);
            if (taken == actor.clips.end())
                return id;
            if (taken->chain == Types::ActorChain::None)
                return 0u;
            taken->id = 0;
            return id;
        };
        for (auto& oneOff : oneOffs)
        {
            auto& clip = oneOff.clip;
            if (oneOff.death)
            {
                // the run goes on until it, or it stands until it; nothing after it
                MakeRoomAt(actor, clip.start);
                DropClipsAfter(actor, clip.start);
                clip.id = free(clip.id);
                const auto at = clip.start;
                actor.clips.push_back(std::move(clip));
                Normalize(actor);
                const auto death = std::ranges::find_if(actor.clips, [&](const Types::ActorClip& each) {
                    return each.start == at && each.chain == Types::ActorChain::None;
                });
                for (auto& event : actor.events)
                    if (std::ranges::find(oneOff.hits, event.id) != oneOff.hits.end())
                        event.death = death->id;
                Normalize(actor);
                // (what came after it lies with it)
                return joined;
            }
            // as PlayAt puts one in: what is shown there goes on after it, its animation where it would have been
            const auto shown = MakeRoomAt(actor, clip.start);
            if (const auto next = GetNextStart(actor, clip.start))
                clip.length = std::max(std::min(clip.length, *next - clip.start), 1u);
            clip.id = free(clip.id);
            const auto end = clip.End();
            actor.clips.push_back(clip);
            if (shown && shown->End() > end)
            {
                auto after = TrimClipStart(*shown, end, 1);
                after.length = shown->End() - end;
                after.id = oneOff.resumedId ? free(*oneOff.resumedId) : 0u;
                actor.clips.push_back(std::move(after));
            }
            Normalize(actor);
            if (oneOff.joined)
                joined.push_back(std::ranges::find_if(actor.clips, [&](const Types::ActorClip& each) {
                                     return each.start == clip.start && each.animation == clip.animation;
                                 })->id);
        }
        return joined;
    }

    void Anchor(Types::ActorClip& clip, const PathTiming& timing, bool death, std::uint32_t reach, bool takeOver)
    {
        const auto offsetFrom = [&](std::uint32_t tick) {
            return static_cast<std::int32_t>(static_cast<std::int64_t>(clip.start) - tick);
        };
        clip.anchorPoint = 0;
        // the path's end: from within reach before it on (a death made there: as it arrives)
        if (timing.arrival && clip.start + reach >= *timing.arrival)
        {
            clip.anchor = Types::ActorAnchor::End;
            clip.anchorOffset = offsetFrom(*timing.arrival);
            if (death && takeOver)
                clip.anchorOffset = std::max(clip.anchorOffset, 0);
            return;
        }
        // a one-off while it walks: the point it reaches nearest then
        std::optional<std::size_t> nearest;
        if (!death && timing.start && clip.start >= *timing.start)
            for (std::size_t i = 0; i + 1 < timing.reached.size(); ++i)
                if (timing.reached[i] && (!nearest || std::abs(offsetFrom(*timing.reached[i])) <
                                                          std::abs(offsetFrom(*timing.reached[*nearest]))))
                    nearest = i;
        if (nearest)
        {
            clip.anchor = Types::ActorAnchor::Point;
            clip.anchorPoint = static_cast<std::uint32_t>(*nearest);
            clip.anchorOffset = offsetFrom(*timing.reached[*nearest]);
            return;
        }
        clip.anchor = Types::ActorAnchor::Time;
        clip.anchorOffset = 0;
    }

    void AnchorMadeDeath(Types::Actor& actor, std::uint32_t clipId, const PathTiming& timing,
                         const AnimationLookup& animations)
    {
        const auto clip = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id);
        if (clip == actor.clips.end())
            return;
        const auto index = static_cast<std::size_t>(clip - actor.clips.begin());
        const auto facts = GetClipFacts(actor, animations);
        const auto& before = index > 0 ? actor.clips[index - 1] : *clip;
        const bool afterWalk = index > 0 && before.End() >= clip->start &&
                               (facts[index - 1].walk || IsPathTransition(before) ||
                                before.chain == Types::ActorChain::Walk);
        if (afterWalk && timing.arrival)
        {
            clip->anchor = Types::ActorAnchor::End;
            clip->anchorOffset = 0;
            clip->anchorPoint = 0;
            return;
        }
        Anchor(*clip, timing, true, ANCHOR_REACH, true);
    }

    std::vector<glm::vec3> OffsetPath(const std::vector<glm::vec3>& path, float left)
    {
        if (path.size() < 2)
            return path;
        // (to the left of a way along the ground)
        const auto side = [](glm::vec3 way) {
            way.z = 0.0f;
            return glm::length(way) > 0.01f ? glm::normalize(glm::vec3(-way.y, way.x, 0.0f)) : glm::vec3(0.0f);
        };
        std::vector<glm::vec3> moved;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            const auto in = i > 0 ? path[i] - path[i - 1] : path[1] - path[0];
            const auto out = i + 1 < path.size() ? path[i + 1] - path[i] : in;
            auto a = side(in), b = side(out);
            if (glm::length(a) < 0.5f)
                a = b;
            if (glm::length(b) < 0.5f)
                b = a;
            // (the mitre: half way between both sides, as much longer as keeps it that far from each; a hairpin no
            // more than twice as far)
            auto mitre = a + b;
            mitre = glm::length(mitre) > 0.01f ? glm::normalize(mitre) : a;
            moved.push_back(path[i] + mitre * left / std::max(glm::dot(mitre, a), 0.5f));
        }
        return moved;
    }

    bool ConvertPickedWalks(Types::Actor& actor, const Body& body, const AnimationLookup& animations,
                            const std::function<bool(std::string_view)>& has)
    {
        if (actor.movement != Types::ActorMovement::Path || actor.locomotion != Types::ActorLocomotion::Auto ||
            actor.stretches.empty() || actor.path.size() < 2 || IsVehicle(body))
            return false;
        const auto facts = GetClipFacts(actor, animations);
        const auto timing = GetPathTiming(actor, animations);
        const auto rounded = RoundActorPath(actor, facts);
        const auto stretchCount = actor.path.size() - 1;
        bool converted = false;
        for (std::size_t k = 0; k < actor.clips.size(); ++k)
        {
            auto& clip = actor.clips[k];
            const auto& walk = facts[k].walk;
            if (!clip.picked || (clip.chain != Types::ActorChain::None && clip.chain != Types::ActorChain::Walk) ||
                !clip.loop || !walk || !walk->angle || facts[k].kind == Types::AnimationKind::Torso ||
                std::abs(*walk->angle) > TransitionLogic::MAX_WALK_ANGLE || walk->speed < IDLE_SPEED)
                continue;
            // the stretches it covered: those whose middle it walked, else the one it set off on
            std::vector<std::size_t> covered;
            for (std::size_t i = 0; i < stretchCount; ++i)
                if (timing.left[i] && timing.reached[i + 1])
                    if (const auto middle = (*timing.left[i] + *timing.reached[i + 1]) / 2;
                        middle >= clip.start && middle < clip.End())
                        covered.push_back(i);
            if (covered.empty())
            {
                const auto progress = GetPathProgress(actor, clip.start + 1, animations);
                std::size_t on = 0;
                for (std::size_t i = 0; i < stretchCount; ++i)
                    if (const auto distance = GetPointDistance(rounded, i); distance && *distance <= progress)
                        on = i;
                covered.push_back(on);
            }
            const auto& first = actor.stretches[std::min(covered.front(), actor.stretches.size() - 1)];
            const auto gait =
                GetLookGait(clip.animation, body, animations).value_or(std::pair(first.gait, first.crouched));
            if (covered.size() == stretchCount)
            {
                std::erase_if(actor.looks, [&](const Types::ActorLook& look) {
                    return look.gait == gait.first && look.crouched == gait.second;
                });
                actor.looks.push_back({gait.first, gait.second, clip.animation});
            }
            for (const auto i : covered)
                if (i < actor.stretches.size())
                {
                    auto& stretch = actor.stretches[i];
                    stretch.gait = gait.first;
                    stretch.crouched = gait.second;
                    stretch.look = covered.size() == stretchCount ? std::string() : clip.animation;
                }
            // (the walk stands for the locomotion, which plays the look)
            if (auto own = GetGaitAnimation(body, gait.first, gait.second, has); !own.empty())
                clip.animation = std::move(own);
            clip.picked = false;
            converted = true;
        }
        if (converted)
        {
            FitStretches(actor);
            ResolveStretches(actor, body, has);
        }
        return converted;
    }

    bool IsDeathAnimation(std::string_view animation, bool loop, std::span<const Types::ActorNotetrack> notetracks)
    {
        if (loop)
            return false;
        if (std::ranges::any_of(notetracks, [](const Types::ActorNotetrack& notetrack) {
                return notetrack.name.starts_with("start_ragdoll");
            }))
            return true;
        // (not "soldier", which has a die in it)
        return HasNamePart(animation, {"die", "dead", "dying"}, {"death"});
    }

    bool IsDeath(std::string_view animation, const std::optional<AnimationInfo>& info)
    {
        if (info && info->death)
            return *info->death;
        return IsDeathAnimation(animation, info && info->loop,
                                info ? info->notetracks : std::span<const Types::ActorNotetrack>{});
    }

    Types::AnimationState GetSuitedState(const Types::Actor& actor, std::uint32_t tick, float speed,
                                         const PathTiming& timing, const AnimationLookup& animations)
    {
        using Types::AnimationMovement;
        using Types::AnimationPosture;
        const auto moving = speed > 130.0f  ? AnimationMovement::Run
                            : speed > 20.0f ? AnimationMovement::Walk
                                            : AnimationMovement::Stop;
        const Types::ActorClip* shown = nullptr;
        for (const auto& clip : actor.clips)
            if (clip.start <= tick && (!shown || clip.start >= shown->start) &&
                !IsDeath(clip.animation, animations(clip.animation)))
                shown = &clip;

        // a clip of the path's (setting off, a turn, a stop and its wait, the walk on after a turn) or its walk
        const auto walks = [&](const Types::ActorClip& clip) {
            if (clip.chain != Types::ActorChain::None && clip.chain != Types::ActorChain::Join)
                return true;
            const auto locomotion = FindLocomotion(actor, clip.animation);
            return locomotion && locomotion->gait != Gait::Idle && !locomotion->vehicle;
        };
        if (shown && actor.movement == Types::ActorMovement::Path && !actor.stretches.empty() && timing.start &&
            walks(*shown))
        {
            std::optional<std::size_t> stretch;
            for (std::size_t i = 0; i < timing.left.size() && i < actor.stretches.size(); ++i)
                if (timing.left[i] && *timing.left[i] <= tick)
                    stretch = i;
            const bool arrived = timing.arrival && tick >= *timing.arrival;
            const bool going = tick >= *timing.start && !arrived;
            const auto& on = actor.stretches[std::min(stretch.value_or(0), actor.stretches.size() - 1)];
            return {on.crouched ? AnimationPosture::Crouch : AnimationPosture::Stand,
                    going ? moving : AnimationMovement::Stop};
        }
        if (shown)
        {
            // (a transition is in its end state once it has played; a loop stays in its own)
            const auto info = animations(shown->animation);
            const auto state = !info ? std::nullopt : info->loop || !info->to ? info->from : info->to;
            if (state && state->posture != AnimationPosture::Unknown)
                return {state->posture, state->movement == AnimationMovement::Unknown ? moving : state->movement};
        }
        return {actor.stance == Types::ActorStance::Crouch  ? AnimationPosture::Crouch
                : actor.stance == Types::ActorStance::Prone ? AnimationPosture::Prone
                                                            : AnimationPosture::Stand,
                moving};
    }

    std::string DescribeSuitedState(const Types::AnimationState& state)
    {
        using Types::AnimationMovement;
        const auto posture = state.posture == Types::AnimationPosture::Crouch  ? "crouched"
                             : state.posture == Types::AnimationPosture::Prone ? "prone"
                                                                               : "standing";
        switch (state.movement)
        {
            case AnimationMovement::Walk:
                return state.posture == Types::AnimationPosture::Stand ? "walking" : std::format("{}, walking", posture);
            case AnimationMovement::Run:
            case AnimationMovement::Sprint:
                return state.posture == Types::AnimationPosture::Stand ? "running" : std::format("{}, running", posture);
            default:
                return posture;
        }
    }

    std::string GetAddedClipAnimation(const Types::Actor& actor, std::optional<std::uint32_t> selectedClip,
                                      std::string_view idle, const AnimationLookup& animations)
    {
        const auto selected = std::ranges::find(actor.clips, selectedClip.value_or(0), &Types::ActorClip::id);
        const auto* clip = selectedClip && selected != actor.clips.end() ? &*selected
                           : actor.clips.empty()                         ? nullptr
                                                                         : &actor.clips.back();
        if (!clip || IsDeath(clip->animation, animations(clip->animation)))
            return std::string(idle);
        return clip->animation;
    }

    std::uint32_t PlayAt(Types::Actor& actor, std::uint32_t tick, const std::string& animation, float duration,
                         bool loop, std::span<const Types::ActorNotetrack> notetracks,
                         std::optional<std::uint32_t> blendIn)
    {
        const auto shown = MakeRoomAt(actor, tick);
        // a death falls where it is, as quickly as AddDeath's, and lies there
        const bool death = IsDeathAnimation(animation, loop, notetracks);
        if (death)
            DropClipsAfter(actor, tick);
        const auto next = GetNextStart(actor, tick);
        // as long as what was shown there would have gone on
        const auto until = std::min(shown ? shown->End() : next ? *next : tick + 5000, next.value_or(UINT32_MAX));

        // one play (up to the next clip), looping if it loops: a longer clip plays it again
        Types::ActorClip clip{.animation = animation, .start = tick, .loop = loop, .picked = true};
        clip.length = std::max(std::min(ToTicks(duration), next ? *next - tick : UINT32_MAX), 1u);
        if (shown)
            clip.moves = shown->moves;
        if (death)
            clip.blendIn = DEATH_BLEND;
        if (blendIn)
            clip.blendIn = *blendIn;
        const auto id = AddClip(actor, clip);
        if (death)
        {
            MakeDeath(actor, id, duration);
            return id;
        }

        // after one play, what was shown goes on, its animation where it would have been
        const auto resume = tick + clip.length;
        if (shown && until > resume)
        {
            auto after = TrimClipStart(*shown, resume, 1);
            after.length = until - resume;
            AddClip(actor, after);
        }
        return id;
    }

    ClipAfter GetClipAfter(const Types::Actor& actor, std::uint32_t clipId)
    {
        const auto& clips = actor.clips;
        const auto index =
            static_cast<std::size_t>(std::ranges::find(clips, clipId, &Types::ActorClip::id) - clips.begin());
        if (index + 1 >= clips.size() || clips[index + 1].start > clips[index].End())
            return ClipAfter::Hold;
        return index > 0 && clips[index + 1].animation == clips[index - 1].animation ? ClipAfter::Resume
                                                                                     : ClipAfter::Next;
    }

    bool SetClipAfter(Types::Actor& actor, std::uint32_t clipId, ClipAfter after, std::uint32_t end)
    {
        auto& clips = actor.clips;
        const auto index =
            static_cast<std::size_t>(std::ranges::find(clips, clipId, &Types::ActorClip::id) - clips.begin());
        const auto was = GetClipAfter(actor, clipId);
        if (index >= clips.size() || after == was || was == ClipAfter::Next)
            return false;
        if (after == ClipAfter::Hold)
        {
            clips.erase(clips.begin() + static_cast<std::ptrdiff_t>(index + 1));
            return true;
        }
        if (after != ClipAfter::Resume || index == 0)
            return false;
        const auto from = clips[index].End();
        auto until = index + 1 < clips.size() ? clips[index + 1].start : end;
        if (until <= from)
            until = from + 5000;
        // the clip before it, its animation where it would have been by then
        auto resumed = clips[index - 1];
        resumed.offset = std::max(0.0f, resumed.offset + (from - resumed.start) / 1000.0f * resumed.speed);
        resumed.start = from;
        resumed.length = until - from;
        AddClip(actor, std::move(resumed));
        return true;
    }

    std::vector<glm::vec3> CutPath(const std::vector<glm::vec3>& path, float distance, float radius)
    {
        if (path.size() <= 1)
            return path;
        const auto rounded = RoundPath(path, radius);
        if (rounded.pieces.empty())
            return {path.front()};
        // walked to its end: all of it
        if (distance >= rounded.length - 1e-3f)
            return path;
        distance = std::max(distance, 0.0f);
        const auto after = std::ranges::upper_bound(rounded.pieces, distance, {}, &RoundedPath::Piece::distance);
        const auto index = static_cast<std::size_t>(std::max<std::ptrdiff_t>(after - rounded.pieces.begin() - 1, 0));
        const auto& piece = rounded.pieces[index];
        // its points before one, then the ones given (each unless it is where the one before is); the last is the stop,
        // which takes all of the stretch to it for the corner before it
        const auto cut = [&](std::size_t before, std::initializer_list<glm::vec3> added) {
            std::vector<glm::vec3> points(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(before));
            for (const auto point : added)
                if (points.empty() || glm::distance(points.back(), point) >= STOP_DISTANCE)
                    points.push_back(point);
            return points;
        };
        // the arc of a corner a straight piece (or none) before a piece comes from: the corner the stretch to that
        // piece's point starts at
        const auto arcBefore = [&](std::size_t at) -> const RoundedPath::Piece* {
            auto k = at;
            if (k > 0 && rounded.pieces[k - 1].radius <= 0.0f && rounded.pieces[k - 1].point == rounded.pieces[at].point)
                --k;
            if (k == 0)
                return nullptr;
            const auto& arc = rounded.pieces[k - 1];
            return arc.radius > 0.0f && !arc.stop && arc.point + 1 == rounded.pieces[at].point ? &arc : nullptr;
        };
        glm::vec3 tangent;
        const auto here = GetPieceAt(piece, distance - piece.distance, tangent);
        // on the curve leaving a stop: there (the curve, which leaves at a tangent for the next point, ends there)
        if (piece.radius > 0.0f && piece.stop)
            return cut(piece.point + 1, {here});
        const auto before = arcBefore(index);
        if (piece.radius > 0.0f)
        {
            // on the arc of a corner: its point moved back to where the arc's tangent there meets the stretch it came
            // along, which rounded as wide makes the arc up to there; the corner before keeps its arc, pinned by a
            // point where it ends when the stretch to the moved point is too short for it
            const auto meets = piece.start + piece.direction * piece.radius *
                                                 std::tan((distance - piece.distance) / piece.radius / 2.0f);
            if (before && glm::distance(path[before->point], meets) / 2.0f < GetCornerReach(*before) + 1e-3f)
            {
                glm::vec3 ignored;
                return cut(piece.point, {GetPieceAt(*before, before->length, ignored), meets, here});
            }
            return cut(piece.point, {meets, here});
        }
        // on a straight stretch: there; the corner before keeps its arc, pinned by a point where it ends when half the
        // stretch to its next point was all it reached
        if (before)
        {
            const auto reach = GetCornerReach(*before);
            const auto& corner = path[before->point];
            if (glm::distance(corner, path[piece.point]) / 2.0f < reach + 1e-3f &&
                glm::distance(corner, here) > reach + 1e-3f)
                return cut(piece.point, {piece.start, here});
        }
        return cut(piece.point, {here});
    }

    namespace
    {
        // A killstreak's flights up to a tick go on as they went once a new one starts there: each moving clip before
        // it keeps the speed it got to (its own from then on), which a flight that came to rest no longer sets once it
        // goes on into the new one (a short hop's lower peak)
        void HoldFlightSpeeds(Types::Actor& actor, std::uint32_t tick, const AnimationLookup& animations)
        {
            const auto facts = GetClipFacts(actor, animations);
            const auto flights = GetFlightProfile(actor, facts);
            if (!flights)
                return;
            for (std::size_t k = 0; k < actor.clips.size(); ++k)
            {
                auto& clip = actor.clips[k];
                if (clip.start >= tick || !facts[k].walk || clip.speed <= 0.0f)
                    continue;
                const auto speed = flights->pieces[k].to;
                if (std::abs(speed - facts[k].walk->speed * clip.speed) > 0.5f)
                    clip.walkSpeed = std::max(speed / clip.speed, 0.01f);
            }
        }

        // Fits a killstreak's flight to its path (see VehicleLogic::FitFlight): from how far along it and how fast its
        // walk clip starts, to rest at its path's end as the clip after it has blended in, at its acceleration: the
        // walk's blend in its speed-up, the blend in of the clip after it its slow-down. False for an actor without
        // flights, or a walk without a clip after it that stays.
        bool FitFlightClips(Types::Actor& actor, std::size_t index, std::optional<std::size_t> after,
                            const AnimationLookup& animations)
        {
            const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models);
            if (!killstreak || !after)
                return false;
            const auto acceleration = Components::KillstreakLogic::Get(*killstreak).acceleration;
            const auto facts = GetClipFacts(actor, animations);
            if (acceleration <= 0.0f || !facts[index].walk || facts[*after].walk)
                return false;
            const auto rounded = RoundActorPath(actor, facts);
            const auto flights = GetFlightProfile(actor, facts, rounded);
            if (!flights)
                return false;
            const auto& piece = flights->pieces[index];
            const auto& walk = actor.clips[index];
            const auto fit = Components::VehicleLogic::FitFlight(rounded.length - piece.distance, piece.from,
                                                                 facts[index].walk->speed * std::max(walk.speed, 0.01f),
                                                                 acceleration);
            const auto length = std::max(fit.length, MIN_WALK_LENGTH);
            actor.clips[index].length = length;
            // (one too short for its speed-up changes its speed over all of it)
            actor.clips[index].blendIn = fit.length < MIN_WALK_LENGTH && piece.from > 0.0f ? length : fit.speedUp;
            auto& rest = actor.clips[*after];
            rest = MoveStart(rest, actor.clips[index].start + length);
            rest.blendIn = fit.slowDown;
            Normalize(actor);
            return true;
        }
    }  // namespace

    std::uint32_t WalkTo(Types::Actor& actor, std::uint32_t tick, glm::vec3 target, const std::string& walk,
                         const std::string& rest, const AnimationLookup& animations, float guessedSpeed)
    {
        const auto from = EvaluatePlacement(actor, tick, animations);
        HoldFlightSpeeds(actor, tick, animations);
        switch (actor.movement)
        {
            case Types::ActorMovement::Attached:
                return 0;
            case Types::ActorMovement::Still:
                // from where it stands, which the root motion of its clips before then moves it from as before
                actor.movement = Types::ActorMovement::Path;
                actor.path = {actor.position};
                // it stood until then
                for (auto& clip : actor.clips)
                    if (clip.start < tick)
                        clip.moves = false;
                break;
            case Types::ActorMovement::Path:
                if (actor.path.empty())
                    actor.path = {from.position};
                else
                {
                    // to where it has walked to, and a stop there, which it leaves curving onto the way to the target
                    actor.path = CutPath(actor.path, GetPathProgress(actor, tick, animations),
                                         GetCornerRadius(actor, GetPathSpeed(actor, GetClipFacts(actor, animations))));
                    if (actor.path.size() >= 2)
                        actor.path.push_back(actor.path.back());
                }
                break;
            case Types::ActorMovement::Keyframes:
            {
                // facing the way it goes from the tick on
                const auto way = glm::vec2(target - from.position);
                const auto yaw = glm::length(way) > 0.01f ? glm::degrees(std::atan2(way.y, way.x)) : from.yaw;
                ApplyPlacement(actor, tick, from.position, yaw, animations);
                break;
            }
        }

        // the walk, then what it did there; when that walks, what it was to do once it got there (the clip starting
        // where the walk ends), else rest
        const auto shownClip = std::ranges::find_if(actor.clips.rbegin(), actor.clips.rend(),
                                                    [&](const Types::ActorClip& clip) { return clip.start <= tick; });
        const bool shownWalks =
            shownClip != actor.clips.rend() && GetClipWalk(actor, *shownClip, animations).angle.has_value();
        auto shown = MakeRoomAt(actor, tick);
        std::optional<Types::ActorClip> then;
        if (shown && shownWalks)
        {
            const auto end = shown->End();
            const auto rested = std::ranges::find(actor.clips, end, &Types::ActorClip::start);
            if (rested != actor.clips.end())
            {
                then = *rested;
                actor.clips.erase(rested);
            }
            shown =
                then ? then : std::optional(Types::ActorClip{.animation = rest, .start = tick, .length = end - tick});
        }
        const auto next = GetNextStart(actor, tick);
        const auto walkId = AddClip(actor, {.animation = walk, .start = tick, .length = START_WALK_LENGTH});
        const auto arrival = tick + START_WALK_LENGTH;
        const auto until = std::min(shown ? shown->End() : arrival + 5000, next.value_or(UINT32_MAX));
        if (until > arrival)
        {
            auto after = shown ? MoveStart(*shown, arrival) : Types::ActorClip{.animation = rest, .start = arrival};
            after.length = until - arrival;
            if (!after.animation.empty())
                AddClip(actor, after);
        }

        if (actor.movement == Types::ActorMovement::Path)
        {
            if (glm::distance(actor.path.back(), target) > 0.5f)
                actor.path.push_back(target);
        }
        else
        {
            const auto way = glm::vec2(target - from.position);
            const auto yaw = glm::length(way) > 0.01f ? glm::degrees(std::atan2(way.y, way.x)) : from.yaw;
            ApplyPlacement(actor, arrival, target, yaw, animations);
        }
        FitWalk(actor, walkId, animations, guessedSpeed);
        return walkId;
    }

    void FitWalk(Types::Actor& actor, std::uint32_t walkClip, const AnimationLookup& animations, float guessedSpeed)
    {
        const auto index = static_cast<std::size_t>(std::ranges::find(actor.clips, walkClip, &Types::ActorClip::id) -
                                                    actor.clips.begin());
        if (index >= actor.clips.size())
            return;
        const auto walk = actor.clips[index];
        // the clip that starts where it ends, which starts where it gets to
        const auto after = index + 1 < actor.clips.size() && actor.clips[index + 1].start == walk.End()
                               ? std::optional(index + 1)
                               : std::nullopt;
        const auto withLength = [&](std::uint32_t length) {
            auto fitted = actor;
            fitted.clips[index].length = length;
            if (after)
                fitted.clips[*after] = MoveStart(actor.clips[*after], walk.start + length);
            return fitted;
        };
        const auto clipWalk = GetClipWalk(actor, actor.clips[index], animations);
        const auto speed = (clipWalk.angle ? clipWalk.speed : guessedSpeed) * std::max(walk.speed, 0.01f);

        if (actor.movement == Types::ActorMovement::Keyframes)
        {
            // from where it stands at the start to the next key
            const auto to =
                std::ranges::find_if(actor.keys, [&](const Types::ActorKey& key) { return key.tick > walk.start; });
            if (to == actor.keys.end() || speed <= 0.0f)
                return;
            // (between where its keys have it: root motion moves it as much at both)
            const auto distance = glm::distance(GetKeyedPlacement(actor.keys, walk.start).position, to->position);
            auto length = std::max(MIN_WALK_LENGTH, static_cast<std::uint32_t>(distance / speed * 1000.0f));
            // (not onto the key after it)
            if (const auto beyond = std::next(to); beyond != actor.keys.end())
                length = std::min(length, beyond->tick - 1 - walk.start);
            auto fitted = withLength(length);
            fitted.keys[to - actor.keys.begin()].tick = walk.start + length;
            Normalize(fitted);
            actor = std::move(fitted);
            return;
        }
        if (actor.movement != Types::ActorMovement::Path || FitFlightClips(actor, index, after, animations))
            return;

        const auto total = GetPathLength(actor, animations);
        std::uint32_t length = MIN_WALK_LENGTH;
        if (clipWalk.angle)
        {
            // as far along it as it gets by the time the clip after it has taken over
            const auto reached = [&](std::uint32_t tried) {
                const auto fitted = withLength(tried);
                const auto blend = after ? fitted.clips[*after].blendIn : 0u;
                return GetPathProgress(fitted, walk.start + tried + blend, animations);
            };
            constexpr std::uint32_t LONGEST = 600000;
            if (reached(length) < total)
            {
                std::uint32_t low = length, high = length;
                while (high < LONGEST && reached(high) < total)
                {
                    low = high;
                    high = std::min(high * 2, LONGEST);
                }
                while (high - low > 1)
                {
                    const auto middle = low + (high - low) / 2;
                    (reached(middle) < total ? low : high) = middle;
                }
                length = high;
            }
        }
        else if (speed > 0.0f)
        {
            const auto left = total - GetPathProgress(actor, walk.start, animations);
            length = std::max(MIN_WALK_LENGTH, static_cast<std::uint32_t>(std::max(left, 0.0f) / speed * 1000.0f));
        }
        actor = withLength(length);
        Normalize(actor);
    }

    std::int64_t SlideWalk(Types::Actor& actor, std::uint32_t setOff, std::int64_t delta)
    {
        // (from the clip it sets off with: its walk, or the walk's start)
        const auto shown = std::ranges::find_if(actor.clips.rbegin(), actor.clips.rend(),
                                                [&](const Types::ActorClip& clip) { return clip.start <= setOff; });
        if (shown != actor.clips.rend())
            setOff = shown->start;
        // the clip before, which lasts until it
        std::optional<std::size_t> before;
        for (std::size_t k = 0; k < actor.clips.size(); ++k)
            if (actor.clips[k].start < setOff)
                before = k;
        auto earliest = -static_cast<std::int64_t>(setOff);
        if (before)
            earliest = std::max(earliest, static_cast<std::int64_t>(actor.clips[*before].start) + MIN_WALK_LENGTH -
                                              static_cast<std::int64_t>(setOff));
        delta = std::max(delta, std::min<std::int64_t>(earliest, 0));
        if (delta == 0)
            return 0;
        const auto shift = [&](std::uint32_t& tick) {
            if (tick >= setOff)
                tick = static_cast<std::uint32_t>(static_cast<std::int64_t>(tick) + delta);
        };
        if (before)
        {
            auto& clip = actor.clips[*before];
            if (clip.End() >= setOff)
                clip.length = static_cast<std::uint32_t>(static_cast<std::int64_t>(setOff) + delta - clip.start);
        }
        for (std::size_t k = 0; k < actor.clips.size(); ++k)
            if (k != before)
                shift(actor.clips[k].start);
        for (auto& clip : actor.faceClips)
            shift(clip.start);
        for (auto& event : actor.events)
            shift(event.tick);
        for (auto& key : actor.keys)
            shift(key.tick);
        Normalize(actor);
        return delta;
    }

    void FitOnlyWalk(Types::Actor& actor, const AnimationLookup& animations)
    {
        if (actor.movement != Types::ActorMovement::Path)
            return;
        const auto facts = GetClipFacts(actor, animations);
        std::optional<std::uint32_t> walking;
        for (std::size_t k = 0; k < facts.size(); ++k)
        {
            if (!facts[k].walk)
                continue;
            if (walking)
                return;
            walking = actor.clips[k].id;
        }
        if (walking)
            FitWalk(actor, *walking, animations, actor.pathSpeed);
    }

    Types::Actor MakeStart(Start start, const StartAnimations& animations, std::uint32_t tick, std::uint32_t end,
                           const std::string& weapon)
    {
        Types::Actor actor;
        // (as its body was made for, and joined with the game's transitions)
        actor.movementSet = Types::ActorMovementSet::Auto;
        actor.autoTransitions = true;
        if (animations.locomotion)
        {
            actor.locomotion = Types::ActorLocomotion::Auto;
            actor.pathSpeed = start == Start::RunPath ? animations.runSpeed : animations.walkSpeed;
        }
        if (animations.face)
            actor.face = Types::ActorFace::Auto;
        end = std::max(end, tick + START_WALK_LENGTH + 1000);
        const auto add = [&](const std::string& animation, std::uint32_t from, std::uint32_t to, bool moves) {
            if (!animation.empty() && to > from)
                AddClip(actor, {.animation = animation,
                                .start = from,
                                .length = to - from,
                                .moves = moves,
                                .notetrackShots = start != Start::Shoot});
        };
        switch (start)
        {
            case Start::Standing:
                add(animations.idle, tick, end, true);
                break;
            case Start::WalkPath:
            case Start::RunPath:
                actor.movement = Types::ActorMovement::Path;
                actor.path = {glm::vec3(0.0f)};
                add(start == Start::WalkPath ? animations.walk : animations.run, tick, tick + START_WALK_LENGTH, true);
                add(animations.idle, tick + START_WALK_LENGTH, end, true);
                break;
            case Start::Shoot:
                actor.weapon = weapon;
                add(animations.shoot.empty() ? animations.idle : animations.shoot, tick, end, false);
                AddEvent(actor, {.type = Types::ActorEventType::Fire, .tick = tick, .shots = 10, .interval = 100});
                break;
            case Start::Die:
                add(animations.idle, tick, end, true);
                if (!animations.death.empty())
                    AddDeath(actor, tick + DIE_AFTER, animations.death, animations.deathDuration);
                break;
        }
        Normalize(actor);
        return actor;
    }

    StartAnimations GetDogStartAnimations(DogBreed breed, const AnimationLookup& animations)
    {
        const auto has = [&](std::string_view name) { return animations(std::string(name)).has_value(); };
        const auto body = Body::Dog(breed);
        const auto find = [&](Gait gait) {
            return FindLocomotionAnimation(body, Types::ActorStance::Stand, gait, Heading::Forward, has);
        };
        StartAnimations start{.idle = find(Gait::Idle), .walk = find(Gait::Walk), .run = find(Gait::Run)};
        start.locomotion = !start.idle.empty() && !start.walk.empty() && !start.run.empty();
        for (const auto death : {GetDogDeath(breed), GetDogDeath(DogBreed::Shepherd)})
            if (const auto info = animations(std::string(death)))
            {
                start.death = death;
                start.deathDuration = info->duration;
                break;
            }
        // the speeds of the breed the walk and run are of
        const auto presets = GetSpeedPresets(
            Body::Dog(FindLocomotion(start.walk).value_or(LocomotionAnimation{}).dog.value_or(DogBreed::Shepherd)));
        for (const auto& preset : presets)
        {
            if (preset.gait == Gait::Walk)
                start.walkSpeed = preset.speed;
            else if (preset.gait == Gait::Run)
                start.runSpeed = preset.speed;
        }
        return start;
    }

    void ChangeBody(Types::Actor& actor, const Body& from, const Body& to, const StartAnimations& defaults,
                    const AnimationLookup& animations)
    {
        if (from == to)
            return;
        const auto has = [&](std::string_view name) { return animations(std::string(name)).has_value(); };
        // (a killstreak, like a dog, stands and goes one way, with no face of a person's)
        const bool dog = IsDog(to) || IsVehicle(to);
        for (std::size_t k = 0; k < actor.clips.size(); ++k)
        {
            auto& clip = actor.clips[k];
            const auto info = animations(clip.animation);
            // the other's idles, walks and runs: the new one's of the gait
            if (const auto locomotion = FindLocomotion(clip.animation);
                locomotion && (!info || info->kind != Types::AnimationKind::Torso))
            {
                const bool fromDog = locomotion->dog.has_value() || locomotion->vehicle;
                const auto stance = dog || fromDog ? Types::ActorStance::Stand : locomotion->stance;
                const auto heading = dog || fromDog ? Heading::Forward : locomotion->heading;
                if (auto animation = FindLocomotionAnimation(to, stance, locomotion->gait, heading, has);
                    !animation.empty())
                    clip.animation = std::move(animation);
                continue;
            }
            // a death: the new one's, a play of it up to the next clip
            const bool death = info && info->death ? *info->death
                                                   : IsDeathAnimation(clip.animation, clip.loop,
                                                                      info ? info->notetracks
                                                                           : std::span<const Types::ActorNotetrack>{});
            if (!death || defaults.death.empty() || clip.animation == defaults.death)
                continue;
            clip.animation = defaults.death;
            clip.offset = 0.0f;
            if (!clip.loop)
            {
                auto length = ToTicks(defaults.deathDuration / std::max(clip.speed, 0.01f));
                if (k + 1 < actor.clips.size() && actor.clips[k + 1].start > clip.start)
                    length = std::min(length, actor.clips[k + 1].start - clip.start);
                clip.length = std::max(length, 1u);
            }
        }
        if (dog)
        {
            actor.stance = Types::ActorStance::Auto;
            actor.face = Types::ActorFace::Off;
        }
        else if (defaults.face)
            actor.face = Types::ActorFace::Auto;

        // a preset's speed: the new one's preset of that gait (or the fastest it has below it)
        if (actor.locomotion == Types::ActorLocomotion::Auto && actor.movement == Types::ActorMovement::Path)
        {
            std::optional<Gait> gait;
            for (const auto& preset : GetSpeedPresets(from))
                if (std::abs(actor.pathSpeed - preset.speed) < 0.5f)
                {
                    gait = preset.gait;
                    break;
                }
            if (gait)
            {
                std::optional<float> speed;
                for (const auto& preset : GetSpeedPresets(to))
                    if (preset.gait <= *gait && preset.stance == Types::ActorStance::Stand)
                        speed = preset.speed;
                if (speed && std::abs(*speed - actor.pathSpeed) >= 0.5f)
                {
                    actor.pathSpeed = *speed;
                    Normalize(actor);
                    FitOnlyWalk(actor, animations);
                }
            }
        }
        Normalize(actor);
    }

    void KeepMovementSet(Types::Actor& actor, Types::ActorMovementSet was, Types::ActorMovementSet now)
    {
        if (actor.movementSet == Types::ActorMovementSet::Auto && was != now && was != Types::ActorMovementSet::Auto &&
            !IsAsAdded(actor))
            actor.movementSet = was;
    }

    bool IsAsAdded(const Types::Actor& actor)
    {
        const bool walks = (actor.movement == Types::ActorMovement::Path && actor.path.size() >= 2) ||
                           (actor.movement == Types::ActorMovement::Keyframes && !actor.keys.empty());
        return actor.clips.size() <= 1 && !walks && actor.events.empty();
    }

    std::string FindFirst(std::span<const std::string_view> names, const std::function<bool(std::string_view)>& has)
    {
        const auto found = std::ranges::find_if(names, has);
        return found != names.end() ? std::string(*found) : std::string();
    }

    void AddRecent(std::vector<std::string>& recent, std::string_view name, std::size_t most)
    {
        std::erase(recent, name);
        recent.emplace(recent.begin(), name);
        if (recent.size() > most)
            recent.resize(most);
    }

    bool History::Record(const std::vector<Types::Actor>& before, bool sameStep)
    {
        redoSteps.clear();
        if (stepOpen || (sameStep && !undoSteps.empty()))
            return false;
        undoSteps.push_back(before);
        if (undoSteps.size() > MAX_STEPS)
            undoSteps.pop_front();
        stepOpen = true;
        return true;
    }

    void History::EndStep()
    {
        stepOpen = false;
    }

    std::optional<std::vector<Types::Actor>> History::Undo(std::vector<Types::Actor> current)
    {
        stepOpen = false;
        if (undoSteps.empty())
            return std::nullopt;
        redoSteps.push_back(std::move(current));
        auto actors = std::move(undoSteps.back());
        undoSteps.pop_back();
        return actors;
    }

    std::optional<std::vector<Types::Actor>> History::Redo(std::vector<Types::Actor> current)
    {
        stepOpen = false;
        if (redoSteps.empty())
            return std::nullopt;
        undoSteps.push_back(std::move(current));
        auto actors = std::move(redoSteps.back());
        redoSteps.pop_back();
        return actors;
    }

    bool History::CanUndo() const
    {
        return !undoSteps.empty();
    }

    bool History::CanRedo() const
    {
        return !redoSteps.empty();
    }

    void History::Clear()
    {
        undoSteps.clear();
        redoSteps.clear();
        stepOpen = false;
    }

    namespace
    {
        nlohmann::json ClipToJson(const Types::ActorClip& clip)
        {
            auto node = nlohmann::json{
                {"animation", clip.animation},
                {"start", clip.start},
                {"length", clip.length},
                {"speed", clip.speed},
                {"offset", clip.offset},
                {"loop", clip.loop},
                {"blendIn", clip.blendIn},
                {"moves", clip.moves},
                {"direction", static_cast<int>(clip.direction)},
                {"walkSpeed", clip.walkSpeed},
                {"notetrackShots", clip.notetrackShots},
                {"notetrackSounds", clip.notetrackSounds},
                {"rootMotion", clip.rootMotion},
                {"chain", static_cast<int>(clip.chain)},
            };
            if (clip.turn)
                node["turn"] = *clip.turn;
            if (clip.corner)
                node["corner"] = *clip.corner;
            if (clip.picked)
                node["picked"] = true;
            if (!clip.grip)
                node["grip"] = false;
            if (clip.spinRate > 0.0f)
                node["spinRate"] = clip.spinRate;
            if (clip.crashSpeed > 0.0f)
                node["crashSpeed"] = clip.crashSpeed;
            if (clip.anchor != Types::ActorAnchor::Time)
            {
                node["anchor"] = static_cast<int>(clip.anchor);
                node["anchorOffset"] = clip.anchorOffset;
                node["anchorPoint"] = clip.anchorPoint;
            }
            return node;
        }

        Types::ActorClip ClipFromJson(const nlohmann::json& node)
        {
            Types::ActorClip clip;
            clip.animation = node.value("animation", "");
            clip.start = node.value("start", 0u);
            clip.length = std::max(node.value("length", 3000u), 1u);
            clip.speed = node.value("speed", 1.0f);
            clip.offset = node.value("offset", 0.0f);
            clip.loop = node.value("loop", true);
            clip.blendIn = node.value("blendIn", 250u);
            clip.moves = node.value("moves", true);
            clip.direction = static_cast<Types::ActorWalkDirection>(
                std::clamp(node.value("direction", 0), 0, static_cast<int>(Types::ActorWalkDirection::Right)));
            clip.walkSpeed = std::max(node.value("walkSpeed", 0.0f), 0.0f);
            clip.spinRate = std::max(node.value("spinRate", 0.0f), 0.0f);
            clip.crashSpeed = std::max(node.value("crashSpeed", 0.0f), 0.0f);
            clip.notetrackShots = node.value("notetrackShots", true);
            clip.notetrackSounds = node.value("notetrackSounds", true);
            clip.grip = node.value("grip", true);
            // (scenes from before root motion get it too: their deaths slid to stay where they started)
            clip.rootMotion = node.value("rootMotion", true);
            clip.chain = static_cast<Types::ActorChain>(
                std::clamp(node.value("chain", 0), 0, static_cast<int>(Types::ActorChain::Walk)));
            if (const auto turn = node.find("turn"); turn != node.end() && turn->is_number())
                clip.turn = turn->get<float>();
            if (const auto corner = node.find("corner"); corner != node.end() && corner->is_number_unsigned())
                clip.corner = corner->get<std::uint32_t>();
            clip.picked = node.value("picked", false);
            clip.anchor = static_cast<Types::ActorAnchor>(
                std::clamp(node.value("anchor", 0), 0, static_cast<int>(Types::ActorAnchor::Point)));
            clip.anchorOffset = node.value("anchorOffset", 0);
            clip.anchorPoint = node.value("anchorPoint", 0u);
            return clip;
        }

        nlohmann::json StretchesToJson(const std::vector<Types::ActorStretch>& stretches)
        {
            auto list = nlohmann::json::array();
            for (const auto& stretch : stretches)
            {
                list.push_back({{"gait", static_cast<int>(stretch.gait)},
                                {"crouched", stretch.crouched},
                                {"animation", stretch.animation},
                                {"wait", stretch.wait},
                                {"blendAt", static_cast<int>(stretch.blendAt)},
                                {"matchFeet", stretch.matchFeet}});
                if (!stretch.look.empty())
                    list.back()["look"] = stretch.look;
                if (stretch.pace != 1.0f)
                    list.back()["pace"] = stretch.pace;
                if (stretch.blend)
                    list.back()["blend"] = *stretch.blend;
                if (stretch.joinAsBefore)
                    list.back()["joinAsBefore"] = true;
            }
            return list;
        }

        // (scenes from before joins have their joins as they were)
        std::vector<Types::ActorStretch> StretchesFromJson(const nlohmann::json& list)
        {
            std::vector<Types::ActorStretch> stretches;
            const bool beforeJoins =
                std::ranges::none_of(list, [](const nlohmann::json& node) { return node.contains("blendAt"); });
            for (const auto& node : list)
            {
                Types::ActorStretch stretch{
                    .gait = static_cast<Types::ActorGait>(
                        std::clamp(node.value("gait", 0), 0, static_cast<int>(Types::ActorGait::Sprint))),
                    .crouched = node.value("crouched", false),
                    .animation = node.value("animation", ""),
                    .wait = node.value("wait", 0u),
                    .look = node.value("look", ""),
                    .pace = std::clamp(node.value("pace", 1.0f), MIN_PACE, MAX_PACE),
                    .blendAt = static_cast<Types::ActorJoinAt>(
                        std::clamp(node.value("blendAt", 0), 0, static_cast<int>(Types::ActorJoinAt::After))),
                    .matchFeet = node.value("matchFeet", true),
                    .joinAsBefore = node.value("joinAsBefore", false)};
                if (const auto blend = node.find("blend"); blend != node.end() && blend->is_number_unsigned())
                    stretch.blend = std::clamp(blend->get<std::uint32_t>(), 1u, 2000u);
                stretches.push_back(std::move(stretch));
            }
            if (beforeJoins)
                for (std::size_t i = 1; i < stretches.size(); ++i)
                    if (!GoesAlike(stretches[i], stretches[i - 1]))
                        stretches[i].joinAsBefore = true;
            return stretches;
        }

        glm::vec3 VectorFromJson(const nlohmann::json& node, const char* name)
        {
            const auto values = node.value(name, std::vector<float>{0, 0, 0});
            return values.size() == 3 ? glm::vec3(values[0], values[1], values[2]) : glm::vec3(0);
        }

        nlohmann::json PropsToJson(const std::vector<Types::ActorProp>& props)
        {
            auto list = nlohmann::json::array();
            for (const auto& prop : props)
                list.push_back({{"model", prop.model},
                                {"tag", prop.tag},
                                {"offset", {prop.offset.x, prop.offset.y, prop.offset.z}},
                                {"angles", {prop.angles.x, prop.angles.y, prop.angles.z}}});
            return list;
        }

        std::vector<Types::ActorProp> PropsFromJson(const nlohmann::json& list)
        {
            std::vector<Types::ActorProp> props;
            for (const auto& node : list)
                props.push_back({node.value("model", ""), node.value("tag", "tag_weapon_left"),
                                 VectorFromJson(node, "offset"), VectorFromJson(node, "angles")});
            return props;
        }
    }  // namespace

    nlohmann::json LookToJson(const Types::Actor& actor)
    {
        return {{"models", actor.models}, {"character", actor.character},
                {"weapon", actor.weapon}, {"attachments", actor.attachments},
                {"camo", actor.camo},     {"facePaint", actor.facePaint},
                {"stowedWeapon", actor.stowedWeapon}, {"stowedAttachments", actor.stowedAttachments},
                {"stowedCamo", actor.stowedCamo},
                {"props", PropsToJson(actor.props)}};
    }

    void ApplyLook(Types::Actor& actor, const nlohmann::json& look)
    {
        if (const auto models = look.value("models", std::vector<std::string>{}); !models.empty())
        {
            actor.models = models;
            actor.character = look.value("character", "");
        }
        actor.weapon = look.value("weapon", "");
        actor.attachments = look.value("attachments", std::vector<std::string>{});
        std::ranges::sort(actor.attachments);
        actor.camo = look.value("camo", 0);
        actor.facePaint = look.value("facePaint", 0);
        actor.stowedWeapon = look.value("stowedWeapon", "");
        actor.stowedAttachments = look.value("stowedAttachments", std::vector<std::string>{});
        actor.stowedCamo = look.value("stowedCamo", 0);
        actor.props = PropsFromJson(look.value("props", nlohmann::json::array()));
    }

    nlohmann::json ClipsToJson(const Types::Actor& actor)
    {
        // from the first clip's start, of either row
        auto first = UINT32_MAX;
        for (const auto* row : {&actor.clips, &actor.faceClips})
            if (!row->empty())
                first = std::min(first, row->front().start);
        const auto list = [&](const std::vector<Types::ActorClip>& clips) {
            auto list = nlohmann::json::array();
            for (auto clip : clips)
            {
                clip.start -= first;
                list.push_back(ClipToJson(clip));
            }
            return list;
        };
        return {{"clips", list(actor.clips)}, {"faceClips", list(actor.faceClips)}};
    }

    void ApplyClips(Types::Actor& actor, const nlohmann::json& clips, std::uint32_t tick)
    {
        // (presets kept before the Face row are the list of clips alone)
        const auto add = [&](const nlohmann::json& list, bool face) {
            for (const auto& node : list)
            {
                auto clip = ClipFromJson(node);
                clip.start += tick;
                if (face)
                    AddFaceClip(actor, std::move(clip));
                else
                    AddClip(actor, std::move(clip));
            }
        };
        if (clips.is_array())
        {
            add(clips, false);
            return;
        }
        add(clips.value("clips", nlohmann::json::array()), false);
        add(clips.value("faceClips", nlohmann::json::array()), true);
    }

    std::vector<Types::Actor> MakeFormation(const Types::Actor& actor, const Formation& formation, float yaw)
    {
        std::vector<Types::Actor> copies;
        const auto columns = std::max(formation.columns, 1u);
        const glm::vec2 forward(std::cos(glm::radians(yaw)), std::sin(glm::radians(yaw)));
        const glm::vec2 right(forward.y, -forward.x);
        for (std::uint32_t i = 1; i < formation.count; ++i)
        {
            auto copy = actor;
            copy.id = 0;
            const auto column = static_cast<float>(i % columns);
            const auto row = static_cast<float>(i / columns);
            const auto shift2 = right * column * formation.spacing.x - forward * row * formation.spacing.y;
            const glm::vec3 shift(shift2, 0.0f);
            copy.position += shift;
            for (auto& key : copy.keys)
                key.position += shift;
            for (auto& point : copy.path)
                point += shift;
            // attached, the offset moves in the bone's axes, which the actor faces turned by its yaw from
            if (copy.movement == Types::ActorMovement::Attached)
            {
                const auto turn = glm::radians(copy.attachment.yaw);
                const glm::vec2 boneRight(std::sin(turn), -std::cos(turn)), boneForward(std::cos(turn), std::sin(turn));
                copy.attachment.offset +=
                    glm::vec3(boneRight * column * formation.spacing.x - boneForward * row * formation.spacing.y, 0.0f);
            }

            const auto delay = i * formation.stagger;
            for (auto& clip : copy.clips)
                clip.start += delay;
            for (auto& clip : copy.faceClips)
                clip.start += delay;
            for (auto& event : copy.events)
                event.tick += delay;
            for (auto& key : copy.keys)
                key.tick += delay;
            copies.push_back(std::move(copy));
        }
        return copies;
    }

    nlohmann::json SceneToJson(const Scene& scene, const std::string& demoName)
    {
        using json = nlohmann::json;
        json list = json::array();
        for (const auto& actor : scene.actors)
        {
            json clips = json::array();
            for (const auto& clip : actor.clips)
                clips.push_back(ClipToJson(clip));
            json faceClips = json::array();
            for (const auto& clip : actor.faceClips)
                faceClips.push_back(ClipToJson(clip));
            json keys = json::array();
            for (const auto& key : actor.keys)
                keys.push_back({{"tick", key.tick},
                                {"position", {key.position.x, key.position.y, key.position.z}},
                                {"yaw", key.yaw}});
            json path = json::array();
            for (const auto& point : actor.path)
                path.push_back({point.x, point.y, point.z});
            json events = json::array();
            for (const auto& event : actor.events)
            {
                // the actor looked at by where it is in the scene, as ids aren't saved
                const auto looked = std::ranges::find(scene.actors, event.lookActor, &Types::Actor::id);
                events.push_back({
                    {"type", static_cast<int>(event.type)},
                    {"tick", event.tick},
                    {"shots", event.shots},
                    {"interval", event.interval},
                    {"tracers", event.tracers},
                    {"hits", event.hits},
                    {"spread", event.spread},
                    {"rockets", event.rockets},
                    {"effect", event.effect},
                    {"tag", event.tag},
                    {"lookTarget", static_cast<int>(event.lookTarget)},
                    {"lookPoint", {event.lookPoint.x, event.lookPoint.y, event.lookPoint.z}},
                    {"lookActor", looked != scene.actors.end() && event.lookActor != 0
                                      ? static_cast<int>(looked - scene.actors.begin())
                                      : -1},
                    {"lookPlayer", event.lookPlayer},
                    {"lookTurn", event.lookTurn},
                });
                // a Hit's death by where it is among the clips, as ids aren't saved
                if (event.type == Types::ActorEventType::Hit)
                {
                    const auto death = std::ranges::find(actor.clips, event.death, &Types::ActorClip::id);
                    events.back()["death"] = event.death != 0 && death != actor.clips.end()
                                                 ? static_cast<int>(death - actor.clips.begin())
                                                 : -1;
                }
                // (a killstreak's death's effects and Hide: older builds play them as unlinked events)
                else if (const auto death = std::ranges::find(actor.clips, event.death, &Types::ActorClip::id);
                         event.death != 0 && death != actor.clips.end())
                {
                    events.back()["death"] = static_cast<int>(death - actor.clips.begin());
                    events.back()["deathAt"] = event.deathAt;
                    events.back()["afterDeathEnd"] = event.afterDeathEnd;
                }
            }
            const auto props = PropsToJson(actor.props);
            const auto& attachment = actor.attachment;
            const auto attachedTo = std::ranges::find(scene.actors, attachment.actor, &Types::Actor::id);
            list.push_back({
                {"name", actor.name},
                {"models", actor.models},
                {"character", actor.character},
                {"clips", clips},
                {"stance", static_cast<int>(actor.stance)},
                {"locomotion", static_cast<int>(actor.locomotion)},
                {"pathSpeed", actor.pathSpeed},
                {"movementSet", static_cast<int>(actor.movementSet)},
                {"autoTransitions", actor.autoTransitions},
                {"face", static_cast<int>(actor.face)},
                {"faceClips", faceClips},
                {"position", {actor.position.x, actor.position.y, actor.position.z}},
                {"yaw", actor.yaw},
                {"hidden", actor.hidden},
                {"movement", static_cast<int>(actor.movement)},
                {"keys", keys},
                {"path", path},
                {"followGround", actor.followGround},
                {"weapon", actor.weapon},
                {"attachments", actor.attachments},
                {"camo", actor.camo},
                {"stowedWeapon", actor.stowedWeapon},
                {"stowedAttachments", actor.stowedAttachments},
                {"stowedCamo", actor.stowedCamo},
                {"facePaint", actor.facePaint},
                {"events", events},
                {"props", props},
                {"attachment",
                 {{"kind", static_cast<int>(attachment.kind)},
                  {"actor", attachedTo != scene.actors.end() && attachment.actor != 0
                                ? static_cast<int>(attachedTo - scene.actors.begin())
                                : -1},
                  {"player", attachment.player},
                  {"tag", attachment.tag},
                  {"offset", {attachment.offset.x, attachment.offset.y, attachment.offset.z}},
                  {"yaw", attachment.yaw}}},
            });
            // (a flying killstreak's alone: scenes of others stay as they were)
            if (actor.flyHeight > 0.0f)
                list.back()["flyHeight"] = actor.flyHeight;
            // (a killstreak's effects turned off and its damage, when it has those)
            if (!actor.effectsOff.empty())
                list.back()["effectsOff"] = actor.effectsOff;
            // (a scene effect's)
            if (const auto& effect = actor.effect)
                list.back()["effect"] = {{"name", effect->name},   {"pitch", effect->pitch},
                                         {"roll", effect->roll},   {"start", effect->start},
                                         {"end", effect->end},     {"plays", static_cast<int>(effect->plays)},
                                         {"ends", static_cast<int>(effect->ends)}, {"every", effect->every}};
            // (a light's)
            if (const auto& light = actor.light)
            {
                list.back()["light"] = {{"id", light->id},
                                        {"kind", static_cast<int>(light->kind)},
                                        {"color", {light->color.r, light->color.g, light->color.b}},
                                        {"intensity", light->intensity},
                                        {"radius", light->radius},
                                        {"pitch", light->pitch},
                                        {"cone", light->cone},
                                        {"softness", light->softness},
                                        {"shadows", light->shadows},
                                        {"highlights", light->highlights},
                                        {"flickerAmount", light->flickerAmount},
                                        {"flickerRate", light->flickerRate},
                                        {"start", light->start},
                                        {"end", light->end}};
                if (const auto& follow = light->follow)
                    list.back()["light"]["follow"] = {
                        {"kind", static_cast<int>(follow->target.kind)},
                        {"id", follow->target.id},
                        {"bone", follow->bone},
                        {"offset", {follow->offset.x, follow->offset.y, follow->offset.z}},
                        {"firstPerson", static_cast<int>(follow->firstPerson)},
                        {"steady", follow->steady}};
            }
            // (a prop's on its own)
            // (the Mortar Team's points)
            if (const auto& mortar = actor.mortar)
            {
                json points = json::array();
                for (const auto& point : mortar->points)
                    points.push_back({point.x, point.y, point.z});
                list.back()["mortar"] = {{"points", points}};
            }
            if (const auto& prop = actor.placedProp)
                list.back()["placedProp"] = {{"id", prop->id},     {"model", prop->model}, {"pitch", prop->pitch},
                                             {"roll", prop->roll}, {"scale", prop->scale}, {"start", prop->start},
                                             {"end", prop->end}};
            if (actor.damage != Types::ActorDamage::None)
                list.back()["damage"] = {{"level", static_cast<int>(actor.damage)}, {"from", actor.damagedFrom}};
            // (paths with gaits alone, the same)
            if (!actor.stretches.empty())
            {
                list.back()["stretches"] = StretchesToJson(actor.stretches);
                list.back()["pace"] = actor.pace;
            }
            if (!actor.looks.empty())
            {
                auto looks = json::array();
                for (const auto& look : actor.looks)
                    looks.push_back({{"gait", static_cast<int>(look.gait)},
                                     {"crouched", look.crouched},
                                     {"animation", look.animation}});
                list.back()["looks"] = looks;
            }
        }
        return {{"demo", demoName}, {"pack", scene.pack}, {"players", scene.players}, {"actors", list}};
    }

    Scene SceneFromJson(const nlohmann::json& root, std::uint32_t demoEndTick)
    {
        using json = nlohmann::json;
        Scene scene;
        scene.pack = root.value("pack", "");
        scene.players = root.value("players", json::object());
        for (const auto& node : root.at("actors"))
        {
            Types::Actor actor;
            actor.id = static_cast<std::uint32_t>(scene.actors.size() + 1);
            actor.name = node.value("name", "");
            actor.models = node.value("models", std::vector<std::string>{});
            actor.character = node.value("character", "");
            // (a map character saved with its faction's head_1 gets its body's own: see CharacterLogic::GiveOwnHead)
            Components::CharacterLogic::GiveOwnHead(actor.models, actor.character);
            for (const auto& clipNode : node.value("clips", json::array()))
                actor.clips.push_back(ClipFromJson(clipNode));
            if (const auto animation = node.value("animation", ""); !animation.empty() && actor.clips.empty())
            {
                Types::ActorClip clip{.animation = animation, .start = node.value("animationStart", 0u)};
                clip.length = std::max(demoEndTick, clip.start + 1) - clip.start;
                clip.speed = node.value("animationSpeed", 1.0f);
                actor.clips.push_back(std::move(clip));
            }
            actor.stance = static_cast<Types::ActorStance>(
                std::clamp(node.value("stance", 0), 0, static_cast<int>(Types::ActorStance::Prone)));
            // (scenes from before locomotion walk as they did)
            actor.locomotion = static_cast<Types::ActorLocomotion>(
                std::clamp(node.value("locomotion", 0), 0, static_cast<int>(Types::ActorLocomotion::Auto)));
            actor.pathSpeed = std::max(node.value("pathSpeed", Types::Actor{}.pathSpeed), 0.0f);
            // (scenes from before transitions walk and run with the players' animations, and join nothing)
            actor.movementSet = static_cast<Types::ActorMovementSet>(
                std::clamp(node.value("movementSet", static_cast<int>(Types::ActorMovementSet::Players)), 0,
                           static_cast<int>(Types::ActorMovementSet::RunAndGun)));
            actor.autoTransitions = node.value("autoTransitions", false);
            // (scenes from before faces keep them still, as they were)
            actor.face = static_cast<Types::ActorFace>(
                std::clamp(node.value("face", 0), 0, static_cast<int>(Types::ActorFace::Auto)));
            for (const auto& clipNode : node.value("faceClips", json::array()))
                actor.faceClips.push_back(ClipFromJson(clipNode));
            const auto position = node.value("position", std::vector<float>{0, 0, 0});
            if (position.size() == 3)
                actor.position = {position[0], position[1], position[2]};
            actor.yaw = node.value("yaw", 0.0f);
            actor.hidden = node.value("hidden", false);
            actor.movement = static_cast<Types::ActorMovement>(
                std::clamp(node.value("movement", 0), 0, static_cast<int>(Types::ActorMovement::Attached)));
            for (const auto& keyNode : node.value("keys", json::array()))
            {
                const auto keyPosition = keyNode.value("position", std::vector<float>{0, 0, 0});
                if (keyPosition.size() == 3)
                    actor.keys.push_back({keyNode.value("tick", 0u),
                                          {keyPosition[0], keyPosition[1], keyPosition[2]},
                                          keyNode.value("yaw", 0.0f)});
            }
            for (const auto& pointNode : node.value("path", json::array()))
            {
                const auto point = pointNode.get<std::vector<float>>();
                if (point.size() == 3)
                    actor.path.emplace_back(point[0], point[1], point[2]);
            }
            actor.followGround = node.value("followGround", true);
            // (scenes from before gaits walk their paths at pathSpeed, as they did)
            actor.stretches = StretchesFromJson(node.value("stretches", json::array()));
            for (const auto& look : node.value("looks", json::array()))
                actor.looks.push_back({static_cast<Types::ActorGait>(std::clamp(
                                           look.value("gait", 0), 0, static_cast<int>(Types::ActorGait::Sprint))),
                                       look.value("crouched", false), look.value("animation", "")});
            actor.pace = std::clamp(node.value("pace", 1.0f), MIN_PACE_FROM_BEFORE, MAX_PACE_FROM_BEFORE);
            actor.flyHeight = std::max(node.value("flyHeight", 0.0f), 0.0f);
            actor.effectsOff = node.value("effectsOff", std::vector<std::string>{});
            std::ranges::sort(actor.effectsOff);
            if (const auto effect = node.find("effect"); effect != node.end() && effect->is_object())
                actor.effect = Types::ActorEffect{
                    .name = effect->value("name", ""),
                    .pitch = effect->value("pitch", -90.0f),
                    .roll = effect->value("roll", 0.0f),
                    .start = effect->value("start", 0u),
                    .end = effect->value("end", 0u),
                    .plays = static_cast<Types::EffectPlays>(
                        std::clamp(effect->value("plays", 0), 0, static_cast<int>(Types::EffectPlays::Loop))),
                    .ends = static_cast<Types::EffectEnds>(
                        std::clamp(effect->value("ends", 0), 0, static_cast<int>(Types::EffectEnds::Cut))),
                    .every = effect->value("every", 2000u)};
            if (const auto light = node.find("light"); light != node.end() && light->is_object())
            {
                const auto vector = [](const json& parent, const char* name, glm::vec3 fallback) {
                    const auto values = parent.value(name, std::vector<float>{});
                    return values.size() == 3 ? glm::vec3(values[0], values[1], values[2]) : fallback;
                };
                const Types::ActorLight defaults;
                actor.light = Types::ActorLight{
                    .id = light->value("id", 0u),
                    .kind = static_cast<Types::LightKind>(
                        std::clamp(light->value("kind", 0), 0, static_cast<int>(Types::LightKind::Spot))),
                    .color = vector(*light, "color", defaults.color),
                    .intensity = light->value("intensity", defaults.intensity),
                    .radius = light->value("radius", defaults.radius),
                    .pitch = light->value("pitch", defaults.pitch),
                    .cone = light->value("cone", defaults.cone),
                    .softness = light->value("softness", defaults.softness),
                    .shadows = light->value("shadows", false),
                    .highlights = light->value("highlights", defaults.highlights),
                    .flickerAmount = light->value("flickerAmount", 0.0f),
                    .flickerRate = light->value("flickerRate", defaults.flickerRate),
                    .start = light->value("start", 0u),
                    .end = light->value("end", 0u)};
                if (const auto follow = light->find("follow"); follow != light->end() && follow->is_object())
                    actor.light->follow = Types::LightFollow{
                        .target = {.kind = static_cast<Types::BoneCameraTargetKind>(
                                       std::clamp(follow->value("kind", 0), 0,
                                                  static_cast<int>(Types::BoneCameraTargetKind::Actor))),
                                   .id = follow->value("id", 0)},
                        .bone = follow->value("bone", ""),
                        .offset = vector(*follow, "offset", {}),
                        .firstPerson = static_cast<Types::LightFirstPerson>(std::clamp(
                            follow->value("firstPerson", 0), 0, static_cast<int>(Types::LightFirstPerson::ViewGun))),
                        .steady = std::clamp(follow->value("steady", 0.0f), 0.0f, 0.5f)};
            }
            if (const auto mortar = node.find("mortar"); mortar != node.end() && mortar->is_object())
            {
                actor.mortar = Types::ActorMortar{};
                for (const auto& point : mortar->value("points", json::array()))
                    if (point.is_array() && point.size() == 3)
                        actor.mortar->points.emplace_back(point[0].get<float>(), point[1].get<float>(),
                                                          point[2].get<float>());
            }
            if (const auto prop = node.find("placedProp"); prop != node.end() && prop->is_object())
                actor.placedProp = Types::ActorPlacedProp{.id = prop->value("id", 0u),
                                                          .model = prop->value("model", ""),
                                                          .pitch = prop->value("pitch", 0.0f),
                                                          .roll = prop->value("roll", 0.0f),
                                                          .scale = prop->value("scale", 1.0f),
                                                          .start = prop->value("start", 0u),
                                                          .end = prop->value("end", 0u)};
            if (const auto damage = node.find("damage"); damage != node.end() && damage->is_object())
            {
                actor.damage = static_cast<Types::ActorDamage>(
                    std::clamp(damage->value("level", 0), 0, static_cast<int>(Types::ActorDamage::Heavy)));
                actor.damagedFrom = damage->value("from", 0u);
            }
            actor.weapon = node.value("weapon", "");
            actor.attachments = node.value("attachments", std::vector<std::string>{});
            std::ranges::sort(actor.attachments);
            actor.camo = node.value("camo", 0);
            actor.stowedWeapon = node.value("stowedWeapon", "");
            actor.stowedAttachments = node.value("stowedAttachments", std::vector<std::string>{});
            actor.stowedCamo = node.value("stowedCamo", 0);
            actor.facePaint = node.value("facePaint", 0);
            // (Hits from before deaths were linked died with the clip that started with them)
            bool linkedByTick = false;
            for (const auto& eventNode : node.value("events", json::array()))
            {
                Types::ActorEvent event;
                event.type = static_cast<Types::ActorEventType>(
                    std::clamp(eventNode.value("type", 0), 0, static_cast<int>(Types::ActorEventType::Look)));
                event.tick = eventNode.value("tick", 0u);
                event.shots = eventNode.value("shots", 1u);
                event.interval = eventNode.value("interval", 100u);
                event.tracers = eventNode.value("tracers", true);
                event.hits = eventNode.value("hits", true);
                event.spread = eventNode.value("spread", -1.0f);
                event.rockets = eventNode.value("rockets", false);
                event.effect = eventNode.value("effect", "");
                event.tag = eventNode.value("tag", "");
                event.lookTarget = static_cast<Types::ActorLookTarget>(std::clamp(
                    eventNode.value("lookTarget", 1), 0, static_cast<int>(Types::ActorLookTarget::Player)));
                const auto lookPoint = eventNode.value("lookPoint", std::vector<float>{0, 0, 0});
                if (lookPoint.size() == 3)
                    event.lookPoint = {lookPoint[0], lookPoint[1], lookPoint[2]};
                event.lookActor = static_cast<std::uint32_t>(eventNode.value("lookActor", -1) + 1);
                event.lookPlayer = eventNode.value("lookPlayer", 0);
                event.lookTurn = eventNode.value("lookTurn", 400u);
                // (the clips get the ids 1, 2, ... in order)
                if (event.type == Types::ActorEventType::Hit)
                {
                    if (const auto death = eventNode.find("death"); death != eventNode.end() && death->is_number())
                        event.death = static_cast<std::uint32_t>(std::max(death->get<int>(), -1) + 1);
                    else
                        linkedByTick = true;
                }
                else if (const auto death = eventNode.find("death"); death != eventNode.end() && death->is_number())
                {
                    event.death = static_cast<std::uint32_t>(std::max(death->get<int>(), -1) + 1);
                    event.deathAt = eventNode.value("deathAt", 0u);
                    event.afterDeathEnd = eventNode.value("afterDeathEnd", false);
                }
                // (killstreaks play no sounds: the "sound" of events older builds saved is left out)
                actor.events.push_back(std::move(event));
            }
            const auto vector = VectorFromJson;
            actor.props = PropsFromJson(node.value("props", json::array()));
            if (const auto attachmentNode = node.value("attachment", json::object()); !attachmentNode.empty())
            {
                auto& attachment = actor.attachment;
                attachment.kind = static_cast<Types::ActorAttachment::Kind>(
                    std::clamp(attachmentNode.value("kind", 0), 0, static_cast<int>(Types::ActorAttachment::Kind::Player)));
                attachment.actor = static_cast<std::uint32_t>(attachmentNode.value("actor", -1) + 1);
                attachment.player = attachmentNode.value("player", 0);
                attachment.tag = attachmentNode.value("tag", "tag_origin");
                attachment.offset = vector(attachmentNode, "offset");
                attachment.yaw = attachmentNode.value("yaw", 0.0f);
            }
            Normalize(actor);
            if (linkedByTick)
                LinkDeaths(actor);
            scene.actors.push_back(std::move(actor));
        }
        return scene;
    }
}  // namespace IWXMVM::ActorLogic
