#pragma once
#include "Types/DemoEvents.hpp"

namespace IWXMVM::Components::KillFinder
{
    // A kill of the demo with what makes it stand out
    struct Entry
    {
        std::size_t index;            // into DemoEvents::kills
        bool byPov;                   // made by the player the demo followed
        bool ofPov;                   // the player the demo followed died
        std::int32_t seriesCount;     // kills in the quick series this kill belongs to, 1 when alone
        std::int32_t seriesNumber;    // which kill of the series this is, from 1
        std::uint32_t seriesStart;    // tick of the series' first and last kill
        std::uint32_t seriesEnd;
        bool collateral;              // more than one kill by the attacker at the same moment
        std::int32_t streak;          // kills by the attacker since they last died, this one included

        bool IsHighlight() const;
    };

    struct Settings
    {
        bool showOwnKills = true;
        bool showOwnDeaths = true;
        bool showOtherKills = false;
        bool onlyHighlights = false;
        float preroll = 3.0f;         // seconds a jump lands before a kill
        float postroll = 2.0f;        // seconds after a kill that a capture range ends
    };

    // kills closer together than this belong to one series
    constexpr std::uint32_t SERIES_GAP = 2000;
    // in game units, about 40 m
    constexpr float LONGSHOT_DISTANCE = 1575.0f;
    constexpr float UNITS_PER_METER = 39.37f;

    Settings& GetSettings();

    // null while the demo is being read or when the game can't find kills
    std::shared_ptr<const Types::DemoEvents> GetEvents();
    const std::vector<Entry>& GetEntries();
    bool IsShown(const Entry& entry);

    const Types::DemoKill& GetKill(const Entry& entry);
    std::string FormatTick(std::uint32_t tick);  // m:ss on the demo's timeline
    ImVec4 GetColor(const Entry& entry);
    std::string Describe(const Entry& entry);    // "attacker > victim"
    std::string GetDetails(const Entry& entry);  // weapon, distance and what stands out

    void JumpTo(std::uint32_t tick);  // to the preroll before the tick
    // how many jumps to kills there were since the mod started (the first-run card's step)
    std::uint32_t GetJumps();
    void JumpToNext();
    void JumpToPrevious();
    void SetCaptureRange(const Entry& entry);  // around the kill, or the whole series it belongs to
}  // namespace IWXMVM::Components::KillFinder
