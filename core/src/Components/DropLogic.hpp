#pragma once
#include "Types/Actor.hpp"
#include "Components/KillstreakLogic.hpp"

#include <functional>

namespace IWXMVM::Components::DropLogic
{
    // What a killstreak's Fire drops: the Napalm Strike's bombs and the care package,
    // as their scripts have them. All of it comes from the actor and its Fire event, so a rewind or a jump gets it
    // again and the timeline's speed and Freeze time move it as they move actors; the game is only asked where the
    // ground is. No game state; tested in tests/actors-tests.

    // the most things one Fire drops: past them a Fire's shots drop nothing (each bomb is seven fires, and the game has
    // room for 1024 effects)
    inline constexpr std::uint32_t MOST_DROPS = 32;
    // how long something falls at most (from far higher than anything flies) before it counts as lost
    inline constexpr float MOST_FALL = 8.0f;

    // Where the killstreak is at a tick, as drawn
    using Place = std::function<Types::ActorPlacement(std::uint32_t)>;

    // What the game says is in the way going from one point to another, the first thing met
    struct Hit
    {
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
    };
    using Trace = std::function<std::optional<Hit>(glm::vec3, glm::vec3)>;

    // A thing as it leaves the killstreak
    struct Release
    {
        std::uint32_t tick = 0;
        glm::vec3 point{0.0f};
        glm::vec3 velocity{0.0f};  // units a second
        float yaw = 0.0f;
    };
    // where it is after some seconds in the air (the game's gravity)
    glm::vec3 GetPoint(const Release& release, float seconds);

    struct Landing
    {
        float seconds = 0.0f;
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
    };
    // where its fall meets something, none within MOST_FALL
    std::optional<Landing> FindLanding(const Release& release, const Trace& trace);

    // An effect of the game a drop plays in the world, at a tick: as the scene's effects (ActorLogic::GetEffectPlays),
    // a playhead that lands inside its life starts it as far back as it has played
    struct Play
    {
        std::string_view effect;
        std::uint32_t tick = 0;
        std::uint32_t life = 0;  // ticks it goes on being seen for
        glm::vec3 point{0.0f};
        glm::vec3 angles{0.0f};  // pitch, yaw and roll, the game's way
    };

    // One thing a Fire drops
    struct Drop
    {
        std::uint32_t shot = 0;  // of its event
        std::string_view model;
        Release release;
        std::optional<Landing> landing;
        // the tick the model is gone (the game swaps the napalm bomb for nothing half a second before it lands); a care
        // package stays
        std::uint32_t goneAt = UINT32_MAX;
        std::vector<Play> plays;
    };

    // What a Fire event of an actor drops, a shot at a time: none for one that drops nothing, or for shots while the
    // actor is hidden
    std::vector<Drop> MakeDrops(const KillstreakLogic::Info& info, const Types::Actor& actor,
                                const Types::ActorEvent& event, const Place& place, const Trace& trace);

    // The Mortar Team's shells (_mortar.gsc): a Fire event's tick is when
    // the first lands; each of its points takes the event's shots (the game's two) its interval apart, the next point
    // MORTAR_POINT_APART later. A shell is fired MORTAR_PITCH degrees up from MORTAR_DISTANCE back along the actor's
    // yaw, as fast as lands it on the point; it is seen from MORTAR_START_RATIO along its arc (5.2 s before it lands,
    // far above the map) and bursts where it lands. A shot is a shell: shot = point * shots + its shell.
    inline constexpr float MORTAR_DISTANCE = 6000.0f;
    inline constexpr float MORTAR_PITCH = 75.0f;
    inline constexpr float MORTAR_START_RATIO = 0.3f;
    inline constexpr std::uint32_t MORTAR_POINT_APART = 2500;
    inline constexpr std::string_view MORTAR_MODEL = "mortar_shell";
    inline constexpr std::string_view MORTAR_BLAST = "weapon/mortar/fx_mortar_strike_dirt_mp";
    std::vector<Drop> MakeMortarDrops(const Types::Actor& actor, const Types::ActorEvent& event, const Trace& trace);
    // how long a shell is seen in the air before it lands, ms
    std::uint32_t GetMortarFlight();

    // Where a drop's model is at a tick, none when it isn't there
    struct Shown
    {
        glm::vec3 point{0.0f};
        glm::mat3 rotation{1.0f};
    };
    std::optional<Shown> GetShown(const Drop& drop, std::uint32_t tick);
}  // namespace IWXMVM::Components::DropLogic
