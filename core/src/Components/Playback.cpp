#include "StdInclude.hpp"
#include "Playback.hpp"

#include "Mod.hpp"
#include "FrozenSpans.hpp"
#include "Rewinding.hpp"

namespace IWXMVM::Components::Playback
{
    bool isPlaybackPaused = false;
    uint32_t timelineTick = 0;
    std::optional<uint32_t> frozenTick = std::nullopt;

    // The bullet time spans, and while the timeline is in one, its tick there with the demo's tick it holds at (which
    // the timeline's can't be told from: the demo stands at it for the whole span). Set by the UI (render thread) and
    // the game's frames (main thread).
    std::mutex spansMutex;
    std::shared_ptr<const std::vector<Types::FrozenSpan>> frozenSpans =
        std::make_shared<const std::vector<Types::FrozenSpan>>();
    struct Held
    {
        uint32_t timeline = 0;
        uint32_t world = 0;
    };
    std::optional<Held> held;

    void TogglePaused()
    {
        isPlaybackPaused = !isPlaybackPaused;
    }

    bool IsPaused()
    {
        return isPlaybackPaused;
    }

    std::shared_ptr<const std::vector<Types::FrozenSpan>> GetFrozenSpans()
    {
        std::lock_guard lock(spansMutex);
        return frozenSpans;
    }

    void SetFrozenSpans(std::vector<Types::FrozenSpan> spans)
    {
        std::lock_guard lock(spansMutex);
        frozenSpans = std::make_shared<const std::vector<Types::FrozenSpan>>(std::move(spans));
    }

    void SetHeld(std::optional<Held> hold)
    {
        std::lock_guard lock(spansMutex);
        held = hold;
    }

    // the span a timeline tick is in, with the demo's tick there
    std::optional<Held> FindHeld(const std::vector<Types::FrozenSpan>& spans, std::int64_t tick)
    {
        const auto at = static_cast<std::uint32_t>(std::max<std::int64_t>(tick, 0));
        if (!FrozenSpans::FindAt(spans, at))
            return std::nullopt;
        return Held{at, FrozenSpans::ToWorld(spans, at)};
    }

    uint32_t GetTimelineTick()
    {
        if (!frozenTick.has_value())
        {
            const auto world = Mod::GetGameInterface()->GetDemoInfo().gameTick;
            std::optional<Held> hold;
            std::shared_ptr<const std::vector<Types::FrozenSpan>> spans;
            {
                std::lock_guard lock(spansMutex);
                hold = held;
                spans = frozenSpans;
            }
            // (a hold left over from spans changed since is dropped; what this thread worked out is returned, as
            // another thread may set timelineTick meanwhile from what it read a moment before)
            const auto tick = hold && hold->world == world && FindHeld(*spans, hold->timeline)
                                  ? hold->timeline
                                  : FrozenSpans::ToTimeline(*spans, world);
            timelineTick = tick;
            return tick;
        }

        return timelineTick;
	}

    void SetTimelineTick(uint32_t tick)
    {
        timelineTick = tick;
    }

    uint32_t GetWorldTick()
    {
        return FrozenSpans::ToWorld(*GetFrozenSpans(), GetTimelineTick());
    }

    uint32_t GetActorTick()
    {
        const auto frozen = frozenTick;
        return FrozenSpans::ToActors(*GetFrozenSpans(), GetTimelineTick(), frozen);
    }

    uint32_t ToTimelineTick(uint32_t worldTick)
    {
        return FrozenSpans::ToTimeline(*GetFrozenSpans(), worldTick);
    }

    uint32_t ActorToTimelineTick(uint32_t actorTick)
    {
        return FrozenSpans::ToTimeline(*GetFrozenSpans(), actorTick, true);
    }

    uint32_t TimelineToActorTick(uint32_t tick)
    {
        return FrozenSpans::ToWorld(*GetFrozenSpans(), tick, true);
    }

    uint32_t ShiftActorTick(uint32_t actorTick, std::int64_t timelineTicks)
    {
        const auto spans = GetFrozenSpans();
        const auto moved = static_cast<std::int64_t>(FrozenSpans::ToTimeline(*spans, actorTick, true)) + timelineTicks;
        return FrozenSpans::ToWorld(*spans, static_cast<uint32_t>(std::clamp<std::int64_t>(moved, 0, 0xFFFFFFFFll)),
                                    true);
    }

    uint32_t GetTimelineEnd()
    {
        return ToTimelineTick(Mod::GetGameInterface()->GetDemoInfo().endTick);
    }

    std::optional<std::size_t> GetCurrentFrozenSpan()
    {
        return FrozenSpans::FindAt(*GetFrozenSpans(), GetTimelineTick());
    }

    std::optional<uint32_t> GetFrozenTick()
    {
        return frozenTick;
    }

    bool IsGameFrozen()
    {
        return frozenTick.has_value();
    }

    void ToggleFrozenTick()
    {
        if (frozenTick.has_value())
        {
            // back where it was frozen: in a span, at its place in it
            SetHeld(FindHeld(*GetFrozenSpans(), *frozenTick));
            frozenTick.reset();
        }
        else
        {
            frozenTick.emplace(GetTimelineTick());
        }
    }

    void MoveFrozenTick(uint32_t tick)
    {
        timelineTick = tick;
        if (frozenTick.has_value())
            frozenTick = tick;
    }

    void SkipForward(std::int32_t ticks)
    {
        if (frozenTick.has_value())
        {
            timelineTick += ticks;
            return;
        }

        SeekTimeline(static_cast<std::int64_t>(GetTimelineTick()) + ticks, true);
    }

    void SkipDemoForward(std::int32_t ticks)
    {
        auto addresses = Mod::GetGameInterface()->GetPlaybackDataAddresses();
        auto realtime = reinterpret_cast<int32_t*>(addresses.cls.realtime);
        *realtime = *realtime + ticks;
        LOG_DEBUG("Skipping forward {} ticks, realtime: {}", ticks, *realtime);
    }

    void SeekTimeline(std::int64_t tick, bool ignoreDeadzone)
    {
        if (frozenTick.has_value())
        {
            timelineTick = static_cast<uint32_t>(std::max<std::int64_t>(tick, 0));
            return;
        }

        const auto spans = GetFrozenSpans();
        const auto target = static_cast<uint32_t>(std::max<std::int64_t>(tick, 0));
        const auto world = static_cast<std::int64_t>(Mod::GetGameInterface()->GetDemoInfo().gameTick);
        const auto targetWorld = static_cast<std::int64_t>(FrozenSpans::ToWorld(*spans, target));
        SetHeld(FindHeld(*spans, target));
        const auto delta = targetWorld - world;
        if (delta > 0)
            SkipDemoForward(static_cast<std::int32_t>(delta));
        else if (delta < -REWIND_DEADZONE || (delta < 0 && ignoreDeadzone))
            Rewinding::RewindBy(static_cast<std::int32_t>(delta));
    }

    void SetTickDelta(int32_t value, bool ignoreDeadzone)
    {
        if (frozenTick.has_value())
        {
            if (value > 0)
                SkipForward(value);
            else if ((value < -REWIND_DEADZONE) || (value < 0 && ignoreDeadzone))
                Rewinding::RewindBy(value);
            return;
        }

        if (value != 0)
            SeekTimeline(static_cast<std::int64_t>(GetTimelineTick()) + value, value > 0 || ignoreDeadzone);
    }

    void HandleImportedFrozenTickLogic(std::optional<std::uint32_t> importedFrozenTick)
    {
        if (frozenTick.has_value())
        {
            // toggle frozen tick off so we can rewind the actual state of the game
            frozenTick.reset();
        }

        if (!importedFrozenTick.has_value())
        {
            // since no frozen tick is set in the imported file, we're done here
            return;
        }

        // set actual tick
        Playback::SetTickDelta(importedFrozenTick.value() - GetTimelineTick(), true);

        // (re)enable frozen tick with specified value
        frozenTick.emplace(importedFrozenTick.value());

        // update current (frozen) tick
        SetTimelineTick(importedFrozenTick.value());

        if (!Playback::IsPaused())
        {
            Playback::TogglePaused();
        }
    }

    void GeneratePattern(auto& pattern, float fps, float timescale)
    {
        // SV_Frame is called once per frame, so we calculate the number of calls it takes to advance 1000 ms
        // by using the following formula: fps / timescale

        // example: 500 fps, 0.01 timescale
        // 500 / 0.01 = 50'000
        // we need to generate a pattern where 1000 * 1ms and 49000 * 0ms are interleaved, and fit this into an array of
        // size 1000

        const std::size_t callCountPerSecond =
            static_cast<std::size_t>(std::clamp(fps / timescale, 1000.0f, 1'000'000.0f));

        const std::size_t onesCount = static_cast<size_t>(pattern.size() / (callCountPerSecond / static_cast<float>(pattern.size())));
        const std::size_t zerosCount = pattern.size() - onesCount;

        for (std::size_t i = 0, zeros = 1, ones = 1; i < pattern.size(); ++i)
        {
            if (ones * zerosCount < zeros * onesCount)
            {
                ++ones;
                pattern[i] = 1;
            }
            else
            {
                ++zeros;
                pattern[i] = 0;
            }
        }

        assert(std::accumulate(pattern.begin(), pattern.end(), 0) > 0);
    }

    std::int32_t CalculatePlaybackDeltaInternal(std::int32_t gameMsec)
    {
        // check if we need to skip forward for exact rewinding
        if (Components::Rewinding::CheckSkipForward())
        {
            return 0;
        }

        auto& captureManager = Components::CaptureManager::Get();
        if (captureManager.IsCapturing())
        {
            return captureManager.OnGameFrame();
        }

        // workaround for low timescales that would take a couple of seconds to trigger the rewind process to begin
        if (Components::Rewinding::IsRewinding())
        {
            return 50;
        }

        // always return 0 msec when pausing demo playback
        if (IsPaused() && Mod::GetGameInterface()->IsPlayingDemo())
        {
            return 0;
        }

        const std::optional<Types::Dvar> timescale = Mod::GetGameInterface()->GetDvar("timescale");

        // we can use the original msec value when its value is greater than 1, and/or when timescale is equal or
        // greater than 1.0
        if (gameMsec > 1 || !timescale.has_value() || timescale.value().value->floating_point >= 1.0f)
            return gameMsec;

        const std::optional<Types::Dvar> com_maxfps = Mod::GetGameInterface()->GetDvar("com_maxfps");
        if (!com_maxfps.has_value())
            return gameMsec;

        static float lastTimeScale = timescale.value().value->floating_point;
        static std::int32_t lastMaxFps = com_maxfps.value().value->int32;
        static std::array<std::uint8_t, 1000> pattern{};
        static std::size_t patternIndex = 0;

        // below we're going to generate a pattern of interleaved 0s and 1s based on (imgui) frame times
        // we generate a new pattern each second, or whenever timescale or com_maxfps values changes

        if (lastTimeScale != timescale.value().value->floating_point || lastMaxFps != com_maxfps.value().value->int32)
        {
            // this branch ensures that any change to the timescale or max fps immediately changes the pattern
            float frameRate;

            if (lastMaxFps >
                com_maxfps.value().value->int32)  // max fps was decreased, imgui fps is potentially too high
                frameRate = std::min(ImGui::GetIO().Framerate, static_cast<float>(com_maxfps.value().value->int32));
            else if (lastMaxFps <
                     com_maxfps.value().value->int32)  // max fps was increased, imgui fps is potentially too low
                frameRate = std::max(ImGui::GetIO().Framerate, static_cast<float>(com_maxfps.value().value->int32));
            else
            {
                assert(lastTimeScale != timescale.value().value->floating_point);
                frameRate = ImGui::GetIO().Framerate;
            }

            lastTimeScale = timescale.value().value->floating_point;
            lastMaxFps = com_maxfps.value().value->int32;
            patternIndex = 0;

            GeneratePattern(pattern, frameRate, timescale.value().value->floating_point);
        }
        else if (patternIndex % 1000 == 0)
            GeneratePattern(pattern, ImGui::GetIO().Framerate, timescale.value().value->floating_point);

        // advance (1ms) or pause(0ms) based on the pattern
        return pattern[patternIndex++ % 1000];
    }

    std::int32_t CalculatePlaybackDelta(std::int32_t gameMsec)
    {
        auto delta = CalculatePlaybackDeltaInternal(gameMsec);

        // (a demo left paused or frozen must not hold the next game: with no time, its client waits at "Awaiting
        // challenge" for ever)
        if (!Mod::GetGameInterface()->IsPlayingDemo())
            return delta;

        if (IsGameFrozen() && !Components::Rewinding::IsRewinding())
        {
            const auto timelineTick = GetTimelineTick();
            const auto endTick = GetTimelineEnd();
            SetTimelineTick((timelineTick + delta >= endTick) ? endTick - 1 : timelineTick + delta);

            return 0;
        }

        // With bullet time the timeline moves on by what the game would play, and the demo by what the spans leave of
        // it: it lands on a span's tick exactly and holds there while the timeline goes through the span
        const auto spans = GetFrozenSpans();
        if (spans->empty() || Components::Rewinding::IsRewinding() || delta <= 0)
            return delta;
        const auto next = GetTimelineTick() + static_cast<uint32_t>(delta);
        const auto world = Mod::GetGameInterface()->GetDemoInfo().gameTick;
        const auto nextWorld = FrozenSpans::ToWorld(*spans, next);
        SetHeld(FindHeld(*spans, next));
        return nextWorld > world ? static_cast<std::int32_t>(nextWorld - world) : 0;
    }
}  // namespace IWXMVM::Components::Playback
