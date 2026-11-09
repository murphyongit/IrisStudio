#include "StdInclude.hpp"
#include "GettingStartedLogic.hpp"

namespace IWXMVM::UI::GettingStartedLogic
{
    namespace
    {
        constexpr std::array<std::string_view, STEPS> NAMES = {"loadDemo", "jumpToKill", "addNodes", "record"};

        void Tick(State& state, Step step, bool done)
        {
            if (done)
                state.done[static_cast<std::size_t>(step)] = true;
        }
    }  // namespace

    State Update(State state, const Seen& seen)
    {
        Tick(state, Step::LoadDemo, seen.inDemo);
        Tick(state, Step::JumpToKill, seen.inDemo && seen.jumpedToKill);
        Tick(state, Step::AddNodes, seen.inDemo && seen.nodes >= 2);
        Tick(state, Step::Record, seen.inDemo && seen.recording);
        return state;
    }

    std::size_t CountDone(const State& state)
    {
        return static_cast<std::size_t>(std::ranges::count(state.done, true));
    }

    std::optional<Step> GetNext(const State& state)
    {
        for (std::size_t i = 0; i < STEPS; i++)
        {
            if (!state.done[i])
                return static_cast<Step>(i);
        }
        return std::nullopt;
    }

    std::vector<std::string> GetDoneNames(const State& state)
    {
        std::vector<std::string> names;
        for (std::size_t i = 0; i < STEPS; i++)
        {
            if (state.done[i])
                names.emplace_back(NAMES[i]);
        }
        return names;
    }

    State WithDoneNames(State state, std::span<const std::string> names)
    {
        for (const auto& name : names)
        {
            if (const auto it = std::ranges::find(NAMES, name); it != NAMES.end())
                state.done[static_cast<std::size_t>(it - NAMES.begin())] = true;
        }
        return state;
    }
}  // namespace IWXMVM::UI::GettingStartedLogic
