#include "StdInclude.hpp"
#include "RestartLogic.hpp"

namespace IWXMVM::Components::RestartLogic
{
    namespace
    {
        // ReShade's effects drawn again, or with its toggle off, loaded (nothing draws them to count)
        bool IsReShadeReady(Wait& wait, std::uint64_t now, const ReShadeState& reshade)
        {
            if (reshade.framesSince >= RESHADE_READY_FRAMES)
                return true;
            if (reshade.loading)
                return false;
            if (!reshade.effectsOn)
                return true;
            if (!wait.loadedAt)
                wait.loadedAt = now;
            return now - *wait.loadedAt >= RESHADE_DRAW_MS;
        }

        // A frame of the wait for Pack or Surface depth: whether it's over. Off, it isn't waited for. What is left is
        // read over some frames: a frame after a jump, or of a texture let go, can still have nothing waiting.
        bool AdvanceLoad(LoadWait& load, std::uint64_t now, bool on, std::uint32_t left, bool moving,
                         std::uint64_t timeout)
        {
            if (load.done)
                return true;
            if (!load.started)
            {
                load.started = now;
                load.on = on;
            }
            load.left = left;
            load.most = std::max(load.most, left);
            load.clearFrames = left == 0 && !moving ? load.clearFrames + 1 : 0;
            if (!load.on || load.clearFrames >= LOAD_READY_FRAMES)
            {
                load.done = now;
                return true;
            }
            if (now - *load.started < timeout)
                return false;
            load.timedOut = true;
            load.done = now;
            return true;
        }

        // 1,495
        std::string FormatCount(std::uint32_t count)
        {
            return count < 1000 ? std::to_string(count) : std::format("{},{:03}", count / 1000, count % 1000);
        }

        std::string FormatTime(std::uint64_t ms)
        {
            const auto seconds = ms / 1000;
            return std::format("{}:{:02}", seconds / 60, seconds % 60);
        }
    }  // namespace

    Wait Start(Kind kind, std::uint64_t now, std::uint32_t resets)
    {
        return {.kind = kind, .created = now, .started = now, .resets = resets};
    }

    Step Advance(Wait& wait, std::uint64_t now, const Frame& frame)
    {
        if (wait.held)
        {
            wait.heldForReShade |= frame.reshade.loading;
            ++wait.heldFrames;
            if (wait.heldFrames < SCREEN_FRAMES || (frame.reshade.loading && now - wait.started < wait.holdTimeout))
                return Step::Held;
            wait.held = false;
            wait.heldMs = wait.heldForReShade ? static_cast<std::uint32_t>(now - wait.started) : 0;
            wait.started = now;
            wait.resets = frame.resets;
            return Step::Ask;
        }

        if (!wait.gameDone)
        {
            // (the shadow map's size is read as the game makes its render targets again, which may be before the
            // device's reset: the reset counts too)
            if (frame.resets == wait.resets || !frame.changed)
                return now - wait.started >= GAME_TIMEOUT_MS ? Step::GameTimedOut : Step::Game;
            wait.gameDone = now;
            wait.waitsForReShade = frame.reshade.present;
        }

        if (wait.waitsForReShade && !wait.reshadeDone)
        {
            if (!IsReShadeReady(wait, now, frame.reshade))
            {
                if (now - *wait.gameDone < wait.reshadeTimeout)
                    return Step::ReShade;
                wait.reshadeTimedOut = true;
            }
            wait.reshadeDone = now;
        }

        if (wait.kind == Kind::Capture && !wait.atStart)
        {
            wait.atStart = true;
            return Step::AtStart;
        }

        const bool capture = wait.kind == Kind::Capture;
        const auto timeout = capture ? CAPTURE_LOAD_TIMEOUT_MS : VIEW_LOAD_TIMEOUT_MS;
        const auto packLeft = frame.pack.drawn + (capture ? frame.pack.ahead : 0);
        if (!AdvanceLoad(wait.pack, now, frame.pack.on, packLeft, frame.moving, timeout))
            return Step::Pack;
        if (!AdvanceLoad(wait.surface, now, frame.surface.on, frame.surface.drawn, frame.moving, timeout))
            return Step::Surface;
        return Step::Done;
    }

    std::string GetLabel(Kind kind, std::string_view what)
    {
        switch (kind)
        {
            case Kind::Capture:
                return std::format("SWITCHING TO {} FOR THE CAPTURE", what);
            case Kind::Back:
                return std::format("SWITCHING BACK TO {}", what);
            default:
            {
                std::string upper(what);
                std::ranges::transform(upper, upper.begin(), [](char c) { return static_cast<char>(std::toupper(c)); });
                return std::format("SWITCHING SHADOW QUALITY TO {}", upper);
            }
        }
    }

    Screen GetScreen(const Wait& wait, Step step, std::uint64_t now)
    {
        Screen screen;
        const auto heldForReShade = step == Step::Held && wait.heldForReShade;
        const auto count = [&](std::string_view name, const LoadWait& load) {
            return load.most ? std::format("Loading {} {} of {}", name, FormatCount(load.most - load.left),
                                           FormatCount(load.most))
                             : std::format("Loading {}", name);
        };
        switch (step)
        {
            case Step::Held:
            case Step::Ask:
            case Step::Game:
                screen.line = heldForReShade ? "Loading ReShade" : "Restarting the renderer";
                break;
            case Step::ReShade:
            case Step::AtStart:
                screen.line = "Loading ReShade";
                break;
            case Step::Pack:
                screen.line = count("Pack", wait.pack);
                break;
            case Step::Surface:
                screen.line = count("Surface depth", wait.surface);
                break;
            default:
                break;
        }
        if (now - wait.created >= SHOW_TIME_MS)
            screen.time = FormatTime(now - wait.created);
        // (a load from ReShade's cache is seconds: minutes are a size it hasn't compiled at)
        const auto onReShade = heldForReShade        ? now - wait.started
                               : step == Step::ReShade ? now - *wait.gameDone
                                                       : 0;
        if (onReShade >= SHOW_NOTE_MS)
            screen.note = "First time at this size, a few minutes";
        return screen;
    }

    float GetRingTurn(std::uint64_t now)
    {
        constexpr std::uint64_t TURN_MS = 1080;
        return static_cast<float>(now % TURN_MS) / TURN_MS;
    }

    RingPixel GetRingPixel(float dx, float dy, float radius, float thickness, float turn)
    {
        constexpr float PI = 3.14159265f;
        const auto distance = std::sqrt(dx * dx + dy * dy);
        const auto track = std::clamp(thickness / 2.0f + 0.5f - std::abs(distance - radius), 0.0f, 1.0f);
        if (track == 0.0f)
            return {};
        // from the top, clockwise on the screen, as far along the arc as the pixel is
        auto along = (std::atan2(dy, dx) + PI / 2.0f) / (2.0f * PI) - turn;
        along -= std::floor(along);
        const auto fromEnd = std::min(along, RING_ARC - along) * 2.0f * PI * radius;
        return {track, track * std::clamp(fromEnd + 0.5f, 0.0f, 1.0f)};
    }

    std::string DescribeFramesWithoutReShade(std::uint32_t frames)
    {
        return std::format("ReShade was loading for {} frame{} of the capture", frames, frames == 1 ? "" : "s");
    }

    std::string DescribeNotReady(std::string_view what)
    {
        return std::format("{} wasn't ready as the capture began", what);
    }
}  // namespace IWXMVM::Components::RestartLogic
