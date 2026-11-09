#include "StdInclude.hpp"
#include "LiveActors.hpp"

namespace IWXMVM::Components::LiveActors
{
    // The UI changes them on the render thread while the game reads them on the main thread
    std::mutex mutex;
    std::vector<Types::LiveActor> actors;
    std::map<std::uint32_t, Types::LiveActorState> states;
    std::uint32_t nextId = 1;
    std::atomic<std::uint32_t> generation = 0;
    Types::LiveActorSkill skill = Types::LiveActorSkill::Normal;
    std::optional<std::uint32_t> selected;
    std::vector<PlaceRequest> placeRequests;
    bool removeLooked = false;
    std::atomic<std::int32_t> room = 0;

    // the one a new one copies: the one selected, else the last added
    const Types::LiveActor* FindModel()
    {
        const auto chosen = selected ? std::ranges::find(actors, *selected, &Types::LiveActor::id) : actors.end();
        return chosen != actors.end() ? &*chosen : actors.empty() ? nullptr : &actors.back();
    }

    std::uint32_t Add(Types::LiveActor actor)
    {
        std::lock_guard lock(mutex);
        actor.id = nextId++;
        actors.push_back(std::move(actor));
        ++generation;
        return actors.back().id;
    }

    void Update(const Types::LiveActor& actor)
    {
        std::lock_guard lock(mutex);
        const auto it = std::ranges::find(actors, actor.id, &Types::LiveActor::id);
        if (it == actors.end() || *it == actor)
            return;
        *it = actor;
        ++generation;
    }

    std::uint32_t AddNew(std::optional<ViewPoint> at)
    {
        std::lock_guard lock(mutex);
        auto actor = LiveActorLogic::MakeNew(FindModel());
        actor.id = nextId++;
        actors.push_back(std::move(actor));
        selected = actors.back().id;
        placeRequests.push_back({actors.back().id, at});
        ++generation;
        return actors.back().id;
    }

    void Remove(std::uint32_t id)
    {
        std::lock_guard lock(mutex);
        const auto it = std::ranges::find(actors, id, &Types::LiveActor::id);
        if (it != actors.end())
        {
            // the one after it takes its place in the list
            const auto next = it + 1 != actors.end() ? std::optional((it + 1)->id)
                              : it != actors.begin() ? std::optional((it - 1)->id)
                                                     : std::nullopt;
            if (selected == id)
                selected = next;
            actors.erase(it);
            ++generation;
        }
        if (actors.size() < 2 && selected == ALL_ACTORS)
            selected = actors.empty() ? std::nullopt : std::optional(actors.front().id);
        states.erase(id);
    }

    void Clear()
    {
        std::lock_guard lock(mutex);
        selected.reset();
        placeRequests.clear();
        removeLooked = false;
        if (actors.empty() && states.empty())
            return;
        actors.clear();
        states.clear();
        ++generation;
    }

    std::optional<std::uint32_t> GetSelected()
    {
        std::lock_guard lock(mutex);
        return selected;
    }

    void Select(std::uint32_t id)
    {
        std::lock_guard lock(mutex);
        selected = id;
    }

    void RequestPlace(std::uint32_t id, std::optional<ViewPoint> at)
    {
        std::lock_guard lock(mutex);
        placeRequests.push_back({id, at});
    }

    std::vector<PlaceRequest> TakePlaceRequests()
    {
        std::lock_guard lock(mutex);
        return std::exchange(placeRequests, {});
    }

    void RequestRemoveLooked()
    {
        std::lock_guard lock(mutex);
        removeLooked = true;
    }

    bool TakeRemoveLooked()
    {
        std::lock_guard lock(mutex);
        return std::exchange(removeLooked, false);
    }

    std::vector<Types::LiveActor> GetActors()
    {
        std::lock_guard lock(mutex);
        return actors;
    }

    std::optional<Types::LiveActor> GetActor(std::uint32_t id)
    {
        std::lock_guard lock(mutex);
        const auto it = std::ranges::find(actors, id, &Types::LiveActor::id);
        return it != actors.end() ? std::optional(*it) : std::nullopt;
    }

    std::uint32_t GetGeneration()
    {
        return generation;
    }

    Types::LiveActorSkill GetSkill()
    {
        std::lock_guard lock(mutex);
        return skill;
    }

    void SetSkill(Types::LiveActorSkill newSkill)
    {
        std::lock_guard lock(mutex);
        if (skill == newSkill)
            return;
        skill = newSkill;
        ++generation;
    }

    void SetState(std::uint32_t id, Types::LiveActorState state)
    {
        std::lock_guard lock(mutex);
        if (std::ranges::find(actors, id, &Types::LiveActor::id) != actors.end())
            states[id] = std::move(state);
    }

    void SetRoom(std::int32_t newRoom)
    {
        room = newRoom;
    }

    std::int32_t GetRoom()
    {
        return room;
    }

    Types::LiveActorState GetState(std::uint32_t id)
    {
        std::lock_guard lock(mutex);
        const auto it = states.find(id);
        return it != states.end() ? it->second : Types::LiveActorState{};
    }
}  // namespace IWXMVM::Components::LiveActors
