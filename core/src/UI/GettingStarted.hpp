#pragma once
#include "UI/GettingStartedLogic.hpp"

// The first-run card at the top of the Demo tab: load a demo, jump to a kill, fly
// and add campath nodes, record, each ticked off as the user does it (GettingStartedLogic). config.json keeps the steps
// done and whether it was closed; Help > Getting started shows it again.
namespace IWXMVM::UI::GettingStarted
{
    void RegisterCommands();

    // once a frame: ticks off what was done
    void Update();

    // the card, unless it was closed
    void DrawCard();

    // shows it again, on the Demo tab
    void Show();

    GettingStartedLogic::State GetState();
    // the steps done and whether it is closed, saved
    void SetState(const GettingStartedLogic::State& state);
    // (for the harness) no step done and shown, counting jumps to kills from now
    void Reset();
}  // namespace IWXMVM::UI::GettingStarted
