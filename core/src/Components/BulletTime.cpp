#include "StdInclude.hpp"
#include "BulletTime.hpp"

#include "CaptureManager.hpp"
#include "FrozenSpans.hpp"
#include "KeyframeManager.hpp"
#include "KeyframeSerializer.hpp"
#include "Markers.hpp"
#include "Playback.hpp"
#include "UndoHistory.hpp"
#include "UI/TimelineMath.hpp"

namespace IWXMVM::Components::BulletTime
{
    namespace
    {
        // what a span's edit changes, to put back
        struct State
        {
            std::vector<Types::FrozenSpan> spans;
            std::map<Types::KeyframeableProperty, std::vector<Types::Keyframe>> keyframes;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> markers;  // id and tick
            std::uint32_t captureStart = 0;
            std::uint32_t captureEnd = 0;
        };

        State Take()
        {
            State state;
            state.spans = *Playback::GetFrozenSpans();
            state.keyframes = KeyframeManager::Get().GetKeyframes();
            for (const auto& marker : Markers::GetAll())
                state.markers.emplace_back(marker.id, marker.tick);
            const auto& settings = CaptureManager::Get().GetCaptureSettings();
            state.captureStart = static_cast<std::uint32_t>(settings.startTick);
            state.captureEnd = static_cast<std::uint32_t>(settings.endTick);
            return state;
        }

        void Put(const State& state)
        {
            Playback::SetFrozenSpans(state.spans);
            auto& keyframeManager = KeyframeManager::Get();
            for (const auto& [property, keys] : state.keyframes)
            {
                auto& now = keyframeManager.GetKeyframes(property);
                now = keys;
                keyframeManager.SortAndSaveKeyframes(now);
            }
            for (const auto& [id, tick] : state.markers)
                Markers::SetTick(id, tick);
            auto& settings = CaptureManager::Get().GetCaptureSettings();
            settings.startTick = state.captureStart;
            settings.endTick = state.captureEnd;
            KeyframeSerializer::WriteRecentSoon();
        }

        // the spans in place of those of the state, and the editor's ticks moved as the mapping says
        void Apply(const State& from, std::vector<Types::FrozenSpan> spans,
                   const std::function<std::uint32_t(std::uint32_t)>& move)
        {
            Playback::SetFrozenSpans(std::move(spans));
            auto& keyframeManager = KeyframeManager::Get();
            for (const auto& [property, keys] : from.keyframes)
            {
                auto& now = keyframeManager.GetKeyframes(property);
                now = keys;
                // (keys squeezed into a span's part cut off are kept apart, a tick each)
                std::vector<std::int64_t> ticks;
                std::vector<bool> moved;
                for (const auto& key : keys)
                {
                    ticks.push_back(move(key.tick));
                    moved.push_back(ticks.back() != key.tick);
                }
                UI::TimelineMath::SeparateTicks(ticks, moved, -1);
                for (std::size_t i = 0; i < now.size(); ++i)
                    now[i].tick = static_cast<std::uint32_t>(std::max<std::int64_t>(ticks[i], 0));
                keyframeManager.SortAndSaveKeyframes(now);
            }
            for (const auto& [id, tick] : from.markers)
                Markers::SetTick(id, move(tick));
            auto& settings = CaptureManager::Get().GetCaptureSettings();
            settings.startTick = move(from.captureStart);
            settings.endTick = move(from.captureEnd);
            KeyframeSerializer::WriteRecentSoon();
        }

        void Push(std::string label, State before)
        {
            auto after = Take();
            UndoHistory::Get().Push(
                std::move(label), [before = std::move(before)] { Put(before); },
                [after = std::move(after)] { Put(after); });
        }

        std::optional<std::pair<std::size_t, State>> resizing;

        void Resize(const State& from, std::size_t index, std::uint32_t length)
        {
            auto spans = from.spans;
            const auto start = FrozenSpans::GetStart(spans, index);
            const auto oldLength = spans[index].length;
            if (length == 0)
                spans.erase(spans.begin() + static_cast<std::ptrdiff_t>(index));
            else
                spans[index].length = length;
            Apply(from, std::move(spans),
                  [&](std::uint32_t tick) { return FrozenSpans::RippleResize(tick, start, oldLength, length); });
        }
    }  // namespace

    std::size_t Add(std::uint32_t timelineTick, std::uint32_t length)
    {
        auto before = Take();
        auto [spans, index] = FrozenSpans::Add(before.spans, timelineTick, length);
        Apply(before, std::move(spans),
              [&](std::uint32_t tick) { return FrozenSpans::RippleInsert(tick, timelineTick, length); });
        Push("add bullet time", std::move(before));
        return index;
    }

    void SetLength(std::size_t index, std::uint32_t length)
    {
        auto before = Take();
        if (index >= before.spans.size() || before.spans[index].length == length)
            return;
        Resize(before, index, std::max(length, MIN_LENGTH));
        Push("change the bullet time's length", std::move(before));
    }

    void Remove(std::size_t index)
    {
        auto before = Take();
        if (index >= before.spans.size())
            return;
        Resize(before, index, 0);
        Push("remove bullet time", std::move(before));
    }

    void SetActorsMove(std::size_t index, bool move)
    {
        auto before = Take();
        if (index >= before.spans.size() || before.spans[index].actorsMove == move)
            return;
        auto spans = before.spans;
        spans[index].actorsMove = move;
        Playback::SetFrozenSpans(std::move(spans));
        KeyframeSerializer::WriteRecentSoon();
        Push(move ? "let actors move in bullet time" : "hold actors in bullet time", std::move(before));
    }

    void BeginResize(std::size_t index)
    {
        auto state = Take();
        if (index < state.spans.size())
            resizing.emplace(index, std::move(state));
    }

    void UpdateResize(std::uint32_t length)
    {
        if (resizing)
            Resize(resizing->second, resizing->first, std::max(length, MIN_LENGTH));
    }

    void EndResize()
    {
        if (!resizing)
            return;
        auto [index, before] = std::move(*resizing);
        resizing.reset();
        if (before.spans != *Playback::GetFrozenSpans())
            Push("change the bullet time's length", std::move(before));
    }

    std::string Describe(std::size_t index)
    {
        const auto spans = Playback::GetFrozenSpans();
        if (index >= spans->size())
            return {};
        return std::format("Bullet time {:.1f} s at {}", (*spans)[index].length / 1000.0,
                           UI::TimelineMath::FormatTime(FrozenSpans::GetStart(*spans, index)));
    }
}  // namespace IWXMVM::Components::BulletTime
