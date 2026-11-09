#include "StdInclude.hpp"
#include "KillFinder.hpp"

#include "Mod.hpp"
#include "Components/Playback.hpp"
#include "Components/CaptureManager.hpp"

namespace IWXMVM::Components::KillFinder
{
    Settings settings;

    std::shared_ptr<const Types::DemoEvents> analyzedEvents;
    std::vector<Entry> entries;

    Settings& GetSettings()
    {
        return settings;
    }

    bool Entry::IsHighlight() const
    {
        const auto& kill = GetKill(*this);
        return seriesCount > 1 || collateral || kill.attackerInAir || kill.distance >= LONGSHOT_DISTANCE ||
               kill.method == Types::KillMethod::Melee;
    }

    void Analyze(const Types::DemoEvents& events)
    {
        entries.clear();
        entries.reserve(events.kills.size());

        std::map<std::int32_t, std::int32_t> streaks;
        for (std::size_t i = 0; i < events.kills.size(); i++)
        {
            const auto& kill = events.kills[i];
            Entry entry{};
            entry.index = i;
            entry.byPov = kill.attacker >= 0 && kill.attacker == kill.povClient && kill.attacker != kill.victim;
            entry.ofPov = kill.victim == kill.povClient;
            entry.seriesCount = 1;
            entry.seriesNumber = 1;
            entry.seriesStart = entry.seriesEnd = kill.tick;

            if (kill.attacker >= 0 && kill.attacker != kill.victim && !kill.teamKill)
                entry.streak = ++streaks[kill.attacker];
            streaks[kill.victim] = 0;

            entries.push_back(entry);
        }

        // series and collaterals, per attacker
        std::map<std::int32_t, std::vector<std::size_t>> byAttacker;
        for (std::size_t i = 0; i < entries.size(); i++)
        {
            const auto& kill = events.kills[i];
            if (kill.attacker >= 0 && kill.attacker != kill.victim && !kill.teamKill)
                byAttacker[kill.attacker].push_back(i);
        }

        for (const auto& [attacker, kills] : byAttacker)
        {
            for (std::size_t start = 0; start < kills.size();)
            {
                auto end = start + 1;
                while (end < kills.size() &&
                       events.kills[kills[end]].tick - events.kills[kills[end - 1]].tick <= SERIES_GAP)
                    end++;

                for (auto i = start; i < end; i++)
                {
                    auto& entry = entries[kills[i]];
                    entry.seriesCount = static_cast<std::int32_t>(end - start);
                    entry.seriesNumber = static_cast<std::int32_t>(i - start + 1);
                    entry.seriesStart = events.kills[kills[start]].tick;
                    entry.seriesEnd = events.kills[kills[end - 1]].tick;

                    // the obituaries of one shot come in the same snapshot
                    const auto tick = events.kills[kills[i]].tick;
                    entry.collateral = (i > start && events.kills[kills[i - 1]].tick == tick) ||
                                       (i + 1 < end && events.kills[kills[i + 1]].tick == tick);
                }
                start = end;
            }
        }
    }

    std::shared_ptr<const Types::DemoEvents> GetEvents()
    {
        if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_DemoEvents))
            return nullptr;

        auto events = Mod::GetGameInterface()->GetDemoEvents();
        if (events != analyzedEvents)
        {
            analyzedEvents = events;
            entries.clear();
            if (events)
                Analyze(*events);
        }
        return events;
    }

    const std::vector<Entry>& GetEntries()
    {
        GetEvents();
        return entries;
    }

    const Types::DemoKill& GetKill(const Entry& entry)
    {
        return analyzedEvents->kills[entry.index];
    }

    std::string FormatTick(std::uint32_t tick)
    {
        const auto seconds = tick / 1000;
        return std::format("{}:{:02}", seconds / 60, seconds % 60);
    }

    bool IsShown(const Entry& entry)
    {
        if (settings.onlyHighlights && !entry.IsHighlight())
            return false;
        if (entry.byPov)
            return settings.showOwnKills;
        if (entry.ofPov)
            return settings.showOwnDeaths;
        return settings.showOtherKills;
    }

    ImVec4 GetColor(const Entry& entry)
    {
        if (entry.ofPov)
            return ImVec4(0.92f, 0.30f, 0.30f, 1.0f);
        if (!entry.byPov)
            return ImVec4(0.62f, 0.62f, 0.62f, 1.0f);
        if (entry.seriesCount > 1 || entry.collateral)
            return ImVec4(1.0f, 0.78f, 0.22f, 1.0f);
        return ImVec4(0.30f, 0.85f, 0.45f, 1.0f);
    }

    std::string Describe(const Entry& entry)
    {
        const auto& kill = GetKill(entry);
        if (kill.attacker < 0 || kill.attacker == kill.victim)
            return analyzedEvents->GetPlayerName(kill.victim);
        return std::format("{} > {}", analyzedEvents->GetPlayerName(kill.attacker),
                           analyzedEvents->GetPlayerName(kill.victim));
    }

    std::string GetDetails(const Entry& entry)
    {
        const auto& kill = GetKill(entry);
        std::vector<std::string> parts;

        switch (kill.method)
        {
            case Types::KillMethod::Melee:
                parts.push_back("Knife");
                break;
            case Types::KillMethod::Falling:
                parts.push_back("Fell");
                break;
            case Types::KillMethod::Crush:
                parts.push_back("Crushed");
                break;
            case Types::KillMethod::Suicide:
                parts.push_back(kill.weapon.empty() ? "Suicide" : std::format("Suicide, {}", kill.weapon));
                break;
            default:
                if (!kill.weapon.empty())
                    parts.push_back(kill.weapon);
                if (kill.method == Types::KillMethod::Headshot)
                    parts.push_back("Headshot");
                if (kill.method == Types::KillMethod::Impact)
                    parts.push_back("Impact");
                break;
        }

        if (kill.distance > 0.0f)
            parts.push_back(std::format("{:.0f} m", kill.distance / UNITS_PER_METER));
        if (kill.attackerInAir)
            parts.push_back("In the air");
        if (entry.collateral)
            parts.push_back("Collateral");
        if (entry.seriesCount > 1)
            parts.push_back(std::format("{} of {} in {:.1f} s", entry.seriesNumber, entry.seriesCount,
                                        (entry.seriesEnd - entry.seriesStart) / 1000.0f));
        if (entry.streak >= 5 && entry.streak % 5 == 0)
            parts.push_back(std::format("{} streak", entry.streak));
        if (kill.teamKill)
            parts.push_back("Team kill");

        std::string details;
        for (const auto& part : parts)
            details += (details.empty() ? "" : " | ") + part;
        return details;
    }

    std::uint32_t PrerollTicks()
    {
        return static_cast<std::uint32_t>(settings.preroll * 1000.0f);
    }

    std::atomic<std::uint32_t> jumps = 0;

    std::uint32_t GetJumps()
    {
        return jumps;
    }

    void JumpTo(std::uint32_t tick)
    {
        ++jumps;
        const auto target = tick > PrerollTicks() ? tick - PrerollTicks() : 0;
        // (a kill is at a tick of the demo, which bullet time puts later on the timeline)
        Playback::SeekTimeline(Playback::ToTimelineTick(target));
    }

    void JumpToNext()
    {
        const auto current = Playback::GetWorldTick();
        for (const auto& entry : GetEntries())
        {
            const auto tick = GetKill(entry).tick;
            // a kill whose preroll has started still counts as the current one
            if (IsShown(entry) && tick > current + PrerollTicks() + 100)
            {
                JumpTo(tick);
                return;
            }
        }
    }

    void JumpToPrevious()
    {
        const auto current = Playback::GetWorldTick();
        const auto& all = GetEntries();
        for (auto it = all.rbegin(); it != all.rend(); ++it)
        {
            const auto tick = GetKill(*it).tick;
            // pressing again right after a jump goes on to the kill before
            if (IsShown(*it) && tick + 500 < current + PrerollTicks())
            {
                JumpTo(tick);
                return;
            }
        }
    }

    void SetCaptureRange(const Entry& entry)
    {
        auto& capture = CaptureManager::Get().GetCaptureSettings();
        capture.startTick = entry.seriesStart > PrerollTicks() ? entry.seriesStart - PrerollTicks() : 0;
        capture.endTick = entry.seriesEnd + static_cast<std::uint32_t>(settings.postroll * 1000.0f);
    }
}  // namespace IWXMVM::Components::KillFinder
