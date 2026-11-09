#pragma once
#include "Components/LiveActorLogic.hpp"
#include "Types/LiveActor.hpp"

namespace IWXMVM::Components::LiveActors
{
    // The actors of a live game: what is chosen for each, for the game to bring about, and
    // what it reports back. They last as long as their map does; nothing of them is saved yet.

    // adds one (its id is given here) and returns its id
    std::uint32_t Add(Types::LiveActor actor);
    void Update(const Types::LiveActor& actor);
    void Remove(std::uint32_t id);
    void Clear();

    // A place in the view, each -1 to 1 from its middle, x to the right and y up
    using ViewPoint = std::array<float, 2>;
    // Adds a copy of the one selected (or the last) at the middle of the view or a point of it, and selects it; what a
    // key, the Add button and a right-click in the view do
    std::uint32_t AddNew(std::optional<ViewPoint> at = std::nullopt);

    // Which the panel shows; ALL_ACTORS is all of them at once
    inline constexpr std::uint32_t ALL_ACTORS = 0;
    std::optional<std::uint32_t> GetSelected();
    void Select(std::uint32_t id);

    // Puts an actor at the ground where the view looks, facing the player; the game's main thread sees to it
    struct PlaceRequest
    {
        std::uint32_t id = 0;
        std::optional<ViewPoint> at;
    };
    void RequestPlace(std::uint32_t id, std::optional<ViewPoint> at = std::nullopt);
    std::vector<PlaceRequest> TakePlaceRequests();
    // Removes the actor the view looks at, else the newest; likewise
    void RequestRemoveLooked();
    bool TakeRemoveLooked();

    std::vector<Types::LiveActor> GetActors();
    std::optional<Types::LiveActor> GetActor(std::uint32_t id);
    // changes whenever the actors do
    std::uint32_t GetGeneration();

    // The skill of all of them: the game's bot settings are one set
    Types::LiveActorSkill GetSkill();
    void SetSkill(Types::LiveActorSkill skill);

    // What the game reports; only for actors there are
    void SetState(std::uint32_t id, Types::LiveActorState state);
    Types::LiveActorState GetState(std::uint32_t id);
    // how many actors the game has room for
    void SetRoom(std::int32_t room);
    std::int32_t GetRoom();
}  // namespace IWXMVM::Components::LiveActors
