#include "StdInclude.hpp"
#include "Actors.hpp"

#include "nlohmann/json.hpp"

#include "ActorLogic.hpp"
#include "KillstreakLogic.hpp"
#include "LightLogic.hpp"
#include "Lights.hpp"
#include "PropLogic.hpp"
#include "Props.hpp"

#include "Events.hpp"
#include "Mod.hpp"
#include "Resources.hpp"
#include "Utilities/PathUtils.hpp"

#include <set>
#include "Configuration/Configuration.hpp"
#include "Components/Playback.hpp"
#include "Components/Loadouts.hpp"
#include "Components/PlayerAnimation.hpp"
#include "Components/WeaponCatalog.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Components/UndoHistory.hpp"
#include "Components/PoseSignatures.hpp"

namespace IWXMVM::Resources
{
    INCBIN_EXTERN(CHARACTERS);
    INCBIN_EXTERN(ANIMATIONS);
}

namespace IWXMVM::Components::Actors
{
    // the campaign's and Zombies' characters, read once as the mod starts
    CharacterLogic::CampaignCatalog campaignCharacters;
    std::unordered_map<std::string, Types::ActorCatalog::ModelKind> campaignModelKinds;
    CharacterLogic::ZoneIndex zoneIndex;
    // the character zones the setup built, as last looked at
    std::mutex zonesMutex;
    std::set<std::string, std::less<>> availableZones;
    std::set<std::string, std::less<>> zonesWithoutTextures;
    std::filesystem::path texturesFileInstead;
    std::filesystem::path extraZonesFolder;
    bool zonesIgnored = false;

    // The game draws the actors on its main thread while the UI changes them on the render thread
    std::mutex mutex;
    std::shared_ptr<const Types::ActorCatalog> catalog;
    std::vector<Types::Actor> actors;
    std::string pack;
    std::uint32_t nextId = 1;
    std::optional<std::uint32_t> selected;
    std::vector<PlacementRequest> placementRequests;
    std::atomic<std::uint32_t> generation = 0;

    // saving: the demo's scene is written a moment after it last changed, and read once the demo has loaded
    std::atomic<bool> loadPending = false;
    std::uint32_t savedGeneration = 0;
    std::chrono::steady_clock::time_point changedAt;

    std::optional<std::uint32_t> selectedClip;
    std::optional<std::uint32_t> selectedFaceClip;
    std::optional<std::size_t> selectedWaypoint;
    std::optional<std::uint32_t> selectedEvent;

    // walk clips kept as long as their paths take (see FitWalkWhilePlacing): fitted after each point placed until
    // placing stops, or once the walk's animation is measured
    struct WalkFit
    {
        std::uint32_t id;
        std::uint32_t clip;
        std::string animation;
        float guessedSpeed;
        bool placing;
        bool measured;  // when it was fitted last
    };
    std::vector<WalkFit> walkFits;
    // actors whose paths' transitions were planned before the game measured how their animations move them, planned
    // again once it has (see ReplanMeasuredPaths)
    struct PlanWait
    {
        std::uint32_t id;
        std::vector<std::string> animations;
    };
    std::vector<PlanWait> planWaits;

    // actors whose paths changed (points, gaits, looks, pace, stance, set, clips): their walks are fitted and their
    // transitions planned again once the edit ends (see RunPendingReplans), with what the changes were: the one-offs
    // the user moved or added (anchored again where they are, see ActorLogic::Anchor), whether its look or body
    // changed (it keeps its arrival then) and whether anything but its pace did (a pace asked for moves the arrival
    // without a note)
    struct PendingReplan
    {
        std::set<std::uint32_t> anchored;
        bool keepTime = false;
        bool noted = false;
    };
    std::map<std::uint32_t, PendingReplan> pendingReplans;
    // when each actor arrives as the panel and timeline last showed it (not while a re-plan is pending), for the notes
    std::map<std::uint32_t, std::optional<std::uint32_t>> shownArrivals;
    std::vector<ReplanFlash> flashes;
    // what re-plans changed that the user didn't ask for, said once, and those of a
    // change said once it is planned (see SetLook)
    std::vector<Note> notes;
    std::vector<Note> pendingNotes;
    // the looks tried on each actor's path, the latest first; the actor a look list loops a span of, and since when
    std::map<std::uint32_t, std::vector<std::string>> triedLooks;
    std::optional<PreviewLoop> previewLoop;
    std::chrono::steady_clock::time_point previewLoopSince;
    // clips added whose blend in is worked out once their poses are (see BlendCleanly), with the blend they had then
    struct PendingBlend
    {
        std::uint32_t actor;
        std::uint32_t clip;
        std::optional<std::uint32_t> blendIn;
        std::chrono::steady_clock::time_point asked;
    };
    std::vector<PendingBlend> pendingBlends;
    // (PoseSignatures gives up on one after 5 s)
    constexpr auto BLEND_WAIT = std::chrono::seconds(6);

    // the steps of changes the UI can undo, over the actors only (not the pack)
    ActorLogic::History history;
    // the step of the editor's undo history the actors' open step is: a change after another part of the editor
    // added a step starts a step of its own
    std::uint64_t openStep = 0;

    // before a change to the actors (with the mutex held): keeps them as they were, for undo, as a step of the
    // editor's history named by the label
    void Record(std::string label, bool sameStep = false)
    {
        if (!sameStep && UndoHistory::Get().GetLatest() != openStep)
            history.EndStep();
        if (history.Record(actors, sameStep))
            openStep = UndoHistory::Get().Push(std::move(label), [] { Undo(); }, [] { Redo(); });
    }

    void SortClips(Types::Actor& actor)
    {
        ActorLogic::Normalize(actor);
    }

    void Changed()
    {
        ++generation;
        changedAt = std::chrono::steady_clock::now();
    }

    std::filesystem::path GetScenePath()
    {
        const auto demoName = Mod::GetGameInterface()->GetDemoInfo().name;
        return PathUtils::GetModPath() / "scenes" / std::format("{:X}.json", std::hash<std::string>{}(demoName));
    }

    // what tells a choice for the players changed, which is saved with the scene too
    std::uint64_t savedPlayerGeneration = 0;

    std::uint64_t GetPlayerGeneration()
    {
        return static_cast<std::uint64_t>(Loadouts::GetChoicesGeneration()) + PlayerAnimation::GetChoicesGeneration();
    }

    void Save()
    {
        ActorLogic::Scene scene;
        {
            std::lock_guard lock(mutex);
            scene = {actors, pack};
        }
        Types::PlayerChoices choices;
        Loadouts::GetChoices(choices);
        PlayerAnimation::GetChoices(choices);
        scene.players = PlayerLookLogic::ChoicesToJson(choices);
        const auto root = ActorLogic::SceneToJson(scene, Mod::GetGameInterface()->GetDemoInfo().name);

        // written next to it first, so a crash while writing doesn't leave half a scene
        const auto path = GetScenePath();
        std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary);
            file << root.dump(4);
            if (!file)
            {
                LOG_ERROR("Failed to write the actors of this demo to {}", temporary.string());
                return;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
            LOG_ERROR("Failed to save the actors of this demo to {}: {}", path.string(), error.message());
    }

    // (a scene from before looks, its picked walks made looks: see ActorLogic::ConvertPickedWalks; returns whether it
    // was)
    bool ConvertOldWalks(Types::Actor& actor);

    void Load()
    {
        const auto path = GetScenePath();
        ActorLogic::Scene scene;
        if (std::filesystem::exists(path))
        {
            try
            {
                std::ifstream file(path);
                scene = ActorLogic::SceneFromJson(nlohmann::json::parse(file),
                                                  Mod::GetGameInterface()->GetDemoInfo().endTick);
            }
            catch (const std::exception& e)
            {
                // kept, as the next change saves the scene over it
                auto broken = path;
                broken += ".broken";
                std::error_code error;
                std::filesystem::copy_file(path, broken, std::filesystem::copy_options::overwrite_existing, error);
                LOG_ERROR("Failed to read the actors of this demo from {} (kept as {}): {}", path.string(),
                          broken.string(), e.what());
                scene = {};
            }
        }

        {
            std::lock_guard lock(mutex);
            actors.clear();
            // ids of this session, and the actors looked at with them
            std::map<std::uint32_t, std::uint32_t> ids;
            for (auto& actor : scene.actors)
            {
                ids[actor.id] = nextId;
                actor.id = nextId++;
                actors.push_back(std::move(actor));
            }
            auto remap = [&](std::uint32_t& id) { id = ids.contains(id) ? ids[id] : 0; };
            for (auto& actor : actors)
            {
                for (auto& event : actor.events)
                    remap(event.lookActor);
                remap(actor.attachment.actor);
                // (a prop of a scene from before its keys gets an id for them)
                if (actor.placedProp && !actor.placedProp->id)
                    actor.placedProp->id = PropLogic::MakeId(actors, *Props::GetKeyed());
            }
            selected.reset();
            selectedClip.reset();
            selectedFaceClip.reset();
            selectedEvent.reset();
            history.Clear();
            notes.clear();
            Changed();
            savedGeneration = generation;
        }
        // (each demo has its own: a scene without any has nothing chosen)
        const auto choices = PlayerLookLogic::ChoicesFromJson(scene.players);
        Loadouts::SetChoices(choices);
        PlayerAnimation::SetChoices(choices);
        savedPlayerGeneration = GetPlayerGeneration();
        // (scenes from before looks: their picked walks become looks, keeping their arrival; those from before deaths had
        // one rule: a death that follows the path falls where it is, not a change to undo)
        for (const auto& loaded : GetActors())
        {
            auto actor = loaded;
            const bool deaths = ActorLogic::TakeDeathsOffPath(actor, GetAnimationLookup());
            if (!ConvertOldWalks(actor) && !deaths)
                continue;
            std::lock_guard lock(mutex);
            if (const auto it = std::ranges::find(actors, actor.id, &Types::Actor::id); it != actors.end())
            {
                SortClips(actor);
                *it = std::move(actor);
                Changed();
            }
        }
        // with character zones, the actors' characters load one by one and the scene's level pack (of a scene made
        // before them) stays as it was, unloaded
        if (HasCharacterZones())
        {
            std::lock_guard lock(mutex);
            pack = scene.pack;
        }
        else
            SetPack(scene.pack);
    }

    void FitMeasuredWalks();
    void ReplanMeasuredPaths();

    void Initialize()
    {
        try
        {
            const std::string_view text(reinterpret_cast<const char*>(Resources::CHARACTERS_data),
                                        Resources::CHARACTERS_size);
            campaignCharacters = CharacterLogic::ParseCatalog(nlohmann::json::parse(text));
            campaignModelKinds = CharacterLogic::GetModelKinds(campaignCharacters.characters);
            zoneIndex = CharacterLogic::IndexZones(campaignCharacters.characters);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("The character catalog doesn't read: {}", e.what());
        }
        // the animation catalog, now rather than first in a demo's load
        GetAnimationCatalog();

        RescanCharacterZones();

        // a demo's actors stand in its map
        Events::RegisterListener(EventType::PreDemoLoad, [] {
            // the setup may have run meanwhile
            RescanCharacterZones();
            std::lock_guard lock(mutex);
            actors.clear();
            pack.clear();
            selected.reset();
            selectedClip.reset();
            selectedFaceClip.reset();
            selectedWaypoint.reset();
            selectedEvent.reset();
            placementRequests.clear();
            walkFits.clear();
            planWaits.clear();
            history.Clear();
            Changed();
            savedGeneration = generation;
            loadPending = false;
        });
        Events::RegisterListener(EventType::PostDemoLoad, [] { loadPending = true; });

        Events::RegisterListener(EventType::OnFrame, [] {
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            if (loadPending)
            {
                loadPending = false;
                Load();
                return;
            }
            FitMeasuredWalks();
            ReplanMeasuredPaths();
            if ((generation != savedGeneration &&
                 std::chrono::steady_clock::now() - changedAt > std::chrono::milliseconds(500)) ||
                GetPlayerGeneration() != savedPlayerGeneration)
            {
                savedGeneration = generation;
                savedPlayerGeneration = GetPlayerGeneration();
                Save();
            }
        });
    }

    void SaveIfChanged()
    {
        if ((generation == savedGeneration && GetPlayerGeneration() == savedPlayerGeneration) || loadPending ||
            Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            return;
        savedGeneration = generation;
        savedPlayerGeneration = GetPlayerGeneration();
        Save();
    }

    void SetCatalog(std::shared_ptr<const Types::ActorCatalog> newCatalog)
    {
        // with the killstreaks' motions, which their clips are of in place of animations (see KillstreakLogic)
        if (newCatalog && std::ranges::none_of(KillstreakLogic::MOTIONS, [&](std::string_view motion) {
                return newCatalog->FindAnimation(motion) != nullptr;
            }))
        {
            auto withMotions = std::make_shared<Types::ActorCatalog>(*newCatalog);
            for (const auto motion : KillstreakLogic::MOTIONS)
                withMotions->animations.push_back({.name = std::string(motion),
                                                   .duration = KillstreakLogic::MOTION_DURATION,
                                                   .loop = true});
            // (and their deaths, once)
            for (const auto motion : KillstreakLogic::DEATH_MOTIONS)
                withMotions->animations.push_back({.name = std::string(motion),
                                                   .duration = KillstreakLogic::GetDeathDuration(motion),
                                                   .loop = false});
            std::ranges::sort(withMotions->animations, {}, &Types::ActorCatalog::Animation::name);
            newCatalog = std::move(withMotions);
        }
        std::lock_guard lock(mutex);
        catalog = std::move(newCatalog);
    }

    std::shared_ptr<const Types::ActorCatalog> GetCatalog()
    {
        std::lock_guard lock(mutex);
        return catalog;
    }

    std::vector<Types::Actor> GetActors()
    {
        std::lock_guard lock(mutex);
        return actors;
    }

    std::optional<Types::Actor> GetActor(std::uint32_t id)
    {
        std::lock_guard lock(mutex);
        const auto it = std::ranges::find(actors, id, &Types::Actor::id);
        return it != actors.end() ? std::optional(*it) : std::nullopt;
    }

    std::string UniqueName(std::string base)
    {
        // without a number of its own, the first is "Actor 1"
        while (!base.empty() && (std::isdigit(static_cast<unsigned char>(base.back())) || base.back() == ' '))
            base.pop_back();
        if (base.empty())
            base = "Actor";
        for (std::uint32_t number = 1;; ++number)
        {
            auto name = std::format("{} {}", base, number);
            if (std::ranges::find(actors, name, &Types::Actor::name) == actors.end())
                return name;
        }
    }

    std::string MakeName(std::string base)
    {
        std::lock_guard lock(mutex);
        return UniqueName(std::move(base));
    }

    std::uint32_t Add(Types::Actor actor)
    {
        std::lock_guard lock(mutex);
        actor.id = nextId++;
        if (actor.name.empty())
            actor.name = UniqueName("Actor");
        if (actor.light && !actor.light->id)
            actor.light->id = LightLogic::MakeId(actors, *Lights::GetKeyed());
        if (actor.placedProp && !actor.placedProp->id)
            actor.placedProp->id = PropLogic::MakeId(actors, *Props::GetKeyed());
        SortClips(actor);
        Record("add " + actor.name);
        actors.push_back(std::move(actor));
        Changed();
        return actors.back().id;
    }

    std::uint32_t Duplicate(std::uint32_t id)
    {
        auto copy = [&]() -> std::optional<Types::Actor> {
            std::lock_guard lock(mutex);
            const auto it = std::ranges::find(actors, id, &Types::Actor::id);
            if (it == actors.end())
                return std::nullopt;
            auto copy = *it;
            copy.id = nextId++;
            copy.name = UniqueName(copy.name);
            // next to it, so both can be seen (a Mortar Team's points with it)
            copy.position += glm::vec3(0, 40, 0);
            if (copy.mortar)
                for (auto& point : copy.mortar->points)
                    point += glm::vec3(0, 40, 0);
            if (copy.light)
            {
                copy.light->id = LightLogic::MakeId(actors, *Lights::GetKeyed());
                UndoHistory::Get().BeginGroup("duplicate " + it->name);
            }
            if (copy.placedProp)
            {
                copy.placedProp->id = PropLogic::MakeId(actors, *Props::GetKeyed());
                UndoHistory::Get().BeginGroup("duplicate " + it->name);
            }
            Record("duplicate " + it->name);
            actors.insert(it + 1, copy);
            Changed();
            return copy;
        }();
        if (!copy)
            return 0;
        // (a light's keys, after the lock: keys can ask the actors for a value)
        if (copy->light)
        {
            if (const auto original = GetActor(id); original && original->light)
                Lights::CopyKeys(original->light->id, copy->light->id);
            UndoHistory::Get().EndGroup();
        }
        // (a prop's the same)
        if (copy->placedProp)
        {
            if (const auto original = GetActor(id); original && original->placedProp)
                Props::CopyKeys(original->placedProp->id, copy->placedProp->id);
            UndoHistory::Get().EndGroup();
        }
        return copy->id;
    }

    void Update(const Types::Actor& actor, bool sameStep, std::string_view label)
    {
        std::lock_guard lock(mutex);
        const auto it = std::ranges::find(actors, actor.id, &Types::Actor::id);
        auto sorted = actor;
        SortClips(sorted);
        if (it == actors.end() || *it == sorted)
            return;
        // (paths with gaits: one from before them walks as it did, fitted only as it was, until a gait is picked; and
        // after edits of its clips, its one-offs going on to be where their anchors are)
        if (sorted.movement == Types::ActorMovement::Path && !sorted.stretches.empty())
        {
            const auto gaits = [](const std::vector<Types::ActorStretch>& stretches) {
                std::vector<std::tuple<Types::ActorGait, bool, std::uint32_t>> found;
                for (const auto& stretch : stretches)
                    found.emplace_back(stretch.gait, stretch.crouched, stretch.wait);
                return found;
            };
            // (a stretch's own pace is a pace asked for, its join's blend a look's change)
            const auto paces = [](const std::vector<Types::ActorStretch>& stretches) {
                std::vector<float> found;
                for (const auto& stretch : stretches)
                    found.push_back(stretch.pace);
                return found;
            };
            const auto withoutPaces = [](std::vector<Types::ActorStretch> stretches) {
                for (auto& stretch : stretches)
                    stretch.pace = 1.0f;
                return stretches;
            };
            const bool path = it->path != sorted.path || gaits(it->stretches) != gaits(sorted.stretches) ||
                              it->stance != sorted.stance || it->movement != sorted.movement;
            const bool look = it->looks != sorted.looks ||
                              withoutPaces(it->stretches) != withoutPaces(sorted.stretches) ||
                              it->models != sorted.models || it->character != sorted.character ||
                              it->movementSet != sorted.movementSet;
            const bool pace = it->pace != sorted.pace || paces(it->stretches) != paces(sorted.stretches);
            const bool clips = it->clips != sorted.clips;
            // (a clip of the user's deleted or swapped for another animation: what it does to the arrival is said)
            const bool replaced = std::ranges::any_of(it->clips, [&](const Types::ActorClip& was) {
                const auto now = std::ranges::find(sorted.clips, was.id, &Types::ActorClip::id);
                return was.chain == Types::ActorChain::None &&
                       (now == sorted.clips.end() || now->animation != was.animation);
            });
            if (path || look || pace || clips)
            {
                auto& pending = pendingReplans[sorted.id];
                pending.keepTime = pending.keepTime || (look && !path && !pace);
                pending.noted = pending.noted || path || look || replaced;
                // (a one-off of the user's moved or added, its anchor as it was: anchored again where it is now; one
                // given an anchor, as a drop on one does or one made with one, keeps it)
                if (clips && !path && !look && !pace)
                    for (const auto& clip : sorted.clips)
                        if (clip.chain == Types::ActorChain::None)
                        {
                            const auto was = std::ranges::find(it->clips, clip.id, &Types::ActorClip::id);
                            const bool added = was == it->clips.end();
                            const bool anchored = added ? clip.anchor != Types::ActorAnchor::Time
                                                        : was->anchor != clip.anchor ||
                                                              was->anchorOffset != clip.anchorOffset ||
                                                              was->anchorPoint != clip.anchorPoint;
                            if (anchored)
                                pending.anchored.erase(clip.id);
                            else if (added || was->start != clip.start)
                                pending.anchored.insert(clip.id);
                        }
            }
        }
        // (a killstreak's flight fitted again as its path changes, as placing its points fits it, unsaid: its clips
        // are the user's to drag)
        else if (KillstreakLogic::HasFlights(sorted) && (it->path != sorted.path || it->movement != sorted.movement))
            pendingReplans[sorted.id];
        // (the user's change: what the notes said is the other choice to it)
        if (!sameStep)
            std::erase_if(notes, [&](const Note& note) { return note.actor == sorted.id; });
        Record(label.empty() ? "change " + it->name : std::string(label), sameStep);
        *it = std::move(sorted);
        Changed();
    }

    void Remove(std::uint32_t id)
    {
        std::optional<std::uint32_t> light, prop;
        {
            std::lock_guard lock(mutex);
            const auto it = std::ranges::find(actors, id, &Types::Actor::id);
            if (it == actors.end())
                return;
            if (it->light)
            {
                light = it->light->id;
                UndoHistory::Get().BeginGroup("delete " + it->name);
            }
            if (it->placedProp)
            {
                prop = it->placedProp->id;
                UndoHistory::Get().BeginGroup("delete " + it->name);
            }
            Record("delete " + it->name);
            std::erase_if(actors, [&](const Types::Actor& actor) { return actor.id == id; });
            if (selected == id)
            {
                selected.reset();
                selectedClip.reset();
                selectedFaceClip.reset();
                selectedEvent.reset();
            }
            Changed();
        }
        // (a light's keys go with it, after the lock: keys can ask the actors for a value)
        if (light)
        {
            Lights::ClearKeys(*light);
            UndoHistory::Get().EndGroup();
        }
        if (prop)
        {
            Props::ClearKeys(*prop);
            UndoHistory::Get().EndGroup();
        }
    }

    void Reorder(std::span<const std::uint32_t> order)
    {
        std::lock_guard lock(mutex);
        std::vector<Types::Actor> sorted;
        for (const auto id : order)
        {
            if (const auto it = std::ranges::find(actors, id, &Types::Actor::id); it != actors.end())
                sorted.push_back(*it);
        }
        if (sorted.size() != actors.size() ||
            std::ranges::equal(sorted, actors, {}, &Types::Actor::id, &Types::Actor::id))
            return;
        Record("move a row");
        actors = std::move(sorted);
        Changed();
    }

    void Clear()
    {
        std::vector<std::uint32_t> lights, props;
        {
            std::lock_guard lock(mutex);
            if (!actors.empty())
                Record("delete all actors");
            for (const auto& actor : actors)
            {
                if (actor.light)
                    lights.push_back(actor.light->id);
                if (actor.placedProp)
                    props.push_back(actor.placedProp->id);
            }
            actors.clear();
            selected.reset();
            selectedClip.reset();
            selectedFaceClip.reset();
            selectedEvent.reset();
            Changed();
        }
        for (const auto light : lights)
            Lights::ClearKeys(light);
        for (const auto prop : props)
            Props::ClearKeys(prop);
    }

    // back to (or forward to) other actors; the selection stays where it still can
    bool Restore(std::optional<std::vector<Types::Actor>> restored)
    {
        if (!restored)
            return false;
        actors = std::move(*restored);
        if (selected && std::ranges::find(actors, *selected, &Types::Actor::id) == actors.end())
        {
            selected.reset();
            selectedClip.reset();
            selectedFaceClip.reset();
            selectedEvent.reset();
            selectedWaypoint.reset();
        }
        Changed();
        return true;
    }

    bool Undo()
    {
        std::lock_guard lock(mutex);
        return Restore(history.Undo(actors));
    }

    bool Redo()
    {
        std::lock_guard lock(mutex);
        return Restore(history.Redo(actors));
    }

    bool CanUndo()
    {
        std::lock_guard lock(mutex);
        return history.CanUndo();
    }

    bool CanRedo()
    {
        std::lock_guard lock(mutex);
        return history.CanRedo();
    }

    void EndEditStep()
    {
        std::lock_guard lock(mutex);
        history.EndStep();
    }

    std::uint32_t GetGeneration()
    {
        return generation;
    }

    bool IsSceneLoadPending()
    {
        return loadPending;
    }

    std::optional<std::uint32_t> GetSelected()
    {
        std::lock_guard lock(mutex);
        return selected;
    }

    void Select(std::optional<std::uint32_t> id)
    {
        std::lock_guard lock(mutex);
        if (selected != id)
        {
            selectedClip.reset();
            selectedFaceClip.reset();
            selectedWaypoint.reset();
            selectedEvent.reset();
        }
        selected = id;
    }

    std::optional<std::uint32_t> GetSelectedClip()
    {
        std::lock_guard lock(mutex);
        return selectedClip;
    }

    void SelectClip(std::optional<std::uint32_t> clip)
    {
        std::lock_guard lock(mutex);
        selectedClip = clip;
        if (clip)
            selectedFaceClip.reset();
    }

    std::optional<std::uint32_t> GetSelectedFaceClip()
    {
        std::lock_guard lock(mutex);
        return selectedFaceClip;
    }

    void SelectFaceClip(std::optional<std::uint32_t> clip)
    {
        std::lock_guard lock(mutex);
        selectedFaceClip = clip;
        if (clip)
            selectedClip.reset();
    }

    std::uint32_t AddClip(std::uint32_t actorId, Types::ActorClip clip)
    {
        auto actor = GetActor(actorId);
        if (!actor)
            return 0;
        const auto id = ActorLogic::AddClip(*actor, std::move(clip));
        JoinAround(*actor, id);
        Update(*actor, false, "add clip to " + actor->name);
        return id;
    }

    bool JoinAround(Types::Actor& actor, std::uint32_t clipId)
    {
        if (!actor.autoTransitions)
            return false;
        const auto animations = GetAnimationLookup();
        // (a transition that turns turns it as the game's own turn)
        auto changed = false;
        if (const auto clip = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id); clip != actor.clips.end())
            changed = TransitionLogic::MarkTurn(*clip, animations);
        return TransitionLogic::JoinAround(actor, clipId, GetBody(actor), animations) || changed;
    }

    void SetAutoTransitions(Types::Actor& actor, bool on)
    {
        if (actor.autoTransitions == on)
            return;
        actor.autoTransitions = on;
        // the user's clips, in order (the ids stay as the transitions come and go)
        std::vector<std::uint32_t> clips;
        for (const auto& clip : actor.clips)
            if (clip.chain == Types::ActorChain::None)
                clips.push_back(clip.id);
        const auto body = GetBody(actor);
        const auto animations = GetAnimationLookup();
        for (const auto id : clips)
        {
            const auto clip = std::ranges::find(actor.clips, id, &Types::ActorClip::id);
            if (clip == actor.clips.end())
                continue;
            if (on)
            {
                TransitionLogic::MarkTurn(*clip, animations);
                TransitionLogic::JoinAround(actor, id, body, animations);
            }
            else
            {
                clip->turn.reset();
                TransitionLogic::Unjoin(actor, id);
            }
        }
        // and its path's
        if (!on)
            TransitionLogic::UnplanPath(actor);
        else
        {
            for (const auto walk : TransitionLogic::GetPathWalks(actor, animations))
                TransitionLogic::PlanWalk(actor, walk, body, animations);
            ReplanOnceMeasured(actor);
        }
    }

    std::uint32_t AddFaceClip(std::uint32_t actorId, Types::ActorClip clip)
    {
        std::lock_guard lock(mutex);
        const auto it = std::ranges::find(actors, actorId, &Types::Actor::id);
        if (it == actors.end())
            return 0;
        Record("add face clip to " + it->name);
        const auto id = ActorLogic::AddFaceClip(*it, std::move(clip));
        Changed();
        return id;
    }

    std::uint32_t AddEvent(std::uint32_t actorId, Types::ActorEvent event)
    {
        std::lock_guard lock(mutex);
        const auto it = std::ranges::find(actors, actorId, &Types::Actor::id);
        if (it == actors.end())
            return 0;
        Record("add event to " + it->name);
        const auto id = ActorLogic::AddEvent(*it, std::move(event));
        Changed();
        return id;
    }

    std::optional<std::uint32_t> GetSelectedEvent()
    {
        std::lock_guard lock(mutex);
        return selectedEvent;
    }

    void SelectEvent(std::optional<std::uint32_t> event)
    {
        std::lock_guard lock(mutex);
        selectedEvent = event;
    }

    bool IsVisible(const Types::Actor& actor, std::uint32_t tick)
    {
        return ActorLogic::IsVisible(actor, tick);
    }

    ActorLogic::LookState GetLook(const Types::Actor& actor, std::uint32_t tick)
    {
        return ActorLogic::GetLook(actor, tick);
    }

    std::vector<FiredEvent> GetFiredEvents(const Types::Actor& actor, std::uint32_t from, std::uint32_t to)
    {
        return ActorLogic::GetFiredEvents(actor, from, to);
    }

    std::string ResolveWeapon(const Types::Actor& actor)
    {
        if (actor.weapon.empty())
            return {};
        const auto loadouts = Loadouts::GetCatalog();
        if (!loadouts)
            return {};
        const auto variant = Loadouts::FindClosestVariant(*loadouts, actor.weapon, actor.attachments);
        return variant ? variant->name : std::string{};
    }

    Types::ActorEvent MakeGunFire(const Types::Actor& actor, std::uint32_t tick)
    {
        const auto loadouts = Loadouts::GetCatalog();
        const auto weapon = loadouts ? loadouts->FindWeapon(actor.weapon) : nullptr;
        return ActorLogic::MakeGunFire(weapon ? weapon->fireTime : 0, GetGunClass(actor.id), tick);
    }

    std::string ResolveStowedWeapon(const Types::Actor& actor)
    {
        if (actor.stowedWeapon.empty())
            return {};
        const auto loadouts = Loadouts::GetCatalog();
        const auto variant =
            loadouts ? Loadouts::FindClosestVariant(*loadouts, actor.stowedWeapon, actor.stowedAttachments) : nullptr;
        return variant ? variant->name : std::string{};
    }

    std::vector<Types::ActorAnimationSample> Evaluate(const Types::Actor& actor, std::uint32_t tick)
    {
        return ActorLogic::Evaluate(actor, tick);
    }

    // the motion of the animations the game has measured, by name; and how many it has measured (or measured
    // differently), for what is worked out from them once a change (see GetPathTiming)
    std::mutex motionMutex;
    std::unordered_map<std::string, glm::vec2> motions;
    std::atomic<std::uint32_t> measured = 0;

    void SetAnimationMotion(const std::string& animation, glm::vec2 motionPerPlay)
    {
        std::lock_guard lock(motionMutex);
        if (const auto it = motions.find(animation); it == motions.end() || it->second != motionPerPlay)
            ++measured;
        motions[animation] = motionPerPlay;
    }

    std::optional<glm::vec2> GetAnimationMotion(const std::string& animation)
    {
        std::lock_guard lock(motionMutex);
        const auto it = motions.find(animation);
        return it != motions.end() ? std::optional(it->second) : std::nullopt;
    }

    // how the animations the game has measured move the root, by name
    std::unordered_map<std::string, std::shared_ptr<const ActorLogic::RootMotion>> rootMotions;

    void SetAnimationRootMotion(const std::string& animation, std::shared_ptr<const ActorLogic::RootMotion> motion)
    {
        std::lock_guard lock(motionMutex);
        if (!rootMotions.contains(animation))
            ++measured;
        rootMotions[animation] = std::move(motion);
    }

    std::shared_ptr<const ActorLogic::RootMotion> GetAnimationRootMotion(const std::string& animation)
    {
        std::lock_guard lock(motionMutex);
        const auto it = rootMotions.find(animation);
        return it != rootMotions.end() ? it->second : nullptr;
    }

    // root motions guessed of transitions (ActorLogic::GuessRootMotion) by name, until the game measures them
    std::unordered_map<std::string, std::shared_ptr<const ActorLogic::RootMotion>> guessedRootMotions;

    std::shared_ptr<const ActorLogic::RootMotion> GetGuessedRootMotion(const std::string& animation,
                                                                       const Types::AnimationEntry& entry)
    {
        std::lock_guard lock(motionMutex);
        auto& guess = guessedRootMotions[animation];
        if (!guess)
            guess = std::make_shared<const ActorLogic::RootMotion>(
                ActorLogic::GuessRootMotion(glm::vec2(entry.motion), entry.turn));
        return guess;
    }

    // the catalog's durations and the measured motions, as the walked distance and root motion need them; what the
    // catalog of every animation says of them (a death, alert, its stance, whether it moves the root) where it has them
    ActorLogic::AnimationLookup GetAnimationLookup()
    {
        return [catalog = GetCatalog()](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            // (a killstreak's motion, which the game hasn't: see KillstreakLogic)
            if (KillstreakLogic::IsMotion(name))
                return KillstreakLogic::GetMotionInfo(name);
            const auto animation = catalog ? catalog->FindAnimation(name) : nullptr;
            if (!animation)
                return std::nullopt;
            ActorLogic::AnimationInfo info{animation->duration, GetAnimationMotion(name), animation->notetracks,
                                           animation->kind, animation->family};
            info.rootMotion = GetAnimationRootMotion(name);
            info.loop = animation->loop;
            if (const auto entry = animation->entry)
            {
                if (entry->delta)
                    info.catalogMotion = glm::vec2(entry->motion);
                info.death = entry->death;
                info.alert = entry->alert;
                info.stance = AnimationCatalogLogic::GetStance(entry->from.posture);
                info.delta = entry->delta;
                if (entry->from.posture != Types::AnimationPosture::Unknown ||
                    entry->from.movement != Types::AnimationMovement::Unknown)
                    info.from = entry->from;
                if (entry->to.posture != Types::AnimationPosture::Unknown ||
                    entry->to.movement != Types::AnimationMovement::Unknown)
                    info.to = entry->to;
                if (entry->turn != 0.0f)
                    info.turn = entry->turn;
                info.footfalls = entry->footfalls;
                // (a transition the game hasn't measured yet, as the catalog decoded its whole play: how a path's are
                // planned before they are clips the game measures, planned again once it has; see ReplanOnceMeasured)
                if (!info.rootMotion && entry->delta && info.from && info.to)
                    info.rootMotion = GetGuessedRootMotion(name, *entry);
            }
            return info;
        };
    }

    ActorLogic::Layers GetLayers(const Types::Actor& actor, std::uint32_t tick, const ActorLogic::Body& body)
    {
        return ActorLogic::GetLayers(actor, tick, GetAnimationLookup(), body);
    }

    std::vector<std::string> GetLegsLibrary(const Types::Actor& actor, const ActorLogic::Body& body)
    {
        return ActorLogic::GetLegsLibrary(actor, body, GetAnimationLookup());
    }

    std::vector<std::string> GetFaceLibrary(const Types::Actor& actor)
    {
        // (a dog's face isn't a person's, and a killstreak has none)
        if (GetActorKind(actor) != Types::ActorKind::Human)
            return {};
        return ActorLogic::GetFaceLibrary(actor, GetAnimationLookup());
    }

    std::mutex gunClassMutex;
    std::unordered_map<std::uint32_t, Types::GunClass> gunClasses;

    void SetGunClass(std::uint32_t id, Types::GunClass gun)
    {
        std::lock_guard lock(gunClassMutex);
        gunClasses[id] = gun;
    }

    Types::GunClass GetGunClass(std::uint32_t id)
    {
        std::lock_guard lock(gunClassMutex);
        const auto it = gunClasses.find(id);
        return it != gunClasses.end() ? it->second : Types::GunClass::Rifle;
    }

    // what the game had of the models it has had loaded, by name (a model's bones don't change as it comes and goes)
    struct KnownModel
    {
        std::vector<std::string> bones;
        bool dog = false;  // a dog's skeleton (see AnimationCatalogLogic::GetModelRig)
        // a campaign body's (j_spinelower hangs from j_mainroot, without MP's torso_stabilizer between them)
        bool campaign = false;
    };
    std::mutex knownModelsMutex;
    std::unordered_map<std::string, std::shared_ptr<const KnownModel>> knownModels;

    // none while the game hasn't had it loaded
    std::shared_ptr<const KnownModel> GetKnownModel(const std::string& model)
    {
        {
            std::lock_guard lock(knownModelsMutex);
            if (const auto known = knownModels.find(model); known != knownModels.end())
                return known->second;
        }
        auto bones = Mod::GetGameInterface()->GetModelBones(model);
        if (bones.empty())
            return nullptr;
        auto known = std::make_shared<KnownModel>();
        known->dog = AnimationCatalogLogic::GetModelRig(bones).dog;
        known->campaign = std::ranges::find(bones, "j_spinelower") != bones.end() &&
                          std::ranges::find(bones, "torso_stabilizer") == bones.end();
        known->bones = std::move(bones);
        std::lock_guard lock(knownModelsMutex);
        return knownModels.emplace(model, std::move(known)).first->second;
    }

    Types::ActorKind GetActorKind(const Types::Actor& actor)
    {
        if (actor.effect)
            return Types::ActorKind::Effect;
        if (actor.light)
            return Types::ActorKind::Light;
        if (actor.placedProp)
            return Types::ActorKind::Prop;
        if (actor.mortar)
            return Types::ActorKind::Mortar;
        if (actor.models.empty())
            return Types::ActorKind::Human;
        // (a killstreak by its model, which its bones don't tell)
        if (KillstreakLogic::FindByModels(actor.models))
            return Types::ActorKind::Vehicle;
        if (const auto body = GetKnownModel(actor.models.front()))
            return body->dog ? Types::ActorKind::Dog : Types::ActorKind::Human;
        const auto catalog = GetCatalog();
        if (const auto character = catalog ? GetCharacter(actor, *catalog) : nullptr)
            return CharacterLogic::GetActorKind(character->kind);
        return Types::ActorKind::Human;
    }

    float GetFlyHeight(const Types::Actor& actor)
    {
        const auto killstreak = KillstreakLogic::FindByModels(actor.models);
        return killstreak && KillstreakLogic::Flies(KillstreakLogic::Get(*killstreak).vehicleClass) ? actor.flyHeight
                                                                                                    : 0.0f;
    }

    std::optional<VehicleLogic::Body> GetVehicleBody(const Types::Actor& actor, std::uint32_t tick)
    {
        const auto killstreak = KillstreakLogic::FindByModels(actor.models);
        if (!killstreak)
            return std::nullopt;
        const auto lookup = GetAnimationLookup();
        return VehicleLogic::GetBody(
            KillstreakLogic::Get(*killstreak), tick,
            [&](std::uint32_t at) { return EvaluatePlacement(actor, at); },
            ActorLogic::GetPathProgress(actor, tick, lookup), actor.id);
    }

    // the breed of a dog of a character: Zombies' dogs are hellhounds
    ActorLogic::DogBreed GetBreed(const Types::Character* character, const Types::ActorCatalog& catalog)
    {
        if (character)
            if (const auto level =
                    std::ranges::find(catalog.levels, character->level, &Types::ActorCatalog::Pack::name);
                level != catalog.levels.end() && level->group == "Zombies")
                return ActorLogic::DogBreed::Hellhound;
        return ActorLogic::DogBreed::Shepherd;
    }

    // The movement set an actor walks and turns with: its own, or as its body was made for (Auto): the AI's low ready
    // on a campaign body, else the players'
    Types::ActorMovementSet GetMovementSet(const Types::Actor& actor)
    {
        if (actor.movementSet != Types::ActorMovementSet::Auto)
            return actor.movementSet;
        if (actor.models.empty())
            return Types::ActorMovementSet::Players;
        if (const auto body = GetKnownModel(actor.models.front()))
            return body->campaign ? Types::ActorMovementSet::LowReady : Types::ActorMovementSet::Players;
        // (as its character says while its body isn't loaded: the map's are multiplayer's)
        const auto catalog = GetCatalog();
        const auto character = catalog ? GetCharacter(actor, *catalog) : nullptr;
        return character && !character->id.starts_with("map/") ? Types::ActorMovementSet::LowReady
                                                                : Types::ActorMovementSet::Players;
    }

    ActorLogic::Body GetBody(const Types::Actor& actor, Types::GunClass gun)
    {
        if (const auto killstreak = KillstreakLogic::FindByModels(actor.models))
            return ActorLogic::Body::Vehicle(*killstreak);
        if (GetActorKind(actor) != Types::ActorKind::Dog)
        {
            ActorLogic::Body body(gun);
            body.set = GetMovementSet(actor);
            return body;
        }
        const auto catalog = GetCatalog();
        return ActorLogic::Body::Dog(catalog ? GetBreed(GetCharacter(actor, *catalog), *catalog)
                                             : ActorLogic::DogBreed::Shepherd);
    }

    ActorLogic::Body GetBody(const Types::Actor& actor)
    {
        return GetBody(actor, GetGunClass(actor.id));
    }

    ActorLogic::Body GetBody(const Types::Character& character, const Types::ActorCatalog& catalog)
    {
        if (const auto killstreak = KillstreakLogic::FindByModel(character.body))
            return ActorLogic::Body::Vehicle(*killstreak);
        if (CharacterLogic::GetActorKind(character.kind) == Types::ActorKind::Dog)
            return ActorLogic::Body::Dog(GetBreed(&character, catalog));
        return {};
    }

    std::vector<PassedNotetrack> GetPassedNotetracks(const Types::Actor& actor, std::uint32_t from, std::uint32_t to)
    {
        return ActorLogic::GetPassedNotetracks(actor, from, to, GetAnimationLookup());
    }

    std::vector<ActorLogic::Reload> GetReloads(const Types::Actor& actor, std::uint32_t until,
                                               const ActorLogic::ReloadMomentsLookup& moments)
    {
        return ActorLogic::GetReloads(actor, until, GetAnimationLookup(), moments);
    }

    ActorLogic::ClipWalk GetClipWalk(const Types::Actor& actor, const Types::ActorClip& clip)
    {
        return ActorLogic::GetClipWalk(actor, clip, GetAnimationLookup());
    }

    ActorLogic::RootMotionUse GetRootMotionUse(const Types::Actor& actor, const Types::ActorClip& clip)
    {
        return ActorLogic::GetRootMotionUse(actor, clip, GetAnimationLookup());
    }

    float GetPathLength(const Types::Actor& actor)
    {
        return ActorLogic::GetPathLength(actor, GetAnimationLookup());
    }

    std::vector<glm::vec3> GetPathLine(const Types::Actor& actor)
    {
        return ActorLogic::GetPathLine(ActorLogic::GetRoundedPath(actor, GetAnimationLookup()));
    }

    // where the game last drew attached actors, which only it can work out
    std::mutex drawnMutex;
    std::unordered_map<std::uint32_t, Types::ActorPlacement> drawnPlacements;

    void SetDrawnPlacement(std::uint32_t id, const Types::ActorPlacement& placement)
    {
        std::lock_guard lock(drawnMutex);
        drawnPlacements[id] = placement;
    }

    std::unordered_map<std::uint32_t, LeftOut> leftOuts;

    void SetLeftOut(std::uint32_t id, LeftOut leftOut)
    {
        std::lock_guard lock(drawnMutex);
        if (leftOut.models.empty() && !leftOut.gun && !leftOut.stowed)
            leftOuts.erase(id);
        else
            leftOuts[id] = std::move(leftOut);
    }

    std::optional<LeftOut> GetLeftOut(std::uint32_t id)
    {
        std::lock_guard lock(drawnMutex);
        const auto it = leftOuts.find(id);
        return it != leftOuts.end() ? std::optional(it->second) : std::nullopt;
    }

    Types::ActorPlacement EvaluatePlacement(const Types::Actor& actor, std::uint32_t tick)
    {
        // (a prop where its keys have it, which go by the timeline's tick)
        if (actor.placedProp)
        {
            const auto keyed = Props::WithKeyedPlacement(actor, Playback::ActorToTimelineTick(tick));
            return {keyed.position, keyed.yaw};
        }
        if (actor.movement == Types::ActorMovement::Attached)
        {
            std::lock_guard lock(drawnMutex);
            if (const auto it = drawnPlacements.find(actor.id); it != drawnPlacements.end())
                return it->second;
        }
        return ActorLogic::EvaluatePlacement(actor, tick, GetAnimationLookup());
    }

    void ApplyPlacement(Types::Actor& actor, std::uint32_t tick, glm::vec3 position, float yaw)
    {
        ActorLogic::ApplyPlacement(actor, tick, position, yaw, GetAnimationLookup());
    }

    std::optional<std::size_t> GetSelectedWaypoint()
    {
        std::lock_guard lock(mutex);
        return selectedWaypoint;
    }

    void SelectWaypoint(std::optional<std::size_t> waypoint)
    {
        std::lock_guard lock(mutex);
        selectedWaypoint = waypoint;
    }

    std::string_view GetAnimationCategory(const Types::ActorCatalog::Animation& animation)
    {
        return AnimationCatalogLogic::GetCategory(animation.name, animation.kind, animation.entry);
    }

    const std::vector<std::string_view>& GetAnimationCategories()
    {
        return AnimationCatalogLogic::GetCategories();
    }

    const Types::AnimationCatalog& GetAnimationCatalog()
    {
        // (read on first use, from any thread)
        static const Types::AnimationCatalog animations = [] {
            try
            {
                const auto start = std::chrono::steady_clock::now();
                const std::string_view text(reinterpret_cast<const char*>(Resources::ANIMATIONS_data),
                                            Resources::ANIMATIONS_size);
                auto read = AnimationCatalogLogic::Parse(nlohmann::json::parse(text));
                LOG_INFO("Read the catalog of {} animations in {} ms", read.entries.size(),
                         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                             .count());
                return read;
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("The animation catalog doesn't read: {}", e.what());
                return Types::AnimationCatalog{};
            }
        }();
        return animations;
    }

    std::mutex animationListMutex;
    std::shared_ptr<const AnimationList> animationList;

    std::shared_ptr<const AnimationList> GetAnimationList()
    {
        const auto game = GetCatalog();
        if (!game)
            return nullptr;
        std::lock_guard lock(animationListMutex);
        if (!animationList || animationList->game != game)
        {
            auto list = std::make_shared<AnimationList>();
            static_cast<AnimationCatalogLogic::AnimationList&>(*list) =
                AnimationCatalogLogic::List(GetAnimationCatalog(), *game);
            list->game = game;
            animationList = std::move(list);
        }
        return animationList;
    }

    // the fits of the models asked for, and their rig (the catalog of every animation doesn't change)
    struct Fitting
    {
        AnimationCatalogLogic::ModelRig rig;
        std::shared_ptr<const std::vector<bool>> fits;
    };
    std::mutex fitsMutex;
    std::map<std::vector<std::string>, std::shared_ptr<const Fitting>> fittings;
    // as many lists of models as a few scenes have
    constexpr std::size_t MAX_FITTINGS = 64;

    std::shared_ptr<const Fitting> GetFitting(const std::vector<std::string>& models)
    {
        {
            std::lock_guard lock(fitsMutex);
            if (const auto known = fittings.find(models); known != fittings.end())
                return known->second;
        }
        std::vector<std::string> bones;
        for (const auto& model : models)
        {
            const auto known = GetKnownModel(model);
            // a model the game hasn't loaded: no telling
            if (!known)
                return nullptr;
            bones.insert(bones.end(), known->bones.begin(), known->bones.end());
        }
        if (bones.empty())
            return nullptr;
        auto fitting = std::make_shared<Fitting>();
        fitting->rig = AnimationCatalogLogic::GetModelRig(bones);
        if (const auto killstreak = KillstreakLogic::FindByModels(models))
        {
            fitting->rig.vehicle = true;
            for (const auto animation : KillstreakLogic::Get(*killstreak).animations)
                fitting->rig.own.emplace_back(animation);
        }
        fitting->fits = std::make_shared<const std::vector<bool>>(
            AnimationCatalogLogic::GetFits(GetAnimationCatalog(), fitting->rig));
        std::lock_guard lock(fitsMutex);
        if (fittings.size() >= MAX_FITTINGS)
            fittings.clear();
        return fittings.emplace(models, std::move(fitting)).first->second;
    }

    std::shared_ptr<const std::vector<bool>> GetFits(const std::vector<std::string>& models)
    {
        const auto fitting = GetFitting(models);
        return fitting ? fitting->fits : nullptr;
    }

    std::optional<bool> Fits(const std::vector<std::string>& models, std::string_view animation)
    {
        // a killstreak's motions fit the killstreaks of their class alone
        if (KillstreakLogic::IsMotion(animation))
        {
            const auto killstreak = KillstreakLogic::FindByModels(models);
            return killstreak && KillstreakLogic::FitsMotion(KillstreakLogic::Get(*killstreak).vehicleClass, animation);
        }
        const auto& catalog = GetAnimationCatalog();
        const auto entry = catalog.Find(animation);
        if (!entry)
            return true;
        const auto fits = GetFits(models);
        const auto index = static_cast<std::size_t>(entry - catalog.entries.data());
        if (!fits || index >= fits->size())
            return std::nullopt;
        return (*fits)[index];
    }

    std::string DescribeMisfit(const std::vector<std::string>& models, std::string_view animation)
    {
        if (KillstreakLogic::IsMotion(animation))
        {
            const auto killstreak = KillstreakLogic::FindByModels(models);
            if (!killstreak)
                return "A killstreak's motion: the actor isn't a killstreak";
            const auto& info = KillstreakLogic::Get(*killstreak);
            return KillstreakLogic::FitsMotion(info.vehicleClass, animation)
                       ? std::string()
                       : std::format("Another kind of killstreak's motion: the {} doesn't {}", info.name,
                                     KillstreakLogic::GetMotionLabel(animation));
        }
        const auto& catalog = GetAnimationCatalog();
        const auto entry = catalog.Find(animation);
        const auto fitting = entry ? GetFitting(models) : nullptr;
        return fitting ? AnimationCatalogLogic::DescribeMisfit(catalog, *entry, fitting->rig) : std::string();
    }

    std::vector<TransitionLogic::Candidate> GetChainCandidates(const Types::Actor& actor, std::uint32_t clipId,
                                                              std::size_t most)
    {
        const auto clip = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id);
        if (clip == actor.clips.end())
            return {};
        // (with the fit filter on, those that fit its models, once that is known)
        const auto& catalog = GetAnimationCatalog();
        const auto fits = IsFitFilterOn() ? GetFits(actor.models) : nullptr;
        const auto fitting = [&](const Types::AnimationEntry& entry) {
            const auto index = static_cast<std::size_t>(&entry - catalog.entries.data());
            return !fits || index >= fits->size() || (*fits)[index];
        };
        return TransitionLogic::ChainCandidates(actor, static_cast<std::size_t>(clip - actor.clips.begin()),
                                                GetBody(actor), catalog, GetAnimationLookup(), fitting,
                                                PoseSignatures::GetLookup(actor), most);
    }

    std::uint32_t PlayAfter(Types::Actor& actor, std::uint32_t clipId, const std::string& animation)
    {
        return TransitionLogic::PlayAfter(actor, clipId, animation, GetBody(actor), GetAnimationLookup());
    }

    void ForgetModels()
    {
        {
            std::lock_guard lock(knownModelsMutex);
            knownModels.clear();
        }
        std::lock_guard lock(fitsMutex);
        fittings.clear();
    }

    bool AreModelsLoading(const std::vector<std::string>& models)
    {
        const auto catalog = GetCatalog();
        if (!catalog)
            return false;
        if (!catalog->loadingPack.empty())
            return true;
        for (const auto& model : models)
        {
            if (GetKnownModel(model))
                continue;
            // on its way from a character zone that hasn't failed
            const auto zones = zoneIndex.find(model);
            if (zones != zoneIndex.end() && std::ranges::any_of(zones->second, [&](const std::string& zone) {
                    return IsCharacterZoneAvailable(zone) && !catalog->zoneError.starts_with(zone + ":");
                }))
                return true;
        }
        return false;
    }

    std::atomic<bool> fitFilter = true;

    bool IsFitFilterOn()
    {
        return fitFilter;
    }

    void SetFitFilter(bool on)
    {
        fitFilter = on;
    }

    std::mutex storeMutex;
    std::shared_ptr<const StoreLoaded> storeLoaded;

    void SetStoreLoaded(std::shared_ptr<const StoreLoaded> loaded)
    {
        std::lock_guard lock(storeMutex);
        storeLoaded = std::move(loaded);
    }

    std::shared_ptr<const StoreLoaded> GetStoreLoaded()
    {
        std::lock_guard lock(storeMutex);
        return storeLoaded;
    }

    bool IsStoreLoaded(std::string_view animation)
    {
        const auto loaded = GetStoreLoaded();
        return loaded && loaded->contains(animation);
    }

    bool IsAnimationLoaded(const Types::ActorCatalog::Animation& animation, const StoreLoaded* loaded)
    {
        if (!animation.stored)
            return true;
        return loaded ? loaded->contains(animation.name) : IsStoreLoaded(animation.name);
    }

    bool IsFavoriteAnimation(std::string_view name)
    {
        const auto& favorites = PreferencesConfiguration::Get().favoriteActorAnimations;
        return std::ranges::find(favorites, name) != favorites.end();
    }

    void SetFavoriteAnimation(std::string_view name, bool favorite)
    {
        auto& favorites = PreferencesConfiguration::Get().favoriteActorAnimations;
        const auto it = std::ranges::find(favorites, name);
        if (favorite == (it != favorites.end()))
            return;
        if (favorite)
            favorites.emplace_back(name);
        else
            favorites.erase(it);
        Configuration::Get().Write(true);
    }

    void RequestPlacement(std::uint32_t id, glm::vec3 origin, glm::vec3 direction, std::optional<std::size_t> waypoint,
                          std::optional<float> yaw, std::optional<std::uint32_t> at)
    {
        const auto tick = at.value_or(Components::Playback::GetActorTick());
        std::lock_guard lock(mutex);
        std::erase_if(placementRequests, [&](const PlacementRequest& request) {
            return request.id == id && request.waypoint == waypoint;
        });
        const auto actor = std::ranges::find(actors, id, &Types::Actor::id);
        placementRequests.push_back({.id = id,
                                     .origin = origin,
                                     .direction = direction,
                                     .tick = tick,
                                     .waypoint = waypoint,
                                     .yaw = yaw,
                                     .surface = actor != actors.end() &&
                                                (actor->effect || actor->light || actor->placedProp || actor->mortar)});
    }

    void RequestAim(std::uint32_t id, glm::vec3 origin, glm::vec3 direction)
    {
        std::lock_guard lock(mutex);
        placementRequests.push_back({.id = id,
                                     .origin = origin,
                                     .direction = direction,
                                     .tick = Playback::GetActorTick(),
                                     .surface = true,
                                     .aim = true});
    }

    std::vector<PlacementRequest> TakePlacementRequests()
    {
        std::lock_guard lock(mutex);
        return std::exchange(placementRequests, {});
    }

    void RequestWalkTo(std::uint32_t id, glm::vec3 origin, glm::vec3 direction, bool run, std::optional<float> face,
                       std::optional<Types::ActorGait> gait, bool crouched)
    {
        const auto tick = Components::Playback::GetActorTick();
        std::lock_guard lock(mutex);
        placementRequests.push_back({.id = id,
                                     .origin = origin,
                                     .direction = direction,
                                     .tick = tick,
                                     .yaw = face,
                                     .walk = true,
                                     .run = run,
                                     .gait = gait,
                                     .crouched = crouched});
    }

    // whether the game has an animation loaded itself (not one the animation store would load from the pack): what
    // actors start with and walk with by default, as before the store, so they don't change with it
    bool HasOwnAnimation(const Types::ActorCatalog& catalog, std::string_view name)
    {
        const auto animation = catalog.FindAnimation(name);
        return animation && !animation->stored;
    }

    // whether the game has an animation, or the animation store would load it from the pack: what gaits go with (the
    // AI's sets' are the pack's on a multiplayer map)
    std::function<bool(std::string_view)> GetAnyAnimation()
    {
        return [catalog = GetCatalog()](std::string_view name) { return catalog && catalog->FindAnimation(name); };
    }

    // what walks an actor to a point: the first walk the game has (else run), and the idle it stands with there; with
    // locomotion the players' animations of its gun and stance, which stand for it, the walk the one of its speed
    struct Walking
    {
        std::string walk, rest;
        float guessedSpeed = ActorLogic::WALK_SPEED_GUESS;
        std::optional<float> speed;  // the walk's own, running with locomotion
    };
    // (with locomotion running, at its body's run; at a gait, that gait's animation, at its speed)
    Walking FindWalking(const Types::Actor& actor, bool run = false, const WalkOptions* options = nullptr)
    {
        Walking walking;
        const auto catalog = GetCatalog();
        if (!catalog)
            return walking;
        const auto has = [&](std::string_view name) { return HasOwnAnimation(*catalog, name); };
        using ActorLogic::Gait, ActorLogic::Heading;
        const auto body = GetBody(actor);
        if (options && options->gait && body.kind != Types::ActorKind::Vehicle &&
            actor.locomotion == Types::ActorLocomotion::Auto)
        {
            const auto any = GetAnyAnimation();
            walking.walk = ActorLogic::GetGaitAnimation(body, *options->gait, options->crouched, any);
            const auto stance = options->crouched && body.kind != Types::ActorKind::Dog ? Types::ActorStance::Crouch
                                                                                        : Types::ActorStance::Stand;
            walking.rest = ActorLogic::FindLocomotionAnimation(body, stance, Gait::Idle, Heading::Forward, any);
            walking.guessedSpeed = ActorLogic::GetAnimationSpeed(walking.walk, GetAnimationLookup());
            if (walking.guessedSpeed <= 0.0f)
                walking.guessedSpeed = ActorLogic::WALK_SPEED_GUESS;
            if (!walking.walk.empty() && !walking.rest.empty())
                return walking;
        }
        // running with locomotion: as fast as its body's run
        auto speed = actor.pathSpeed;
        if (run && actor.locomotion == Types::ActorLocomotion::Auto)
            for (const auto& preset : ActorLogic::GetSpeedPresets(body))
                if (preset.gait == Gait::Run && preset.stance == Types::ActorStance::Stand)
                    walking.speed = speed = preset.speed;
        if (body.kind == Types::ActorKind::Vehicle)
        {
            // its motions, at its speed
            const auto& info = KillstreakLogic::Get(body.killstreak);
            walking.walk = KillstreakLogic::GetMotion(info.vehicleClass, Gait::Run);
            walking.rest = KillstreakLogic::GetMotion(info.vehicleClass, Gait::Idle);
            if (!KillstreakLogic::IsMoving(walking.walk))
                walking.walk.clear();
            walking.guessedSpeed = speed;
            return walking;
        }
        if (body.kind == Types::ActorKind::Dog)
        {
            // its breed's (the animation pack's too): the walk of its speed with locomotion, else its walk
            const auto animations = GetAnimationLookup();
            const auto any = [&](std::string_view name) { return catalog->FindAnimation(name) != nullptr; };
            const auto walk =
                ActorLogic::FindLocomotionAnimation(body, Types::ActorStance::Stand, Gait::Walk, Heading::Forward, any);
            walking.walk = actor.locomotion == Types::ActorLocomotion::Auto
                               ? ActorLogic::ChooseLocomotion(body, Types::ActorStance::Stand,
                                                              std::max(speed, ActorLogic::IDLE_SPEED), 0.0f, animations)
                                     .animation
                               : walk;
            walking.rest =
                ActorLogic::FindLocomotionAnimation(body, Types::ActorStance::Stand, Gait::Idle, Heading::Forward, any);
            walking.guessedSpeed = actor.locomotion == Types::ActorLocomotion::Auto
                                       ? speed
                                       : ActorLogic::GetAnimationSpeed(walking.walk, animations);
            return walking;
        }
        if (actor.locomotion == Types::ActorLocomotion::Auto)
        {
            const auto stance = actor.stance == Types::ActorStance::Auto ? Types::ActorStance::Stand : actor.stance;
            const auto choice = ActorLogic::ChooseLocomotion(body, stance, std::max(speed, ActorLogic::IDLE_SPEED), 0.0f,
                                                             GetAnimationLookup());
            // (of an AI's set, the AI's own of the pack too)
            const auto any = [&](std::string_view name) { return catalog->FindAnimation(name) != nullptr; };
            walking.walk = choice.animation;
            walking.rest = ActorLogic::FindLocomotionAnimation(
                body, stance, Gait::Idle, Heading::Forward,
                body.set == Types::ActorMovementSet::Players ? std::function<bool(std::string_view)>(has)
                                                             : std::function<bool(std::string_view)>(any));
            walking.guessedSpeed = speed;
            if (!walking.walk.empty() && !walking.rest.empty())
                return walking;
            walking.speed.reset();
        }
        walking.walk = ActorLogic::FindFirst(ActorLogic::WALK_ANIMATIONS, has);
        if (walking.walk.empty())
        {
            walking.walk = ActorLogic::FindFirst(ActorLogic::RUN_ANIMATIONS, has);
            walking.guessedSpeed = ActorLogic::RUN_SPEED_GUESS;
        }
        walking.rest = ActorLogic::FindFirst(ActorLogic::IDLE_ANIMATIONS, has);
        return walking;
    }

    std::uint32_t WalkTo(Types::Actor& actor, std::uint32_t tick, glm::vec3 target, float* guessedSpeed,
                         const WalkOptions& options)
    {
        const auto walking = FindWalking(actor, options.run, &options);
        if (guessedSpeed)
            *guessedSpeed = walking.guessedSpeed;
        if (walking.walk.empty())
            return 0;
        // (the walk it is on at the tick as it was before it was planned: cut there to walk on elsewhere)
        const auto animations = GetAnimationLookup();
        for (const auto walk : TransitionLogic::GetPathWalks(actor, animations))
            if (const auto [from, to] = TransitionLogic::GetWalkSpan(actor, walk); from <= tick && tick < to)
                TransitionLogic::UnplanWalk(actor, walk);
        // (a path started at a gait goes at the gait's own speed; one from before gaits keeps its speed as a pace)
        const bool fresh =
            actor.stretches.empty() && (actor.movement != Types::ActorMovement::Path || actor.path.size() < 2);
        auto clip = ActorLogic::WalkTo(actor, tick, target, walking.walk, walking.rest, animations,
                                       walking.guessedSpeed);
        // at a gait: the stretch to the point goes at it (the path before it as it went)
        if (clip != 0 && options.gait && actor.movement == Types::ActorMovement::Path)
        {
            const auto body = GetBody(actor);
            const auto any = GetAnyAnimation();
            if (ActorLogic::MakeStretches(actor, body, animations, any))
            {
                if (fresh)
                    actor.pace = 1.0f;
                const auto from = actor.path.size() >= 2 ? actor.path.size() - 2 : 0;
                ActorLogic::SetGait(actor, from, actor.path.size(), *options.gait, options.crouched, body, animations,
                                    any);
                ActorLogic::FitWalk(actor, clip, animations, walking.guessedSpeed);
            }
        }
        // (a killstreak gets up to speed and stops at the game's rate: ActorLogic::FitWalk sets the blends it does that
        // over)
        if (clip != 0 && walking.speed)
        {
            // running: as fast as its run goes, whatever its speed
            const auto walk = std::ranges::find(actor.clips, clip, &Types::ActorClip::id);
            if (walk != actor.clips.end())
                walk->walkSpeed = *walking.speed;
            ActorLogic::FitWalk(actor, clip, animations, *walking.speed);
        }
        if (clip != 0 && actor.movement == Types::ActorMovement::Path)
        {
            // stopping facing the way given, from the way it went
            std::optional<float> stopTurn;
            if (options.face && actor.path.size() >= 2)
            {
                const auto way = glm::vec2(actor.path.back() - actor.path[actor.path.size() - 2]);
                if (glm::length(way) > 0.01f)
                    stopTurn = std::remainder(*options.face - glm::degrees(std::atan2(way.y, way.x)), 360.0f);
            }
            // the new walk planned, and one cut short to walk on (planned again as it goes on, without its stop)
            const auto body = GetBody(actor);
            for (const auto walk : TransitionLogic::GetPathWalks(actor, animations))
                if (walk == clip)
                    TransitionLogic::PlanWalk(actor, walk, body, animations, stopTurn);
                else if (!TransitionLogic::IsPlanned(actor, walk))
                    TransitionLogic::PlanWalk(actor, walk, body, animations);
            ReplanOnceMeasured(actor);
        }
        // fitted again once the game has measured how fast the walk goes (with locomotion, it goes at the actor's
        // speed)
        if (clip != 0 && actor.locomotion != Types::ActorLocomotion::Auto && !GetAnimationMotion(walking.walk))
        {
            std::lock_guard lock(mutex);
            walkFits.push_back({actor.id, clip, walking.walk, walking.guessedSpeed, false, false});
        }
        return clip;
    }

    void Place(const PlacementRequest& request, glm::vec3 ground, glm::vec3 normal)
    {
        auto actor = GetActor(request.id);
        if (!actor)
            return;
        // an effect on the surface, facing out of it (a unit off it, as ActorLogic::MakeEffect puts it)
        if (actor->effect && request.surface)
        {
            const auto placed = ActorLogic::MakeEffect(actor->effect->name, {}, ground, normal, 0);
            actor->position = placed.position;
            actor->yaw = placed.yaw;
            actor->effect->pitch = placed.effect->pitch;
            actor->effect->roll = 0.0f;
            Update(*actor, true);
            return;
        }
        // the Mortar Team's points: one on the ground there; as it is added, the
        // three about where it was put, the sides each then put on the ground below them
        if (actor->mortar && request.surface)
        {
            auto& points = actor->mortar->points;
            if (request.waypoint && *request.waypoint < points.size())
                points[*request.waypoint] = ground;
            else
            {
                points = KillstreakLogic::MakeMortar(ground, actor->yaw, 0).mortar->points;
                for (std::size_t i = 0; i < points.size(); ++i)
                    if (points[i] != ground)
                        RequestPlacement(actor->id, points[i] + glm::vec3(0.0f, 0.0f, 64.0f),
                                         glm::vec3(0.0f, 0.0f, -1.0f), i);
            }
            actor->position = points.empty() ? ground : points[points.size() / 2];
            Update(*actor, true);
            return;
        }
        // a prop standing upright on the surface, facing the camera when put where it looks, from where its
        // keys have it; what has keys is keyed at the playhead, as a light's
        if (actor->placedProp && request.surface)
        {
            auto placed = Props::WithKeyedPlacement(*actor, Playback::GetTimelineTick());
            placed.position = ground;
            if (request.yaw)
                placed.yaw = *request.yaw;
            if (Props::MoveTo(*actor, placed, "move " + actor->name))
                Update(*actor, true);
            return;
        }
        // a light on the surface, from where its keys have it; what has keys is keyed at
        // the playhead (3.13)
        if (actor->light && request.surface)
        {
            auto placed = Lights::WithKeyedPlacement(*actor, Playback::GetTimelineTick());
            if (request.aim)
                LightLogic::AimAt(placed, ground);
            else
                LightLogic::PlaceOnSurface(placed, ground, normal, request.origin);
            if (Lights::MoveTo(*actor, placed, std::format("{} {}", request.aim ? "aim" : "move", actor->name)))
                Update(*actor, true);
            return;
        }
        // (a flying killstreak's points as high above the ground as it flies)
        ground.z += GetFlyHeight(*actor);
        const auto animations = GetAnimationLookup();
        if (request.walk)
        {
            if (WalkTo(*actor, request.tick, ground, nullptr,
                       {.run = request.run, .face = request.yaw, .gait = request.gait, .crouched = request.crouched}) !=
                0)
                Update(*actor, false, std::format("{} {} to a point", request.run ? "run" : "walk", actor->name));
            return;
        }

        if (request.waypoint)
        {
            ActorLogic::MoveWaypoint(*actor, *request.waypoint, ground);
        }
        else
        {
            const auto yaw = request.yaw.value_or(EvaluatePlacement(*actor, request.tick).yaw);
            ApplyPlacement(*actor, request.tick, ground, yaw);
            // (on a path, the way it faces before it walks: the way its start turns it from)
            if (request.yaw && actor->movement == Types::ActorMovement::Path)
                actor->yaw = *request.yaw;
        }
        // the walk along a path being placed goes as far as it now does
        std::optional<WalkFit> fit;
        {
            std::lock_guard lock(mutex);
            const auto found = std::ranges::find_if(
                walkFits, [&](const WalkFit& each) { return each.id == request.id && each.placing; });
            if (found != walkFits.end())
                fit = *found;
        }
        if (fit)
            FitWalk(*actor, fit->clip, fit->guessedSpeed);
        // undone with the change that asked for it
        Update(*actor, true);
    }

    void FitWalkWhilePlacing(std::uint32_t id, std::uint32_t clip, float guessedSpeed)
    {
        const auto actor = GetActor(id);
        if (!actor)
            return;
        const auto found = std::ranges::find(actor->clips, clip, &Types::ActorClip::id);
        if (found == actor->clips.end())
            return;
        std::lock_guard lock(mutex);
        std::erase_if(walkFits, [](const WalkFit& fit) { return fit.placing; });
        walkFits.push_back({id, clip, found->animation, guessedSpeed, true, false});
    }

    namespace
    {
        // FitOnlyWalk's fit and plan, the one-offs taken out
        void FitAndPlanWalk(Types::Actor& actor, const ActorLogic::AnimationLookup& animations)
        {
            const auto walks = TransitionLogic::GetPathWalks(actor, animations);
            if (walks.size() != 1 || !actor.autoTransitions)
            {
                // (none planned without Auto transitions or Locomotion Auto; with walks of their own, as they are)
                if (!actor.autoTransitions || actor.locomotion != Types::ActorLocomotion::Auto)
                    TransitionLogic::UnplanPath(actor);
                ActorLogic::FitOnlyWalk(actor, animations);
                return;
            }
            const auto stopTurn = TransitionLogic::GetStopTurn(actor, walks.front());
            TransitionLogic::UnplanWalk(actor, walks.front());
            ActorLogic::FitOnlyWalk(actor, animations);
            TransitionLogic::PlanWalk(actor, walks.front(), GetBody(actor), animations, stopTurn);
            ReplanOnceMeasured(actor);
        }
    }  // namespace

    void AnchorDeath(Types::Actor& actor, std::uint32_t clipId)
    {
        const auto clip = std::ranges::find(actor.clips, clipId, &Types::ActorClip::id);
        if (clip == actor.clips.end() || actor.movement != Types::ActorMovement::Path || actor.path.size() < 2)
            return;
        // (the timing as FitOnlyWalk will have it: the walk fitted and planned with the one-offs out)
        const auto animations = GetAnimationLookup();
        auto planned = actor;
        ActorLogic::TakeOutOneOffs(planned, animations);
        FitAndPlanWalk(planned, animations);
        ActorLogic::AnchorMadeDeath(actor, clipId, ActorLogic::GetPathTiming(planned, animations), animations);
    }

    void FitOnlyWalk(Types::Actor& actor, const std::set<std::uint32_t>& anchored)
    {
        const auto animations = GetAnimationLookup();
        auto oneOffs = ActorLogic::TakeOutOneOffs(actor, animations);
        FitAndPlanWalk(actor, animations);
        if (!anchored.empty())
        {
            const auto timing = ActorLogic::GetPathTiming(actor, animations);
            for (auto& oneOff : oneOffs)
                if (anchored.contains(oneOff.clip.id))
                    ActorLogic::Anchor(oneOff.clip, timing, oneOff.death);
        }
        // (those that were joined to the clips around them joined again where they are)
        for (const auto id : ActorLogic::PutBackOneOffs(actor, std::move(oneOffs), animations))
            JoinAround(actor, id);
    }

    void FitWalk(Types::Actor& actor, std::uint32_t walkClip, float guessedSpeed)
    {
        const auto animations = GetAnimationLookup();
        const auto planned = std::ranges::any_of(actor.clips, [](const Types::ActorClip& clip) {
            return clip.chain != Types::ActorChain::None && clip.chain != Types::ActorChain::Join;
        });
        const auto stopTurn = TransitionLogic::GetStopTurn(actor, walkClip);
        if (planned)
            TransitionLogic::UnplanWalk(actor, walkClip);
        ActorLogic::FitWalk(actor, walkClip, animations, guessedSpeed);
        TransitionLogic::PlanWalk(actor, walkClip, GetBody(actor), animations, stopTurn);
        ReplanOnceMeasured(actor);
    }

    void SetMovementSet(Types::Actor& actor, Types::ActorMovementSet set)
    {
        const auto from = GetBody(actor);
        actor.movementSet = set;
        const auto to = GetBody(actor);
        if (from == to)
            return;
        actor.movementSet = from.set;
        Rechain(actor, [&] {
            actor.movementSet = set;
            // (its gaits as the new set goes them)
            ActorLogic::ResolveStretches(actor, GetBody(actor), GetAnyAnimation());
            // a speed preset: the new set's of the gait
            std::optional<ActorLogic::SpeedPreset> preset;
            for (const auto& each : ActorLogic::GetSpeedPresets(from))
                if (std::abs(actor.pathSpeed - each.speed) < 0.5f)
                    preset = each;
            if (preset)
                for (const auto& each : ActorLogic::GetSpeedPresets(to))
                    if (each.gait == preset->gait && each.stance == preset->stance)
                        actor.pathSpeed = each.speed;
            ActorLogic::FitOnlyWalk(actor, GetAnimationLookup());
        });
    }

    void Rechain(Types::Actor& actor, const std::function<void()>& change)
    {
        const auto on = actor.autoTransitions;
        if (on)
            SetAutoTransitions(actor, false);
        change();
        if (on)
            SetAutoTransitions(actor, true);
    }

    void StopFittingWalk()
    {
        std::lock_guard lock(mutex);
        std::erase_if(walkFits, [](const WalkFit& fit) { return fit.placing; });
    }

    std::optional<std::uint32_t> GetWalkFittedActor()
    {
        std::lock_guard lock(mutex);
        const auto found = std::ranges::find_if(walkFits, [](const WalkFit& fit) { return fit.placing; });
        return found != walkFits.end() ? std::optional(found->id) : std::nullopt;
    }

    void ReplanOnceMeasured(const Types::Actor& actor)
    {
        std::vector<std::string> unmeasured;
        for (const auto& clip : actor.clips)
            if (clip.chain != Types::ActorChain::None && clip.chain != Types::ActorChain::Join &&
                !GetAnimationMotion(clip.animation) && std::ranges::find(unmeasured, clip.animation) == unmeasured.end())
                unmeasured.push_back(clip.animation);
        // (and the looks of its stretches, which it goes as fast as)
        for (const auto& stretch : actor.stretches)
            if (!stretch.look.empty() || std::ranges::any_of(actor.looks, [&](const Types::ActorLook& look) {
                    return look.animation == stretch.animation;
                }))
                if (!GetAnimationMotion(stretch.animation) &&
                    std::ranges::find(unmeasured, stretch.animation) == unmeasured.end())
                    unmeasured.push_back(stretch.animation);
        std::lock_guard lock(mutex);
        std::erase_if(planWaits, [&](const PlanWait& wait) { return wait.id == actor.id; });
        if (!unmeasured.empty())
            planWaits.push_back({actor.id, std::move(unmeasured)});
    }

    // paths planned before the game measured their transitions are fitted and planned again once it has (as the start,
    // turns and stop move the actor by their root motion, which the plan fits)
    void ReplanMeasuredPaths()
    {
        std::vector<PlanWait> waits;
        {
            std::lock_guard lock(mutex);
            waits = planWaits;
        }
        for (const auto& wait : waits)
        {
            if (!std::ranges::all_of(wait.animations,
                                     [](const std::string& animation) { return GetAnimationMotion(animation).has_value(); }))
                continue;
            {
                std::lock_guard lock(mutex);
                std::erase_if(planWaits, [&](const PlanWait& each) { return each.id == wait.id; });
            }
            auto actor = GetActor(wait.id);
            if (!actor || !actor->autoTransitions)
                continue;
            // (its one-offs out meanwhile, put back where their anchors have them)
            const auto animations = GetAnimationLookup();
            auto oneOffs = ActorLogic::TakeOutOneOffs(*actor, animations);
            for (const auto walk : TransitionLogic::GetPathWalks(*actor, animations))
            {
                const auto clip = std::ranges::find(actor->clips, walk, &Types::ActorClip::id);
                FitWalk(*actor, walk, clip->walkSpeed > 0.0f ? clip->walkSpeed : actor->pathSpeed);
            }
            for (const auto id : ActorLogic::PutBackOneOffs(*actor, std::move(oneOffs), animations))
                JoinAround(*actor, id);
            // undone with the change that planned it
            Update(*actor, true);
        }
    }

    // walks fitted at a guessed speed are fitted again once the game has measured their animations
    void FitMeasuredWalks()
    {
        std::vector<WalkFit> fits;
        {
            std::lock_guard lock(mutex);
            fits = walkFits;
        }
        for (const auto& fit : fits)
        {
            if (fit.measured || !GetAnimationMotion(fit.animation))
                continue;
            if (auto actor = GetActor(fit.id))
            {
                FitWalk(*actor, fit.clip, fit.guessedSpeed);
                Update(*actor, true);
            }
            std::lock_guard lock(mutex);
            for (auto& each : walkFits)
                if (each.id == fit.id && each.clip == fit.clip)
                    each.measured = true;
            std::erase_if(walkFits, [](const WalkFit& each) { return each.measured && !each.placing; });
        }
    }

    std::string GetDeathAnimation(const ActorLogic::Body& body, const std::vector<std::string>* models);

    ActorLogic::StartAnimations GetStartAnimations(const ActorLogic::Body& body)
    {
        ActorLogic::StartAnimations animations;
        const auto catalog = GetCatalog();
        if (!catalog)
            return animations;
        // a killstreak's: its motions, at its speeds
        if (body.kind == Types::ActorKind::Vehicle)
        {
            const auto& info = KillstreakLogic::Get(body.killstreak);
            animations.idle = KillstreakLogic::GetMotion(info.vehicleClass, ActorLogic::Gait::Idle);
            animations.walk = animations.run = KillstreakLogic::GetMotion(info.vehicleClass, ActorLogic::Gait::Run);
            animations.locomotion = true;
            animations.walkSpeed = animations.runSpeed = KillstreakLogic::GetCruiseSpeed(info);
            for (const auto& preset : info.presets)
                if (preset.gait == ActorLogic::Gait::Walk)
                    animations.walkSpeed = preset.speed;
            return animations;
        }
        // a dog's: its breed's (the animation pack's too, as the hellhound's are only there)
        if (body.kind == Types::ActorKind::Dog)
        {
            animations = ActorLogic::GetDogStartAnimations(body.breed, GetAnimationLookup());
            if (auto death = GetDeathAnimation(body, nullptr); !death.empty())
                if (const auto found = catalog->FindAnimation(death))
                {
                    animations.death = std::move(death);
                    animations.deathDuration = found->duration;
                }
            return animations;
        }
        const auto has = [&](std::string_view name) { return HasOwnAnimation(*catalog, name); };
        animations.idle = ActorLogic::FindFirst(ActorLogic::IDLE_ANIMATIONS, has);
        animations.walk = ActorLogic::FindFirst(ActorLogic::WALK_ANIMATIONS, has);
        animations.run = ActorLogic::FindFirst(ActorLogic::RUN_ANIMATIONS, has);
        // with the players' animations, walking and running are their locomotion's (see ActorLogic::GetLayers)
        using ActorLogic::Gait, ActorLogic::Heading;
        const auto walk =
            ActorLogic::FindLocomotionAnimation(body, Types::ActorStance::Stand, Gait::Walk, Heading::Forward, has);
        const auto run =
            ActorLogic::FindLocomotionAnimation(body, Types::ActorStance::Stand, Gait::Run, Heading::Forward, has);
        if (has(ActorLogic::GetStanceIdle(Types::GunClass::Rifle, Types::ActorStance::Stand)) && !walk.empty() &&
            !run.empty())
        {
            animations.locomotion = true;
            animations.walk = walk;
            animations.run = run;
        }
        // with the players' faces, faces of their own
        animations.face = std::ranges::all_of(ActorLogic::FACE_ANIMATIONS, has);
        animations.shoot = ActorLogic::FindFirst(ActorLogic::SHOOT_ANIMATIONS, has);
        animations.death = GetDeathAnimation(body, nullptr);
        if (const auto death = catalog->FindAnimation(animations.death))
            animations.deathDuration = death->duration;
        return animations;
    }

    std::string GetStartWeapon()
    {
        const auto loadouts = Loadouts::GetCatalog();
        if (!loadouts)
            return {};
        constexpr std::array RIFLES{"ak47_mp", "m16_mp", "famas_mp", "galil_mp", "aug_mp", "fnfal_mp", "commando_mp"};
        for (const auto rifle : RIFLES)
            if (loadouts->FindWeapon(rifle))
                return rifle;
        const auto primary = std::ranges::find_if(loadouts->weapons, &Types::LoadoutCatalog::Weapon::primary);
        return primary != loadouts->weapons.end() ? primary->name : std::string();
    }

    // whether an animation is made for a body of a kind: a dog's, or a person's body (not a face's alone)
    bool IsMadeFor(const Types::ActorCatalog::Animation& animation, Types::ActorKind kind)
    {
        if (kind == Types::ActorKind::Dog)
            return animation.family == Types::AnimationFamily::Dog;
        return animation.kind != Types::AnimationKind::Face && (animation.family == Types::AnimationFamily::MpPlayer ||
                                                                animation.family == Types::AnimationFamily::SpHuman ||
                                                                animation.family == Types::AnimationFamily::Human);
    }

    // a body's death: a favourite or recent death of the user's that fits its models (from the animation pack too;
    // made for its kind while that isn't known), else a person's first of DEATH_ANIMATIONS the game has, a dog's its
    // breed's
    std::string GetDeathAnimation(const ActorLogic::Body& body, const std::vector<std::string>* models)
    {
        const auto catalog = GetCatalog();
        if (!catalog)
            return {};
        const auto isDeath = [&](std::string_view name) {
            const auto animation = catalog->FindAnimation(name);
            if (!animation || GetAnimationCategory(*animation) != "Deaths")
                return false;
            const auto fits = models ? Fits(*models, name) : std::nullopt;
            return fits ? *fits : IsMadeFor(*animation, body.kind);
        };
        // (a killstreak dies as the game's does: its own death first)
        if (body.kind == Types::ActorKind::Vehicle)
        {
            const auto deaths = KillstreakLogic::GetDeaths(KillstreakLogic::Get(body.killstreak));
            return deaths.empty() ? std::string() : std::string(deaths.front());
        }
        for (const auto& list : {PreferencesConfiguration::Get().favoriteActorAnimations,
                                 PreferencesConfiguration::Get().recentActorAnimations})
            if (const auto found = std::ranges::find_if(list, isDeath); found != list.end())
                return *found;
        if (body.kind == Types::ActorKind::Dog)
        {
            for (const auto death :
                 {ActorLogic::GetDogDeath(body.breed), ActorLogic::GetDogDeath(ActorLogic::DogBreed::Shepherd)})
                if (catalog->FindAnimation(death))
                    return std::string(death);
            return {};
        }
        return ActorLogic::FindFirst(ActorLogic::DEATH_ANIMATIONS,
                                     [&](std::string_view name) { return HasOwnAnimation(*catalog, name); });
    }

    std::string GetDeathAnimation(const Types::Actor* actor)
    {
        return actor ? GetDeathAnimation(GetBody(*actor), &actor->models) : GetDeathAnimation({}, nullptr);
    }

    std::string GetIdleAnimation(const Types::Actor& actor)
    {
        const auto body = GetBody(actor);
        if (body.kind == Types::ActorKind::Vehicle)
            return std::string(
                KillstreakLogic::GetMotion(KillstreakLogic::Get(body.killstreak).vehicleClass, ActorLogic::Gait::Idle));
        if (body.kind != Types::ActorKind::Dog)
            return "casual_stand_idle";
        const auto catalog = GetCatalog();
        return ActorLogic::FindLocomotionAnimation(
            body, Types::ActorStance::Stand, ActorLogic::Gait::Idle, ActorLogic::Heading::Forward,
            [&](std::string_view name) { return catalog && catalog->FindAnimation(name); });
    }

    std::vector<std::string> GetRecentAnimations()
    {
        return PreferencesConfiguration::Get().recentActorAnimations;
    }

    void AddRecentAnimation(std::string_view name)
    {
        ActorLogic::AddRecent(PreferencesConfiguration::Get().recentActorAnimations, name);
        Configuration::Get().Write(true);
    }

    std::optional<Types::ActorCatalog::EffectInfo> GetEffectInfo(std::string_view effect)
    {
        const auto current = GetCatalog();
        if (!current)
            return std::nullopt;
        const auto found = current->effectInfo.find(effect);
        return found != current->effectInfo.end() ? std::optional(found->second) : std::nullopt;
    }

    std::vector<std::string> GetRecentProps()
    {
        return PreferencesConfiguration::Get().recentProps;
    }

    void AddRecentProp(std::string_view name)
    {
        ActorLogic::AddRecent(PreferencesConfiguration::Get().recentProps, name);
        Configuration::Get().Write(true);
    }

    bool IsFavoriteEffect(std::string_view name)
    {
        const auto& favorites = PreferencesConfiguration::Get().favoriteEffects;
        return std::ranges::find(favorites, name) != favorites.end();
    }

    void SetFavoriteEffect(std::string_view name, bool favorite)
    {
        auto& favorites = PreferencesConfiguration::Get().favoriteEffects;
        const auto it = std::ranges::find(favorites, name);
        if (favorite == (it != favorites.end()))
            return;
        if (favorite)
            favorites.emplace_back(name);
        else
            favorites.erase(it);
        Configuration::Get().Write(true);
    }

    std::vector<std::string> GetRecentEffects()
    {
        return PreferencesConfiguration::Get().recentEffects;
    }

    void AddRecentEffect(std::string_view name)
    {
        ActorLogic::AddRecent(PreferencesConfiguration::Get().recentEffects, name);
        Configuration::Get().Write(true);
    }

    std::string GetPack()
    {
        std::lock_guard lock(mutex);
        return pack;
    }

    void SetPack(const std::string& newPack)
    {
        {
            std::lock_guard lock(mutex);
            if (pack != newPack)
                Changed();
            pack = newPack;
        }
        if (!newPack.empty())
            Mod::GetGameInterface()->LoadContentPack(newPack);
    }

    const char* GetPresetNode(PresetKind kind)
    {
        return kind == PresetKind::Look ? "looks" : "clips";
    }

    std::vector<std::string> GetPresetNames(PresetKind kind)
    {
        std::vector<std::string> names;
        const auto& presets = PreferencesConfiguration::Get().actorPresets;
        if (const auto it = presets.find(GetPresetNode(kind)); it != presets.end() && it->is_object())
            for (const auto& [name, _] : it->items())
                names.push_back(name);
        return names;
    }

    std::vector<std::string> GetPresetAnimations(const std::string& name)
    {
        // (as it adds them to an actor)
        Types::Actor actor;
        if (!ApplyPreset(PresetKind::Clips, name, actor, 0))
            return {};
        std::vector<std::string> animations;
        for (const auto* row : {&actor.clips, &actor.faceClips})
            for (const auto& clip : *row)
                if (std::ranges::find(animations, clip.animation) == animations.end())
                    animations.push_back(clip.animation);
        return animations;
    }

    void SavePreset(PresetKind kind, const std::string& name, const Types::Actor& actor)
    {
        auto& presets = PreferencesConfiguration::Get().actorPresets;
        presets[GetPresetNode(kind)][name] =
            kind == PresetKind::Look ? ActorLogic::LookToJson(actor) : ActorLogic::ClipsToJson(actor);
        Configuration::Get().Write(true);
    }

    bool ApplyPreset(PresetKind kind, const std::string& name, Types::Actor& actor, std::uint32_t tick)
    {
        const auto& presets = PreferencesConfiguration::Get().actorPresets;
        const auto list = presets.find(GetPresetNode(kind));
        if (list == presets.end() || !list->contains(name))
            return false;
        if (kind == PresetKind::Look)
        {
            // (a person made another goes on as it did, see ActorLogic::KeepMovementSet)
            const auto before = GetBody(actor);
            ActorLogic::ApplyLook(actor, list->at(name));
            if (before.kind == Types::ActorKind::Human && GetActorKind(actor) == Types::ActorKind::Human)
                ActorLogic::KeepMovementSet(actor, before.set, GetMovementSet(actor));
        }
        else
            ActorLogic::ApplyClips(actor, list->at(name), tick);
        return true;
    }

    void DeletePreset(PresetKind kind, const std::string& name)
    {
        auto& presets = PreferencesConfiguration::Get().actorPresets;
        if (const auto list = presets.find(GetPresetNode(kind)); list != presets.end())
            list->erase(name);
        Configuration::Get().Write(true);
    }

    std::vector<std::uint32_t> AddFormation(std::uint32_t id, const ActorLogic::Formation& formation,
                                            std::uint32_t tick)
    {
        const auto actor = GetActor(id);
        if (!actor)
            return {};
        const auto yaw = EvaluatePlacement(*actor, tick).yaw;
        const auto copies = ActorLogic::MakeFormation(*actor, formation, yaw);

        std::lock_guard lock(mutex);
        std::vector<std::uint32_t> ids;
        if (copies.empty())
            return ids;
        Record(std::format("add a group of {}", copies.size() + 1));
        auto at = std::ranges::find(actors, id, &Types::Actor::id);
        for (auto copy : copies)
        {
            copy.id = nextId++;
            copy.name = UniqueName(actor->name);
            ids.push_back(copy.id);
            at = actors.insert(at + 1, std::move(copy));
        }
        Changed();
        return ids;
    }

    std::string CopyLookToPlayer(const Types::Actor& actor, std::int32_t client)
    {
        const auto catalog = Loadouts::GetCatalog();
        const auto shownCatalog = GetCatalog();
        if (!catalog || !shownCatalog)
            return "The weapons and characters of this map aren't found yet";

        auto loadout = Loadouts::GetLoadoutForPlayer(client).value_or(Types::Loadout{});
        bool copied = false;
        // (a person's models, the map's or a campaign character's, which load for players as for actors)
        if (GetActorKind(actor) == Types::ActorKind::Human && !actor.models.empty())
        {
            PlayerLookLogic::SetModels(loadout, actor.models, actor.character, [&](const std::string& model) {
                const auto found = shownCatalog->FindModel(model);
                return found && found->kind == Types::ActorCatalog::ModelKind::Head;
            });
            loadout.facePaint = actor.facePaint;
            copied = true;
        }

        if (const auto weapon = catalog->FindWeapon(actor.weapon); weapon && !weapon->actorsOnly)
        {
            const auto from = std::string(weapon->primary ? Types::WeaponRule::ANY_PRIMARY
                                                          : Types::WeaponRule::ANY_SECONDARY);
            std::erase_if(loadout.weaponRules, [&](const Types::WeaponRule& rule) { return rule.from == from; });
            loadout.weaponRules.push_back(
                {.from = from, .weapon = actor.weapon, .attachments = actor.attachments, .camo = actor.camo});
            copied = true;
        }
        if (!copied)
            return "It has neither a person's models nor a weapon of the map";
        if (!actor.stowedWeapon.empty())
            loadout.stowed = actor.stowedWeapon;
        Loadouts::SetLoadoutForPlayer(client, loadout);
        return {};
    }

    std::vector<std::string> GetFacePaintModels()
    {
        std::vector<std::string> models;
        // (a live game has the last demo's scene still here, unseen)
        if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
            for (const auto& actor : GetActors())
                if (actor.facePaint > 0)
                    models.insert(models.end(), actor.models.begin(), actor.models.end());
        if (Loadouts::HasFacePaint())
            for (auto& chosen : Loadouts::GetChosenModels())
                models.insert(models.end(), chosen.models.begin(), chosen.models.end());
        return models;
    }

    bool SetLoadoutCharacter(Types::Loadout& loadout, std::string_view id)
    {
        const auto shownCatalog = GetCatalog();
        const auto character = shownCatalog ? shownCatalog->FindCharacter(id) : nullptr;
        if (!character)
            return false;
        PlayerLookLogic::SetCharacter(loadout, *character);
        return true;
    }

    std::string DescribeModel(std::string_view model)
    {
        // c_<faction>_<rest>: the faction is the first two parts
        std::string text(model);
        if (text.starts_with("c_"))
            text.erase(0, 2);
        std::vector<std::string> parts;
        for (std::size_t start = 0; start <= text.size();)
        {
            const auto end = std::min(text.find('_', start), text.size());
            parts.emplace_back(text.substr(start, end - start));
            start = end + 1;
        }
        auto capitalized = [](std::string word) {
            if (!word.empty())
                word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
            return word;
        };

        std::string faction, rest;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            auto& target = i < 2 && parts.size() > 2 ? faction : rest;
            // a number ends the word before it: head1 -> Head 1
            auto word = parts[i];
            const auto digits = word.find_first_of("0123456789");
            if (digits != std::string::npos && digits > 0)
                word.insert(digits, " ");
            if (word == "fb")
                continue;
            target += (target.empty() ? "" : " ") + capitalized(word);
        }
        return faction.empty() ? rest : std::format("{}: {}", faction, rest);
    }

    void ReloadScene()
    {
        loadPending = true;
    }

    const CharacterLogic::CampaignCatalog& GetCampaignCharacters()
    {
        return campaignCharacters;
    }

    const std::unordered_map<std::string, Types::ActorCatalog::ModelKind>& GetCampaignModelKinds()
    {
        return campaignModelKinds;
    }

    bool IsFavoriteCharacter(std::string_view id)
    {
        const auto& favorites = PreferencesConfiguration::Get().favoriteCharacters;
        return std::ranges::find(favorites, id) != favorites.end();
    }

    void SetFavoriteCharacter(std::string_view id, bool favorite)
    {
        auto& favorites = PreferencesConfiguration::Get().favoriteCharacters;
        const auto it = std::ranges::find(favorites, id);
        if (favorite == (it != favorites.end()))
            return;
        if (favorite)
            favorites.emplace_back(id);
        else
            favorites.erase(it);
        Configuration::Get().Write(true);
    }

    const Types::Character* GetCharacter(const Types::Actor& actor, const Types::ActorCatalog& catalog)
    {
        if (const auto character = catalog.FindCharacter(actor.character))
            return character;
        return CharacterLogic::FindByModels(catalog.characters, actor.models);
    }

    void ApplyCharacter(Types::Actor& actor, const Types::Character& character, const CharacterLogic::Look& look,
                        const Types::ActorCatalog& catalog)
    {
        const auto from = GetBody(actor);
        actor.character = character.id;
        actor.models = CharacterLogic::GetModels(character, look, catalog.FindCharacter(character.burnt));
        if (!HasCharacterZones() && !character.level.empty() && !catalog.HasModels(actor.models) &&
            GetPack() != character.level)
            SetPack(character.level);
        // another kind of body (a person a dog): what it did, as that does it, with that body's transitions; another
        // person goes on as it did, with the movement set it had (see ActorLogic::KeepMovementSet)
        auto to = GetBody(character, catalog);
        if (from.kind == Types::ActorKind::Human && to.kind == Types::ActorKind::Human)
            ActorLogic::KeepMovementSet(actor, from.set, GetMovementSet(actor));
        else
        {
            if (to.kind == Types::ActorKind::Human)
                to.gun = Types::GunClass::Rifle;
            // (a killstreak goes at its speeds)
            if (to.kind == Types::ActorKind::Vehicle && actor.locomotion != Types::ActorLocomotion::Auto)
            {
                actor.locomotion = Types::ActorLocomotion::Auto;
                actor.pathSpeed = KillstreakLogic::GetCruiseSpeed(KillstreakLogic::Get(to.killstreak));
            }
            Rechain(actor, [&] {
                ActorLogic::ChangeBody(actor, from, to, GetStartAnimations(to), GetAnimationLookup());
                // (its gaits as the new body goes them; a killstreak goes at its speeds)
                if (to.kind == Types::ActorKind::Vehicle)
                    actor.stretches.clear();
                else
                    ActorLogic::ResolveStretches(actor, to, GetAnyAnimation());
            });
        }
        // a flying killstreak in the air as high as it flies, and back down for a body that doesn't
        const auto wasHeight = actor.flyHeight;
        const auto killstreak = to.kind == Types::ActorKind::Vehicle ? std::optional(to.killstreak) : std::nullopt;
        const bool flies = killstreak && KillstreakLogic::Flies(KillstreakLogic::Get(*killstreak).vehicleClass);
        const auto height = flies ? (wasHeight > 0.0f ? wasHeight : KillstreakLogic::Get(*killstreak).flyHeight) : 0.0f;
        const bool wasFlying = from.kind == Types::ActorKind::Vehicle &&
                               KillstreakLogic::Flies(KillstreakLogic::Get(from.killstreak).vehicleClass);
        const auto lift = (flies ? height : 0.0f) - (wasFlying ? wasHeight : 0.0f);
        if (lift != 0.0f)
        {
            actor.position.z += lift;
            for (auto& point : actor.path)
                point.z += lift;
            for (auto& key : actor.keys)
                key.position.z += lift;
        }
        actor.flyHeight = height;
        if (flies != wasFlying)
            actor.followGround = !flies;
    }

    std::filesystem::path GetCharacterZonesFolder()
    {
        return PathUtils::GetModPath() / "zones";
    }

    bool HasCharacterZones()
    {
        std::lock_guard lock(zonesMutex);
        return !zonesIgnored && !availableZones.empty();
    }

    bool IsCharacterZoneAvailable(std::string_view zone)
    {
        std::lock_guard lock(zonesMutex);
        return !zonesIgnored && availableZones.contains(zone);
    }

    bool IsCharacterZoneMissingTextures(std::string_view zone)
    {
        std::lock_guard lock(zonesMutex);
        return zonesWithoutTextures.contains(zone);
    }

    void SetTexturesFile(std::filesystem::path file)
    {
        texturesFileInstead = std::move(file);
    }

    void SetExtraZonesFolder(std::filesystem::path folder)
    {
        extraZonesFolder = std::move(folder);
    }

    void IgnoreCharacterZones(bool ignore)
    {
        std::lock_guard lock(zonesMutex);
        zonesIgnored = ignore;
    }

    void RescanCharacterZones()
    {
        std::set<std::string, std::less<>> found;
        std::error_code error;
        for (const auto& folder : {GetCharacterZonesFolder(), extraZonesFolder})
            for (const auto& entry : std::filesystem::directory_iterator(folder, error))
            {
                const auto name = entry.path().stem().string();
                // (character zones, and the weapons' and their sounds')
                if (entry.is_regular_file(error) && entry.path().extension() == ".ff" &&
                    (name.starts_with("t5c_") || name.starts_with("t5w_") || name.starts_with("t5s_")))
                    found.insert(name);
            }
        // a zip lists what it holds at its end, so the file's last part is enough (it can be 100 MB)
        const auto& weapons = WeaponCatalog::Get();
        std::string archiveEnd;
        const auto textures = !texturesFileInstead.empty()
                                  ? texturesFileInstead
                                  : PathUtils::GetModPath().parent_path() / "main" / weapons.texturesFile;
        if (std::ifstream file(textures, std::ios::binary | std::ios::ate); file)
        {
            constexpr std::streamoff TAIL = 1 << 20;
            const auto size = static_cast<std::streamoff>(file.tellg());
            archiveEnd.resize(static_cast<std::size_t>(std::min(size, TAIL)));
            file.seekg(size - static_cast<std::streamoff>(archiveEnd.size()));
            file.read(archiveEnd.data(), static_cast<std::streamsize>(archiveEnd.size()));
        }
        std::set<std::string, std::less<>> withoutTextures;
        for (const auto& [zone, images] : weapons.characterTextures)
            if (found.contains(zone) && !WeaponCatalog::HasTextures(weapons, zone, archiveEnd))
                withoutTextures.insert(zone);
        for (const auto& zone : withoutTextures)
            found.erase(zone);
        std::lock_guard lock(zonesMutex);
        availableZones = std::move(found);
        zonesWithoutTextures = std::move(withoutTextures);
    }

    std::vector<std::string> GetActorZones(const Types::Actor& actor, const Types::ActorCatalog& catalog)
    {
        // a charred twin is its character's burnt look
        auto character = GetCharacter(actor, catalog);
        if (character && !character->burntOf.empty())
            if (const auto base = catalog.FindCharacter(character->burntOf))
                character = base;
        const auto burnt = character ? catalog.FindCharacter(character->burnt) : nullptr;
        auto zones = CharacterLogic::GetZonesFor(actor.models, character, burnt, zoneIndex);
        // and its weapon's, a campaign or Zombies one (a person's: a dog or a killstreak holds none)
        if (GetActorKind(actor) == Types::ActorKind::Human)
            for (const auto& weapon : {ResolveWeapon(actor), ResolveStowedWeapon(actor)})
                for (auto& zone : WeaponCatalog::GetZones(WeaponCatalog::Get(), weapon))
                    if (std::ranges::find(zones, zone) == zones.end())
                        zones.push_back(std::move(zone));
        std::erase_if(zones, [](const std::string& zone) { return !IsCharacterZoneAvailable(zone); });
        return zones;
    }

    std::vector<std::string> GetWantedCharacterZones()
    {
        const auto shownCatalog = GetCatalog();
        if (!shownCatalog || !HasCharacterZones())
            return {};
        // (a live game has the last demo's scene still here, unseen)
        auto scene = Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo ? GetActors()
                                                                                         : std::vector<Types::Actor>{};
        const auto first = GetSelected();
        std::ranges::stable_partition(scene, [&](const Types::Actor& actor) { return actor.id == first; });
        std::vector<std::string> wanted;
        for (const auto& actor : scene)
            for (const auto& zone : GetActorZones(actor, *shownCatalog))
                if (std::ranges::find(wanted, zone) == wanted.end())
                    wanted.push_back(zone);
        // and those of what players of the demo wear
        for (const auto& chosen : Loadouts::GetChosenModels())
        {
            const auto character = shownCatalog->FindCharacter(chosen.character);
            const auto burnt = character ? shownCatalog->FindCharacter(character->burnt) : nullptr;
            for (const auto& zone : CharacterLogic::GetZonesFor(chosen.models, character, burnt, zoneIndex))
                if (IsCharacterZoneAvailable(zone) && std::ranges::find(wanted, zone) == wanted.end())
                    wanted.push_back(zone);
        }
        return wanted;
    }
    std::vector<ActorLogic::SpeedPreset> GetGaits(const Types::Actor& actor)
    {
        return ActorLogic::GetGaits(GetBody(actor));
    }

    std::optional<GaitState> GetGait(const Types::Actor& actor, std::size_t point)
    {
        const auto body = GetBody(actor);
        if (actor.movement != Types::ActorMovement::Path || actor.locomotion != Types::ActorLocomotion::Auto ||
            body.kind == Types::ActorKind::Vehicle)
            return std::nullopt;
        const auto animations = GetAnimationLookup();
        if (const auto* stretch = ActorLogic::GetStretch(actor, point))
            return GaitState{stretch->gait, stretch->crouched, actor.pace, false,
                             ActorLogic::GetStretchSpeed(actor, *stretch, animations)};
        // from before gaits: the one its speed picks, at the pace that keeps that speed
        auto trial = actor;
        if (!ActorLogic::MakeStretches(trial, body, animations, GetAnyAnimation()))
            return std::nullopt;
        const auto& stretch = trial.stretches.front();
        return GaitState{stretch.gait, stretch.crouched, trial.pace, true,
                         ActorLogic::GetStretchSpeed(trial, stretch, animations)};
    }

    void SetGait(Types::Actor& actor, std::size_t from, std::size_t to, Types::ActorGait gait, bool crouched)
    {
        ActorLogic::SetGait(actor, from, to, gait, crouched, GetBody(actor), GetAnimationLookup(), GetAnyAnimation());
    }

    bool MakeStretches(Types::Actor& actor)
    {
        return ActorLogic::MakeStretches(actor, GetBody(actor), GetAnimationLookup(), GetAnyAnimation());
    }

    void SetPace(Types::Actor& actor, float pace)
    {
        if (!ActorLogic::MakeStretches(actor, GetBody(actor), GetAnimationLookup(), GetAnyAnimation()))
            return;
        actor.pace = std::clamp(pace, ActorLogic::MIN_PACE_FROM_BEFORE, ActorLogic::MAX_PACE_FROM_BEFORE);
    }

    std::optional<Arrival> SolveArrival(const Types::Actor& actor, std::uint32_t tick)
    {
        auto trial = actor;
        const auto body = GetBody(trial);
        const auto animations = GetAnimationLookup();
        if (!ActorLogic::MakeStretches(trial, body, animations, GetAnyAnimation()))
            return std::nullopt;
        const auto pace =
            ActorLogic::SolvePace(trial, tick, animations, [](Types::Actor& fitted) { FitOnlyWalk(fitted); });
        if (!pace)
            return std::nullopt;
        trial.pace = *pace;
        Arrival arrival{.pace = trial.pace};
        // a faster or slower gait where the pace is beyond what keeps its feet on the ground
        const auto& last = trial.stretches.back();
        if (trial.pace > ActorLogic::MAX_PACE && last.gait != Types::ActorGait::Sprint)
            arrival.instead = static_cast<Types::ActorGait>(static_cast<int>(last.gait) + 1);
        else if (trial.pace < ActorLogic::MIN_PACE && last.gait != Types::ActorGait::Walk)
            arrival.instead = static_cast<Types::ActorGait>(static_cast<int>(last.gait) - 1);
        return arrival;
    }

    bool ArriveAt(Types::Actor& actor, std::uint32_t tick)
    {
        const auto arrival = SolveArrival(actor, tick);
        if (!arrival)
            return false;
        SetPace(actor, std::clamp(arrival->pace, ActorLogic::MIN_PACE, ActorLogic::MAX_PACE));
        FitOnlyWalk(actor);
        return arrival->pace >= ActorLogic::MIN_PACE && arrival->pace <= ActorLogic::MAX_PACE;
    }

    ActorLogic::PathTiming GetPathTiming(const Types::Actor& actor)
    {
        // (worked out once a change of the actors, or of what the game has measured of their animations: the timeline
        // asks every frame)
        static std::mutex timingsMutex;
        static std::map<std::uint32_t, std::pair<std::uint64_t, ActorLogic::PathTiming>> timings;
        const auto now = (static_cast<std::uint64_t>(generation.load()) << 32) | measured.load();
        // (as shown, for the notes: not while a re-plan is pending)
        const auto shown = [&](const ActorLogic::PathTiming& timing) {
            std::lock_guard lock(mutex);
            if (!pendingReplans.contains(actor.id))
                shownArrivals[actor.id] = timing.arrival;
            return timing;
        };
        {
            std::unique_lock lock(timingsMutex);
            if (const auto found = timings.find(actor.id); found != timings.end() && found->second.first == now)
            {
                const auto timing = found->second.second;
                lock.unlock();
                return shown(timing);
            }
        }
        auto timing = ActorLogic::GetPathTiming(actor, GetAnimationLookup());
        {
            std::lock_guard lock(timingsMutex);
            timings[actor.id] = {now, timing};
        }
        return shown(timing);
    }

    // GetJoinDistance's, each time
    std::optional<float> WorkOutJoinDistance(const Types::Actor& actor, std::size_t stretch);

    std::vector<ActorLogic::WalkPiece> GetWalkPieces(const Types::Actor& actor)
    {
        // (worked out once a change, as the timing)
        static std::mutex piecesMutex;
        static std::map<std::uint32_t, std::pair<std::uint64_t, std::vector<ActorLogic::WalkPiece>>> pieces;
        const auto now = (static_cast<std::uint64_t>(generation.load()) << 32) | measured.load();
        {
            std::lock_guard lock(piecesMutex);
            if (const auto found = pieces.find(actor.id); found != pieces.end() && found->second.first == now)
                return found->second.second;
        }
        auto found = ActorLogic::GetWalkPieces(actor, GetPathTiming(actor), GetAnimationLookup());
        std::lock_guard lock(piecesMutex);
        pieces[actor.id] = {now, found};
        return found;
    }

    std::optional<float> GetJoinDistance(const Types::Actor& actor, std::size_t stretch)
    {
        // (worked out once a change, as the timing; while the poses aren't known, again every half second)
        static std::mutex distancesMutex;
        struct Distance
        {
            std::uint64_t generation = 0;
            std::optional<float> distance;
            std::chrono::steady_clock::time_point at;
        };
        static std::map<std::pair<std::uint32_t, std::size_t>, Distance> distances;
        const auto now = (static_cast<std::uint64_t>(generation.load()) << 32) | measured.load();
        const auto key = std::pair(actor.id, stretch);
        {
            std::lock_guard lock(distancesMutex);
            if (const auto found = distances.find(key);
                found != distances.end() && found->second.generation == now &&
                (found->second.distance ||
                 std::chrono::steady_clock::now() - found->second.at < std::chrono::milliseconds(500)))
                return found->second.distance;
        }
        const auto distance = WorkOutJoinDistance(actor, stretch);
        std::lock_guard lock(distancesMutex);
        distances[key] = {now, distance, std::chrono::steady_clock::now()};
        return distance;
    }

    std::optional<float> WorkOutJoinDistance(const Types::Actor& actor, std::size_t stretch)
    {
        const auto timing = GetPathTiming(actor);
        if (stretch == 0 || stretch >= actor.stretches.size() || stretch >= timing.joins.size() ||
            !timing.joins[stretch] || actor.models.empty())
            return std::nullopt;
        // the two looks as the blend starts (a moment into it, both shown), each as far into its play as it is then
        const auto& outgoing = actor.stretches[stretch - 1].animation;
        const auto& incoming = actor.stretches[stretch].animation;
        const auto layers = GetLayers(actor, timing.joins[stretch]->first + 10, GetBody(actor));
        const auto animations = GetAnimationLookup();
        const auto poseOf = [&](const std::string& animation) -> std::optional<TransitionLogic::Pose> {
            const auto legs = std::ranges::find_if(layers.legs, [&](const ActorLogic::LegsSample& sample) {
                return !sample.clip && sample.animation == animation;
            });
            const auto info = animations(animation);
            if (legs == layers.legs.end() || !info || info->duration <= 0.0f)
                return std::nullopt;
            const auto plays = legs->time / info->duration;
            const auto pose = Mod::GetGameInterface()->GetAnimationPoseByName(animation, actor.models.front(),
                                                                              plays - std::floor(plays));
            return pose ? std::optional(TransitionLogic::MakePose(*pose)) : std::nullopt;
        };
        const auto from = poseOf(outgoing), to = poseOf(incoming);
        return from && to ? TransitionLogic::GetPoseDistance(*from, *to) : std::nullopt;
    }

    std::optional<std::uint32_t> GetCleanBlend(const Types::Actor& actor, std::uint32_t tick,
                                               const std::string& animation)
    {
        const auto shown = std::ranges::find_if(actor.clips.rbegin(), actor.clips.rend(),
                                                [&](const Types::ActorClip& clip) { return clip.start <= tick; });
        if (shown == actor.clips.rend())
            return std::nullopt;
        const auto poses = PoseSignatures::GetLookup(actor);
        const auto before = poses(shown->animation), now = poses(animation);
        const auto distance = before && now ? TransitionLogic::GetPoseDistance(before->last, now->first) : std::nullopt;
        if (!distance)
            return std::nullopt;
        // (as IsCleanJoin has it: the distance spread over the blend, at most CLEAN_JOIN in 50 ms)
        const auto needed =
            static_cast<std::uint32_t>(std::ceil(*distance * 50.0f / TransitionLogic::CLEAN_JOIN / 10.0f)) * 10;
        return std::clamp(needed, MIN_CLEAN_BLEND, MAX_CLEAN_BLEND);
    }

    void BlendCleanly(std::uint32_t actorId, std::uint32_t clipId)
    {
        std::optional<std::uint32_t> blendIn;
        if (const auto actor = GetActor(actorId))
            if (const auto clip = std::ranges::find(actor->clips, clipId, &Types::ActorClip::id);
                clip != actor->clips.end())
                blendIn = clip->blendIn;
        std::lock_guard lock(mutex);
        std::erase_if(pendingBlends,
                      [&](const PendingBlend& each) { return each.actor == actorId && each.clip == clipId; });
        pendingBlends.push_back({actorId, clipId, blendIn, std::chrono::steady_clock::now()});
    }

    namespace
    {
        // The change a re-plan or a worked-out blend makes, as part of the last undo step (the change that asked for
        // it), unless the actor was changed since it was read (by another thread, such as the harness's): then that
        // change stays, and returns false
        bool UpdateUnlessChanged(const Types::Actor& before, const Types::Actor& actor)
        {
            std::lock_guard lock(mutex);
            const auto it = std::ranges::find(actors, actor.id, &Types::Actor::id);
            if (it == actors.end() || *it != before)
                return false;
            auto sorted = actor;
            SortClips(sorted);
            if (*it == sorted)
                return true;
            Record("change " + it->name, true);
            *it = std::move(sorted);
            Changed();
            return true;
        }

        // the blends of clips added, once their poses are known; not one changed since (the user's)
        void RunPendingBlends()
        {
            std::vector<PendingBlend> blends;
            {
                std::lock_guard lock(mutex);
                blends.swap(pendingBlends);
            }
            const auto now = std::chrono::steady_clock::now();
            for (const auto& pending : blends)
            {
                auto actor = GetActor(pending.actor);
                if (!actor || now - pending.asked > BLEND_WAIT)
                    continue;
                const auto clip = std::ranges::find(actor->clips, pending.clip, &Types::ActorClip::id);
                if (clip == actor->clips.end() || clip->start == 0 ||
                    (pending.blendIn && clip->blendIn != *pending.blendIn))
                    continue;
                if (const auto blend = GetCleanBlend(*actor, clip->start - 1, clip->animation))
                {
                    const auto before = *actor;
                    clip->blendIn = *blend;
                    if (*blend == before.clips[static_cast<std::size_t>(clip - actor->clips.begin())].blendIn)
                        continue;
                    if (UpdateUnlessChanged(before, *actor))
                    {
                        // (the walk gets to the path's end as the clip after it has taken over: fitted again to the
                        // blend, as a blend typed is)
                        if (actor->movement == Types::ActorMovement::Path && !actor->stretches.empty())
                        {
                            std::lock_guard lock(mutex);
                            pendingReplans[actor->id];
                        }
                        continue;
                    }
                }
                std::lock_guard lock(mutex);
                pendingBlends.push_back(pending);
            }
        }
    }  // namespace

    void RunPendingReplans()
    {
        RunPendingBlends();
        std::map<std::uint32_t, PendingReplan> pending;
        {
            std::lock_guard lock(mutex);
            pending.swap(pendingReplans);
        }
        // (an arrival moved less than this goes unsaid)
        constexpr int NOTED_MOVE = 50;
        for (const auto& [id, asked] : pending)
        {
            auto actor = GetActor(id);
            if (!actor || actor->movement != Types::ActorMovement::Path || actor->path.size() < 2 ||
                GetWalkFittedActor() == id)
                continue;
            const auto before = *actor;
            const auto animations = GetAnimationLookup();
            // when it arrived as last shown, before the change; where its one-offs on the path's end started
            std::optional<std::uint32_t> shown;
            {
                std::lock_guard lock(mutex);
                if (const auto found = shownArrivals.find(id); found != shownArrivals.end())
                    shown = found->second;
            }
            // (one-offs: a loop after the walk, which the walk stops into, hangs on nothing)
            std::map<std::uint32_t, std::uint32_t> onEnd;
            for (const auto& clip : before.clips)
                if (clip.chain == Types::ActorChain::None && clip.anchor == Types::ActorAnchor::End && !clip.loop)
                    onEnd[clip.id] = clip.start;

            FitOnlyWalk(*actor, asked.anchored);

            // said once, with the other choice: its arrival moved (a look or body changed keeps it if a pace within
            // MIN_PACE..MAX_PACE can), its one-offs went with the end
            const auto after = ActorLogic::GetPathTiming(*actor, animations);
            const auto describe = [](const std::optional<std::uint32_t>& tick) {
                return tick ? std::to_string(*tick) : std::string("-");
            };
            LOG_INFO("Planned {} again: arrives {} (shown {}){}{}, {} anchored", actor->name, describe(after.arrival),
                     describe(shown), asked.keepTime ? ", keeping time" : "", asked.noted ? "" : ", unsaid",
                     asked.anchored.size());
            if (asked.noted && shown && after.arrival &&
                std::abs(static_cast<int>(*after.arrival) - static_cast<int>(*shown)) >= NOTED_MOVE)
            {
                const auto arrival = asked.keepTime ? SolveArrival(*actor, *shown) : std::nullopt;
                if (arrival && arrival->pace >= ActorLogic::MIN_PACE && arrival->pace <= ActorLogic::MAX_PACE)
                {
                    const auto pace = actor->pace;
                    SetPace(*actor, arrival->pace);
                    FitOnlyWalk(*actor);
                    AddNote({.actor = id,
                             .kind = "arrival",
                             .text = std::format("Kept {:.2f} s: pace {:.0f}%", *shown / 1000.0f, actor->pace * 100.0f),
                             .letMovePace = pace});
                }
                else
                {
                    const auto moved = static_cast<int>(*after.arrival) - static_cast<int>(*shown);
                    AddNote({.actor = id,
                             .kind = "arrival",
                             .text = std::format("Arrives {:.2f} s {}", std::abs(moved) / 1000.0f,
                                                 moved > 0 ? "later" : "sooner"),
                             .keepArrival = *shown});
                }
            }
            for (const auto& clip : actor->clips)
                if (const auto was = onEnd.find(clip.id);
                    was != onEnd.end() && was->second != clip.start && !asked.anchored.contains(clip.id))
                {
                    const bool death = ActorLogic::IsDeath(clip.animation, animations(clip.animation));
                    AddNote(
                        {.actor = id,
                         .kind = "end",
                         .text = std::format("{} went with the end: {:.2f} s",
                                             death ? std::string("Its death") : clip.animation, clip.start / 1000.0f)});
                }
            if (*actor == before)
                continue;
            // the transitions it put in or moved, and the one-offs it moved
            ReplanFlash flash{.actor = id, .at = std::chrono::steady_clock::now()};
            for (const auto& clip : actor->clips)
            {
                const bool transition = clip.chain != Types::ActorChain::None && clip.chain != Types::ActorChain::Walk;
                const bool oneOff = clip.chain == Types::ActorChain::None && !clip.loop;
                if ((transition || oneOff) && !asked.anchored.contains(clip.id) &&
                    std::ranges::none_of(before.clips, [&](const Types::ActorClip& was) {
                        return was.chain == clip.chain && was.animation == clip.animation && was.start == clip.start;
                    }))
                    flash.clips.push_back(clip.id);
            }
            // (undone with the change that asked for it; one changed meanwhile is planned again next time)
            const bool updated = UpdateUnlessChanged(before, *actor);
            std::lock_guard lock(mutex);
            if (!updated)
            {
                auto& again = pendingReplans[id];
                again.anchored.insert(asked.anchored.begin(), asked.anchored.end());
                again.keepTime = again.keepTime || asked.keepTime;
                again.noted = again.noted || asked.noted;
            }
            else if (!flash.clips.empty())
                flashes.push_back(std::move(flash));
        }
        // (the notes of changes planned now)
        std::vector<Note> said;
        {
            std::lock_guard lock(mutex);
            std::erase_if(pendingNotes, [&](const Note& note) {
                if (pendingReplans.contains(note.actor))
                    return false;
                said.push_back(note);
                return true;
            });
        }
        for (auto& note : said)
            AddNote(std::move(note));
    }

    std::vector<ReplanFlash> GetFlashes()
    {
        std::lock_guard lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(flashes, [&](const ReplanFlash& flash) { return now - flash.at > std::chrono::seconds(1); });
        return flashes;
    }

    std::vector<Note> GetNotes(std::uint32_t actor)
    {
        std::lock_guard lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(notes, [&](const Note& note) { return now - note.at > std::chrono::seconds(12); });
        std::vector<Note> found;
        for (auto it = notes.rbegin(); it != notes.rend(); ++it)
            if (it->actor == actor)
                found.push_back(*it);
        return found;
    }

    void AddNote(Note note)
    {
        std::lock_guard lock(mutex);
        note.at = std::chrono::steady_clock::now();
        std::erase_if(notes, [&](const Note& each) { return each.actor == note.actor && each.kind == note.kind; });
        notes.push_back(std::move(note));
    }

    void DismissNotes(std::uint32_t actor)
    {
        std::lock_guard lock(mutex);
        std::erase_if(notes, [&](const Note& note) { return note.actor == actor; });
    }

    std::optional<std::pair<Types::ActorGait, bool>> SetLook(Types::Actor& actor, std::optional<std::size_t> stretch,
                                                             const std::string& animation, bool said)
    {
        const auto& stretches = actor.stretches;
        const auto was = stretch && *stretch < stretches.size() ? std::optional(stretches[*stretch])
                         : stretches.empty()                    ? std::nullopt
                                                                : std::optional(stretches.front());
        const auto animations = GetAnimationLookup();
        const auto gait = ActorLogic::SetLook(actor, stretch, animation, GetBody(actor), animations, GetAnyAnimation());
        // a look of another gait: the stretch (or path) goes at it now, said once
        if (said && gait && was && (gait->first != was->gait || gait->second != was->crouched))
        {
            const auto name = std::format("{}{}", gait->second ? "Crouch " : "",
                                          gait->first == Types::ActorGait::Walk  ? (gait->second ? "walk" : "Walk")
                                          : gait->first == Types::ActorGait::Run ? (gait->second ? "run" : "Run")
                                                                                 : "Sprint");
            // (said once the change is planned: the change itself takes the notes before it away)
            std::lock_guard lock(mutex);
            pendingNotes.push_back({.actor = actor.id,
                                    .kind = "gait",
                                    .text = std::format("{} is {} ({:.0f} u/s): {} {} now", animation,
                                                        gait->first == Types::ActorGait::Walk  ? "a walk"
                                                        : gait->first == Types::ActorGait::Run ? "a run"
                                                                                               : "a sprint",
                                                        ActorLogic::GetAnimationSpeed(animation, animations),
                                                        stretch ? "this stretch is" : "the path is", name)});
        }
        return gait;
    }

    std::string GetShownAnimation(const Types::Actor& actor, const Types::ActorClip& clip)
    {
        if (actor.movement != Types::ActorMovement::Path || actor.stretches.empty() ||
            actor.locomotion != Types::ActorLocomotion::Auto || clip.picked ||
            (clip.chain != Types::ActorChain::None && clip.chain != Types::ActorChain::Walk))
            return clip.animation;
        const auto locomotion = ActorLogic::FindLocomotion(actor, clip.animation);
        if (!locomotion || locomotion->gait == ActorLogic::Gait::Idle || locomotion->vehicle)
            return clip.animation;
        // (the stretch it sets off on: the first one for one that starts before the actor goes)
        const auto& stretch =
            actor.stretches[std::min(GetStretchAt(actor, clip.start + 1).value_or(0), actor.stretches.size() - 1)];
        return stretch.animation.empty() ? clip.animation : stretch.animation;
    }

    std::optional<std::size_t> GetStretchAt(const Types::Actor& actor, std::uint32_t tick)
    {
        if (actor.movement != Types::ActorMovement::Path || actor.stretches.empty() || actor.path.size() < 2)
            return std::nullopt;
        const auto timing = GetPathTiming(actor);
        std::optional<std::size_t> found;
        for (std::size_t i = 0; i + 1 < actor.path.size() && i < actor.stretches.size(); ++i)
            if (timing.left[i] && *timing.left[i] <= tick)
                found = i;
        return found;
    }

    std::optional<std::uint32_t> GetCutAt(const Types::Actor& actor)
    {
        if (actor.movement != Types::ActorMovement::Path || actor.path.size() < 2 || actor.clips.empty())
            return std::nullopt;
        const auto timing = GetPathTiming(actor);
        const auto& last = actor.clips.back();
        if (!timing.start || timing.arrival || last.chain != Types::ActorChain::None || last.loop ||
            last.start <= *timing.start || !ActorLogic::IsDeath(last.animation, GetAnimationLookup()(last.animation)))
            return std::nullopt;
        return last.start;
    }

    void Preview(const Types::Actor& actor)
    {
        std::lock_guard lock(mutex);
        const auto it = std::ranges::find(actors, actor.id, &Types::Actor::id);
        if (it == actors.end())
            return;
        auto sorted = actor;
        SortClips(sorted);
        if (*it == sorted)
            return;
        *it = std::move(sorted);
        Changed();
    }

    void SetPreviewLoop(std::optional<PreviewLoop> loop)
    {
        std::lock_guard lock(mutex);
        if (loop && loop->to <= loop->from)
            loop.reset();
        if (loop && previewLoop && previewLoop->actor == loop->actor && previewLoop->from == loop->from &&
            previewLoop->to == loop->to)
            return;
        previewLoop = loop;
        previewLoopSince = std::chrono::steady_clock::now();
    }

    std::optional<std::uint32_t> GetPreviewTick(std::uint32_t actor)
    {
        std::lock_guard lock(mutex);
        if (!previewLoop || previewLoop->actor != actor)
            return std::nullopt;
        const auto since =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - previewLoopSince)
                .count();
        return previewLoop->from + static_cast<std::uint32_t>(since % (previewLoop->to - previewLoop->from));
    }

    std::vector<std::string> GetLookCandidates(const Types::Actor& actor, Types::ActorGait gait, bool crouched,
                                               std::size_t most)
    {
        const auto& catalog = GetAnimationCatalog();
        const auto fits = GetFits(actor.models);
        const auto body = GetBody(actor);
        const auto animations = GetAnimationLookup();
        const auto movement = gait == Types::ActorGait::Sprint ? Types::AnimationMovement::Sprint
                              : gait == Types::ActorGait::Run  ? Types::AnimationMovement::Run
                                                               : Types::AnimationMovement::Walk;
        const auto posture = crouched ? Types::AnimationPosture::Crouch : Types::AnimationPosture::Stand;
        const auto own = ActorLogic::GetGaitAnimation(body, gait, crouched, GetAnyAnimation());
        const auto ownSpeed = ActorLogic::GetAnimationSpeed(own, animations);
        std::vector<std::pair<float, std::string>> found;
        for (std::size_t i = 0; i < catalog.entries.size(); ++i)
        {
            const auto& entry = catalog.entries[i];
            // (a variant of the same asset in another zone, named with a space, once)
            if (!entry.loop || entry.death || entry.frames == 0 || entry.from.movement != movement ||
                (body.kind != Types::ActorKind::Dog && entry.from.posture != posture) ||
                (fits && i < fits->size() && !(*fits)[i]) || entry.name.find(' ') != std::string::npos)
                continue;
            const auto speed = glm::length(glm::vec2(entry.motion)) / std::max(entry.Duration(), 0.01f);
            const auto angle = glm::degrees(std::atan2(entry.motion.y, entry.motion.x));
            if (speed < ActorLogic::IDLE_SPEED || std::abs(angle) > TransitionLogic::MAX_WALK_ANGLE)
                continue;
            found.emplace_back(entry.name == own ? -1.0f : std::abs(speed - ownSpeed), entry.name);
        }
        std::ranges::sort(found);
        std::vector<std::string> names;
        for (const auto& [off, name] : found)
            if (names.size() < most)
                names.push_back(name);
        return names;
    }

    std::vector<std::string> GetTriedLooks(std::uint32_t actor)
    {
        std::lock_guard lock(mutex);
        const auto found = triedLooks.find(actor);
        return found != triedLooks.end() ? found->second : std::vector<std::string>{};
    }

    void AddTriedLook(std::uint32_t actor, const std::string& animation)
    {
        std::lock_guard lock(mutex);
        ActorLogic::AddRecent(triedLooks[actor], animation, 6);
    }

    float GetSpeedAt(const Types::Actor& actor, std::uint32_t tick)
    {
        const auto before = EvaluatePlacement(actor, tick >= 50 ? tick - 50 : 0);
        const auto after = EvaluatePlacement(actor, tick + 50);
        return glm::distance(glm::vec2(before.position), glm::vec2(after.position)) / 0.1f;
    }

    DeathCandidates GetDeathCandidates(const Types::Actor& actor, std::uint32_t tick, std::size_t most)
    {
        DeathCandidates candidates;
        // a killstreak's: its deaths, its own first
        if (const auto killstreak = KillstreakLogic::FindByModels(actor.models))
        {
            for (const auto death : KillstreakLogic::GetDeaths(KillstreakLogic::Get(*killstreak)))
                candidates.suited.emplace_back(death);
            candidates.motion = "as the game's";
            return candidates;
        }
        const auto& catalog = GetAnimationCatalog();
        const auto fits = GetFits(actor.models);
        // what it does then: the clip before the death as it ends, or its path's stretch while it walks it, at the
        // speed it goes up to the tick (after it, a death's own motion)
        const auto animations = GetAnimationLookup();
        auto state = ActorLogic::GetSuitedState(actor, tick, GetSpeedAt(actor, tick >= 50 ? tick - 50 : 0),
                                                ActorLogic::GetPathTiming(actor, animations), animations);
        if (state.movement == Types::AnimationMovement::Sprint)
            state.movement = Types::AnimationMovement::Run;
        candidates.motion = ActorLogic::DescribeSuitedState(state);
        const auto movement = state.movement;
        const auto body = GetBody(actor);
        const auto family = body.set == Types::ActorMovementSet::Players ? Types::AnimationFamily::MpPlayer
                                                                         : Types::AnimationFamily::SpHuman;
        std::vector<std::string> suited, others;
        for (std::size_t i = 0; i < catalog.entries.size(); ++i)
        {
            const auto& entry = catalog.entries[i];
            if (!entry.death || (fits && i < fits->size() && !(*fits)[i]) || entry.name.find(' ') != std::string::npos)
                continue;
            // (a person's: not an animal's, a zombie's or a vehicle's, nor of another rig than its set's)
            const auto& name = entry.name;
            const auto has = [&](std::string_view part) { return name.find(part) != std::string::npos; };
            // (a player's body also dies as the campaign's AI do, after the players' own deaths: the rank below)
            if (body.kind == Types::ActorKind::Human &&
                (std::ranges::any_of(std::array<std::string_view, 14>{"monkey", "zombie", "dog", "boss", "horse",
                                                                      "napalm", "fire", "shepherd", "attacked",
                                                                      "hellhound", "vehicle", "jeep", "huey", "heli"},
                                     has) ||
                 (entry.family != family &&
                  !(family == Types::AnimationFamily::MpPlayer && entry.family == Types::AnimationFamily::SpHuman))))
                continue;
            const bool running = entry.from.movement == Types::AnimationMovement::Run ||
                                 entry.from.movement == Types::AnimationMovement::Sprint || has("run") || has("sprint");
            const bool walking = entry.from.movement == Types::AnimationMovement::Walk || has("walk");
            const auto posture = entry.from.posture == Types::AnimationPosture::Prone || has("prone")
                                     ? Types::AnimationPosture::Prone
                                 : entry.from.posture == Types::AnimationPosture::Crouch || has("crouch")
                                     ? Types::AnimationPosture::Crouch
                                     : Types::AnimationPosture::Stand;
            const bool suits =
                posture == (state.posture == Types::AnimationPosture::Back ? Types::AnimationPosture::Prone
                                                                           : state.posture) &&
                (movement == Types::AnimationMovement::Run    ? running
                 : movement == Types::AnimationMovement::Walk ? walking || running
                                                              : !running && !walking);
            (suits ? suited : others).push_back(name);
        }
        // (the body's own kind first, then the campaign's AI; among them the plain ones first: a run's own, the AI's
        // and the players' deaths; a special weapon's, an explosion's, a level's scripted ones last)
        const auto kind = [&](const std::string& name) {
            const auto entry = catalog.Find(name);
            return !entry || entry->family == family ? 0 : 1;
        };
        const auto rank = [](const std::string& name) {
            const auto has = [&](std::string_view part) { return name.find(part) != std::string::npos; };
            if (std::ranges::any_of(
                    std::array<std::string_view, 13>{"crossbow", "explode", "explosion", "arrow", "shotgun", "gib",
                                                     "flame", "melee", "gas", "balcony", "rebirth", "b03", "_v2"},
                    has) ||
                name.starts_with("ch_"))
                return 2;
            return has("run_death") || name.starts_with("ai_death_") || name.starts_with("death_") ||
                           name.starts_with("pb_")
                       ? 0
                       : 1;
        };
        for (auto* list : {&suited, &others})
            std::ranges::stable_sort(*list, {},
                                     [&](const std::string& name) { return std::pair(kind(name), rank(name)); });
        // (its own first, where it suits)
        if (const auto own = GetDeathAnimation(&actor); !own.empty())
            if (const auto at = std::ranges::find(suited, own); at != suited.end())
                std::rotate(suited.begin(), at, at + 1);
        candidates.suited.assign(suited.begin(), suited.begin() + std::min(suited.size(), most));
        candidates.others.assign(others.begin(), others.begin() + std::min(others.size(), most));
        return candidates;
    }

    Types::Actor MakeBeside(const Types::Actor& actor, float left)
    {
        auto copy = actor;
        copy.id = 0;
        if (copy.path.size() >= 2)
        {
            copy.path = ActorLogic::OffsetPath(copy.path, left);
            copy.position = copy.path.front();
        }
        else
        {
            const auto yaw = glm::radians(copy.yaw);
            const auto side = glm::vec3(-std::sin(yaw), std::cos(yaw), 0.0f) * left;
            copy.position += side;
            for (auto& point : copy.path)
                point += side;
        }
        for (auto& key : copy.keys)
            key.position += glm::vec3(-std::sin(glm::radians(key.yaw)), std::cos(glm::radians(key.yaw)), 0.0f) * left;
        return copy;
    }

    void RunWith(Types::Actor& actor, const Types::Actor& leader, float left)
    {
        const auto timing = GetPathTiming(leader);
        if (!timing.start || !timing.arrival || leader.path.size() < 2)
            return;
        // its path beside the leader's, standing at its start until the leader sets off
        actor.movement = Types::ActorMovement::Path;
        actor.path = ActorLogic::OffsetPath(leader.path, left);
        actor.position = actor.path.front();
        actor.yaw = leader.yaw;
        actor.locomotion = Types::ActorLocomotion::Auto;
        actor.stretches.clear();
        actor.looks.clear();
        const auto idle = GetIdleAnimation(actor);
        const auto walking = FindWalking(actor, true);
        if (walking.walk.empty())
            return;
        const auto from = leader.clips.empty() ? *timing.start : std::min(leader.clips.front().start, *timing.start);
        actor.clips = {{.animation = idle, .start = from, .length = std::max(*timing.start - from, 1u), .moves = false},
                       {.animation = walking.walk, .start = *timing.start, .length = 1000},
                       {.animation = idle, .start = *timing.start + 1000, .length = 60000, .moves = false}};
        if (actor.clips.front().length <= 1)
            actor.clips.erase(actor.clips.begin());
        actor.events.clear();
        ActorLogic::Normalize(actor);
        // the gait that gets it there with the leader at the pace nearest 100%
        const auto body = GetBody(actor);
        std::optional<std::pair<Types::Actor, float>> best;
        for (const auto& preset : ActorLogic::GetGaits(body))
        {
            if (preset.stance != Types::ActorStance::Stand)
                continue;
            auto trial = actor;
            const auto gait = preset.gait == ActorLogic::Gait::Sprint ? Types::ActorGait::Sprint
                              : preset.gait == ActorLogic::Gait::Run  ? Types::ActorGait::Run
                                                                      : Types::ActorGait::Walk;
            trial.stretches.assign(trial.path.size(), {.gait = gait});
            trial.pace = 1.0f;
            ActorLogic::ResolveStretches(trial, body, GetAnyAnimation());
            FitOnlyWalk(trial);
            const auto arrival = SolveArrival(trial, *timing.arrival);
            if (!arrival)
                continue;
            const auto off = std::abs(std::log(arrival->pace));
            if (!best || off < best->second)
            {
                SetPace(trial, std::clamp(arrival->pace, ActorLogic::MIN_PACE, ActorLogic::MAX_PACE));
                FitOnlyWalk(trial);
                best = std::pair(std::move(trial), off);
            }
        }
        if (!best)
            return;
        actor = std::move(best->first);
        // (there sooner even at the slowest pace its feet keep to: it stands at its start the difference, worked out
        // again as its plan with the wait changes)
        for (int pass = 0; pass < 4; ++pass)
        {
            const auto own = ActorLogic::GetPathTiming(actor, GetAnimationLookup());
            if (!own.arrival || std::abs(static_cast<int>(*own.arrival) - static_cast<int>(*timing.arrival)) < 30)
                break;
            const auto wait = static_cast<int>(actor.stretches.front().wait) + static_cast<int>(*timing.arrival) -
                              static_cast<int>(*own.arrival);
            if (wait <= 0)
                break;
            actor.stretches.front().wait = static_cast<std::uint32_t>(wait);
            FitOnlyWalk(actor);
        }
    }

    bool ConvertOldWalks(Types::Actor& actor)
    {
        const auto animations = GetAnimationLookup();
        const auto was = ActorLogic::GetPathTiming(actor, animations).arrival;
        if (!ActorLogic::ConvertPickedWalks(actor, GetBody(actor), animations, GetAnyAnimation()))
            return false;
        FitOnlyWalk(actor);
        if (const auto arrival = was ? SolveArrival(actor, *was) : std::nullopt;
            arrival && arrival->pace >= ActorLogic::MIN_PACE && arrival->pace <= ActorLogic::MAX_PACE)
        {
            SetPace(actor, arrival->pace);
            FitOnlyWalk(actor);
        }
        return true;
    }
}  // namespace IWXMVM::Components::Actors
