#pragma once
#include "Types/FrozenSpan.hpp"

namespace IWXMVM::Components
{
    namespace Playback
    {
        static constexpr std::array TIMESCALE_STEPS{
            0.001f, 0.005f, 0.01f,  0.025f, 0.05f, 0.1f,  0.125f, 0.2f,
            0.25f, 0.333f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f,
            2.0f, 5.0f, 10.0f, 20.0f, 50.0f
        };
        constexpr int32_t REWIND_DEADZONE = 250;
        void TogglePaused();
        bool IsPaused();

        // The timeline's tick: the editor's time, which the campath, Look keys, markers and the capture range are
        // in. With bullet time spans it runs on in a span while the demo holds, so it is
        // later than the demo's by the spans before it; without, it is the demo's.
        uint32_t GetTimelineTick();
        void SetTimelineTick(uint32_t tick);
        // the demo's tick at the timeline's (its kills and players are in it)
        uint32_t GetWorldTick();
        // the actors' tick at the timeline's: the demo's, but through spans that let actors move; while Freeze time is
        // on, where it froze (actors, killstreaks and their effects hold with the demo while the timeline runs on for
        // the camera)
        uint32_t GetActorTick();
        // the timeline's tick of a demo's tick (a kill's) or an actor's (a clip's start)
        uint32_t ToTimelineTick(uint32_t worldTick);
        uint32_t ActorToTimelineTick(uint32_t actorTick);
        uint32_t TimelineToActorTick(uint32_t timelineTick);
        // an actor's tick moved by so many ticks of the timeline (a clip dragged across a span)
        uint32_t ShiftActorTick(uint32_t actorTick, std::int64_t timelineTicks);
        // where the timeline ends: the demo's end and the spans' lengths
        uint32_t GetTimelineEnd();

        // the bullet time spans, sorted (see FrozenSpans); any thread
        std::shared_ptr<const std::vector<Types::FrozenSpan>> GetFrozenSpans();
        // others in their place (BulletTime does it, moving the editor's ticks along)
        void SetFrozenSpans(std::vector<Types::FrozenSpan> spans);
        // the span the timeline is in now
        std::optional<std::size_t> GetCurrentFrozenSpan();

        void ToggleFrozenTick();
        // the timeline's tick, and while Freeze time is on where it froze too: actors are posed at the tick exactly
        // while the demo stays where it froze (the harness's seek; the timeline's own seeks move the playhead alone)
        void MoveFrozenTick(uint32_t tick);
        std::optional<uint32_t> GetFrozenTick();
        bool IsGameFrozen();

        void SkipForward(std::int32_t ticks);
        void SkipDemoForward(std::int32_t ticks);
        void SetTickDelta(std::int32_t value, bool ignoreDeadzone = false);
        // goes to a timeline tick: the demo to the tick it is at there (skipping or rewinding; in a span nothing more
        // than the timeline), a small step back past the rewind's dead zone only with ignoreDeadzone
        void SeekTimeline(std::int64_t tick, bool ignoreDeadzone = true);

        void HandleImportedFrozenTickLogic(std::optional<std::uint32_t> frozenTick);

        std::int32_t CalculatePlaybackDelta(std::int32_t gameMsec);
    } // namespace Playback
}  // namespace IWXMVM::Components
