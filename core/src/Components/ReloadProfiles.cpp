#include "StdInclude.hpp"
#include "ReloadProfiles.hpp"

#include "Mod.hpp"
#include "Events.hpp"
#include "Resources.hpp"
#include "Components/Actors.hpp"
#include "Components/PoseLogic.hpp"
#include "Components/UndoHistory.hpp"
#include "Configuration/Configuration.hpp"
#include "Configuration/PreferencesConfiguration.hpp"

namespace IWXMVM::Components::ReloadProfiles
{
    INCBIN_EXTERN(RELOAD_PROFILES);

    namespace
    {
        struct Key
        {
            std::string animation;
            std::string body;
            std::string weapon;

            auto operator<=>(const Key&) const = default;
        };

        std::mutex mutex;
        std::map<Key, std::shared_ptr<const Types::ReloadProfile>> profiles;
        // profiles worked out with an override that changed since, given until they are worked out again, so a
        // magazine doesn't jump onto the gun meanwhile (while a marker is dragged, every frame)
        std::map<Key, std::shared_ptr<const Types::ReloadProfile>> outdated;
        std::deque<Key> requested;
        // the catalog they are of: another has other animations, bodies or guns of the same names
        std::shared_ptr<const Types::ActorCatalog> profilesCatalog;
        std::uint32_t generation = 0;
        std::uint32_t cleared = 0;
        ReloadLogic::ProfileTable curated;
        ReloadLogic::ProfileTable overrides;  // as config.json has them, or as previewed
        // From the moment a demo starts loading until it plays nothing is worked out: the map's assets change
        // meanwhile. A demo load waits for the profile being worked out.
        bool paused = false;
        std::atomic<bool> resumePending = false;
        std::mutex workMutex;
        std::atomic<std::uint32_t> workedOut = 0;

        constexpr std::size_t MAX_REQUESTED = 64;

        // with the mutex held
        void Clear()
        {
            profiles.clear();
            outdated.clear();
            requested.clear();
            ++generation;
            ++cleared;
        }

        // with the mutex held
        std::optional<ReloadLogic::ProfileEntry> FindOverride(const std::string& animation)
        {
            const auto it = overrides.find(animation);
            return it != overrides.end() ? std::optional(it->second) : std::nullopt;
        }

        // Uses an override from now on, without saving it
        void UseOverride(const std::string& animation, const std::optional<ReloadLogic::ProfileEntry>& entry)
        {
            std::lock_guard lock(mutex);
            if (FindOverride(animation) == entry)
                return;
            if (entry)
                overrides[animation] = *entry;
            else
                overrides.erase(animation);
            for (auto it = profiles.begin(); it != profiles.end();)
            {
                if (it->first.animation != animation)
                {
                    ++it;
                    continue;
                }
                outdated[it->first] = std::move(it->second);
                it = profiles.erase(it);
            }
            ++generation;
        }

        // what config.json has
        std::optional<ReloadLogic::ProfileEntry> GetSaved(const std::string& animation)
        {
            const auto& saved = PreferencesConfiguration::Get().reloadProfiles;
            const auto it = saved.find(animation);
            return it != saved.end() ? std::optional(ReloadLogic::EntryFromJson(*it)) : std::nullopt;
        }

        // an override that says nothing is none
        std::optional<ReloadLogic::ProfileEntry> OrNone(const std::optional<ReloadLogic::ProfileEntry>& entry)
        {
            return entry && *entry != ReloadLogic::ProfileEntry{} ? entry : std::nullopt;
        }

        // with the mutex held
        void UseCatalog(const std::shared_ptr<const Types::ActorCatalog>& catalog)
        {
            if (catalog == profilesCatalog)
                return;
            profilesCatalog = catalog;
            Clear();
        }

        Types::ReloadProfile MakeProfile(const Key& key, const Types::ActorCatalog* catalog,
                                         const std::optional<ReloadLogic::ProfileEntry>& override,
                                         const std::optional<ReloadLogic::ProfileEntry>& curatedEntry)
        {
            const auto animation = catalog ? catalog->FindAnimation(key.animation) : nullptr;
            if (!animation)
                return Types::ReloadProfile{.note = "The animation isn't loaded."};
            const auto& game = Mod::GetGameInterface();
            return ReloadLogic::MakeProfile({
                .animation = key.animation,
                .death = animation->entry ? animation->entry->death
                                          : ActorLogic::IsDeathAnimation(key.animation, animation->loop,
                                                                         animation->notetracks),
                .duration = animation->duration,
                .notetracks = animation->notetracks,
                .override = override ? &*override : nullptr,
                .curated = curatedEntry ? &*curatedEntry : nullptr,
                .rig = key.weapon.empty() ? std::nullopt : game->GetWeaponRig(key.weapon),
                .bones = key.body.empty() ? ReloadLogic::BonesAt{} : GetBones(key.animation, key.body),
            });
        }
    }  // namespace

    // Not for one of 256 frames or more (the LMGs' box reloads): the decoder reads them now, but what their hands would
    // do to the box hasn't been checked, so they keep what their notetracks say.
    ReloadLogic::BonesAt GetBones(const std::string& animation, const std::string& body)
    {
        if (const auto entry = Actors::GetAnimationCatalog().Find(animation); entry && entry->frames >= 256)
            return [](float) -> std::optional<ReloadLogic::ReloadBones> { return std::nullopt; };
        return [animation, body](float time) -> std::optional<ReloadLogic::ReloadBones> {
            const auto pose = Mod::GetGameInterface()->GetAnimationPoseByName(animation, body, time);
            if (!pose)
                return std::nullopt;
            const auto bone = [&](std::string_view name) -> std::optional<glm::mat4> {
                const auto index = pose->FindBone(name);
                return index ? std::optional(PoseLogic::GetTransform(*pose, *index)) : std::nullopt;
            };
            const auto left = bone("j_wrist_le"), right = bone("j_wrist_ri"), gun = bone("tag_weapon_right");
            if (!left || !right || !gun)
                return std::nullopt;
            return ReloadLogic::ReloadBones{
                .leftWrist = *left, .rightWrist = *right, .handTag = bone("tag_inhand"), .gun = *gun};
        };
    }

    void Initialize()
    {
        try
        {
            const std::string_view text(reinterpret_cast<const char*>(RELOAD_PROFILES_data), RELOAD_PROFILES_size);
            curated = ReloadLogic::TableFromJson(nlohmann::json::parse(text, nullptr, true, true));
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("The curated reload profiles don't read: {}", e.what());
        }
        overrides = ReloadLogic::TableFromJson({{"profiles", PreferencesConfiguration::Get().reloadProfiles}});

        Events::RegisterListener(EventType::PreDemoLoad, [] {
            std::lock_guard work(workMutex);
            std::lock_guard lock(mutex);
            paused = true;
            resumePending = false;
            Clear();
        });
        Events::RegisterListener(EventType::PostDemoLoad, [] { resumePending = true; });
        Events::RegisterListener(EventType::OnFrame, [] {
            if (!resumePending || Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            std::lock_guard lock(mutex);
            paused = false;
            resumePending = false;
        });
    }

    void Work()
    {
        std::lock_guard work(workMutex);
        Key key;
        std::shared_ptr<const Types::ActorCatalog> catalog;
        std::optional<ReloadLogic::ProfileEntry> override, curatedEntry;
        std::uint32_t startedAt = 0;
        {
            std::lock_guard lock(mutex);
            if (paused || Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            // (a profile asked for again while it was worked out is there now)
            while (!requested.empty() && profiles.contains(requested.front()))
                requested.pop_front();
            if (requested.empty())
                return;
            key = std::move(requested.front());
            requested.pop_front();
            catalog = profilesCatalog;
            startedAt = cleared;
            override = FindOverride(key.animation);
            if (const auto it = curated.find(key.animation); it != curated.end())
                curatedEntry = it->second;
        }

        std::shared_ptr<const Types::ReloadProfile> profile;
        try
        {
            profile =
                std::make_shared<const Types::ReloadProfile>(MakeProfile(key, catalog.get(), override, curatedEntry));
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Couldn't work out the reload profile of {}: {}", key.animation, e.what());
            profile = std::make_shared<const Types::ReloadProfile>(Types::ReloadProfile{.note = e.what()});
        }

        ++workedOut;
        std::lock_guard lock(mutex);
        if (cleared != startedAt || catalog != profilesCatalog)
            return;
        // one worked out with an override that changed meanwhile is closer than the one before, until the next
        if (FindOverride(key.animation) == override)
        {
            outdated.erase(key);
            profiles[key] = std::move(profile);
        }
        else
            outdated[key] = std::move(profile);
    }

    std::shared_ptr<const Types::ReloadProfile> Get(const std::string& animation, const std::string& body,
                                                    const std::string& weapon)
    {
        const auto catalog = Actors::GetCatalog();
        std::lock_guard lock(mutex);
        UseCatalog(catalog);
        Key key{animation, body, weapon};
        if (const auto it = profiles.find(key); it != profiles.end())
            return it->second;
        const auto old = outdated.find(key);
        auto profile = old != outdated.end() ? old->second : nullptr;
        if (requested.size() < MAX_REQUESTED && std::ranges::find(requested, key) == requested.end())
            requested.push_back(std::move(key));
        return profile;
    }

    bool IsUpToDate(const std::string& animation, const std::string& body, const std::string& weapon)
    {
        std::lock_guard lock(mutex);
        return profiles.contains(Key{animation, body, weapon});
    }

    std::uint32_t GetGeneration()
    {
        const auto catalog = Actors::GetCatalog();
        std::lock_guard lock(mutex);
        UseCatalog(catalog);
        return generation;
    }

    std::uint32_t CountWorkedOut()
    {
        return workedOut;
    }

    ReloadLogic::ProfileLookup GetLookup(const Types::Actor& actor)
    {
        return [body = actor.models.empty() ? std::string() : actor.models.front(),
                weapon = Actors::ResolveWeapon(actor)](const Types::ActorClip& clip) {
            return Get(clip.animation, body, weapon);
        };
    }

    std::vector<ActorLogic::Reload> GetReloads(const Types::Actor& actor, std::uint32_t until)
    {
        const auto profiles = GetLookup(actor);
        return Actors::GetReloads(actor, until,
                                  [&](const Types::ActorClip& clip) -> std::optional<ActorLogic::ReloadMoments> {
                                      const auto profile = profiles(clip);
                                      return profile ? ReloadLogic::GetMoments(*profile) : std::nullopt;
                                  });
    }

    std::optional<ReloadLogic::ProfileEntry> GetOverride(const std::string& animation)
    {
        std::lock_guard lock(mutex);
        return FindOverride(animation);
    }

    void SetOverride(const std::string& animation, const std::optional<ReloadLogic::ProfileEntry>& entry)
    {
        const auto used = OrNone(entry);
        UseOverride(animation, used);
        if (GetSaved(animation) == used)
            return;
        auto& saved = PreferencesConfiguration::Get().reloadProfiles;
        if (used)
            saved[animation] = ReloadLogic::EntryToJson(*used);
        else
            saved.erase(animation);
        Configuration::Get().Write(true);
    }

    void ChangeOverride(const std::string& animation, const std::optional<ReloadLogic::ProfileEntry>& entry,
                        std::string label)
    {
        const auto before = GetSaved(animation);
        const auto after = OrNone(entry);
        SetOverride(animation, after);
        if (after != before)
            UndoHistory::Get().Push(
                std::move(label), [=] { SetOverride(animation, before); }, [=] { SetOverride(animation, after); });
    }

    void PreviewOverride(const std::string& animation, const std::optional<ReloadLogic::ProfileEntry>& entry)
    {
        UseOverride(animation, OrNone(entry));
    }
}  // namespace IWXMVM::Components::ReloadProfiles
