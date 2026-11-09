#pragma once

// The first-run card of the Demo tab, apart from ImGui so it can be tested: four
// steps that tick off as the user does them, in any order, and stay ticked
namespace IWXMVM::UI::GettingStartedLogic
{
    enum class Step : std::uint8_t
    {
        LoadDemo,
        JumpToKill,
        AddNodes,  // fly and add campath nodes, two at least (a path)
        Record,

        Count,
    };
    inline constexpr std::size_t STEPS = static_cast<std::size_t>(Step::Count);

    struct State
    {
        std::array<bool, STEPS> done{};
        bool hidden = false;  // closed (Help > Getting started shows it again)

        bool operator==(const State&) const = default;
    };

    // what the editor shows now, which ticks steps off
    struct Seen
    {
        bool inDemo = false;
        bool jumpedToKill = false;  // since the mod started
        std::size_t nodes = 0;      // of the campath
        bool recording = false;
    };

    // the state with the steps ticked off that what is seen does
    State Update(State state, const Seen& seen);

    std::size_t CountDone(const State& state);
    // the first step not done yet, if one is left
    std::optional<Step> GetNext(const State& state);

    // the steps done by name, as config.json keeps them ("loadDemo", "jumpToKill", "addNodes", "record"), and back
    // (unknown names are left out)
    std::vector<std::string> GetDoneNames(const State& state);
    State WithDoneNames(State state, std::span<const std::string> names);
}  // namespace IWXMVM::UI::GettingStartedLogic
