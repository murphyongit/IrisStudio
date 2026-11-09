#include "StdInclude.hpp"
#include "DemoLoadLogic.hpp"

namespace IWXMVM::UI::DemoLoadLogic
{
    Outcome Advance(Load& load, const Seen& seen)
    {
        ++load.frames;
        if (!load.sent)
            return load.frames >= SEND_AFTER ? Outcome::Send : Outcome::Waiting;
        if (seen.state == Types::GameState::InDemo && seen.playing == load.name)
            return Outcome::Played;
        if (seen.now >= load.since + GIVE_UP_AFTER)
            return Outcome::GaveUp;
        // (a game that can't tell waits until the demo plays, or a minute)
        if (!seen.reading)
            return Outcome::Waiting;
        if (*seen.reading)
        {
            load.read = true;
            load.stopped.reset();
            return Outcome::Waiting;
        }
        if (!load.read)
            return seen.now >= *load.sent + START_WITHIN ? Outcome::NotStarted : Outcome::Waiting;
        // (another demo playing still, as the game leaves it for this one)
        if (seen.state != Types::GameState::MainMenu)
            return Outcome::Waiting;
        if (!load.stopped)
            load.stopped = seen.now;
        return seen.now >= *load.stopped + BACK_AT_MENU ? Outcome::BackAtMenu : Outcome::Waiting;
    }

    std::string_view Describe(Outcome outcome)
    {
        switch (outcome)
        {
            case Outcome::BackAtMenu:
                return "the game went back to the menu";
            case Outcome::NotStarted:
                return "the game didn't start it";
            case Outcome::GaveUp:
                return "it didn't play within a minute";
            default:
                return {};
        }
    }
}  // namespace IWXMVM::UI::DemoLoadLogic
