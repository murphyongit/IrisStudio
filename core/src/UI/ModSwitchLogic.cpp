#include "StdInclude.hpp"
#include "ModSwitchLogic.hpp"

namespace IWXMVM::UI::ModSwitchLogic
{
    Outcome Advance(Switch& modSwitch, const Seen& seen)
    {
        ++modSwitch.frames;
        const bool gaveUp = seen.now >= modSwitch.since + GIVE_UP_AFTER;
        if (!modSwitch.sent)
        {
            if (seen.state != Types::GameState::MainMenu)
            {
                modSwitch.atMenu.reset();
                if (!std::exchange(modSwitch.stopping, true))
                    return Outcome::Stop;
                return gaveUp ? Outcome::GaveUp : Outcome::Waiting;
            }
            if (!modSwitch.atMenu)
                modSwitch.atMenu = seen.now;
            const bool ready = modSwitch.frames >= SEND_AFTER &&
                               (!modSwitch.stopping || seen.now >= *modSwitch.atMenu + MENU_FOR);
            if (!ready)
                return gaveUp ? Outcome::GaveUp : Outcome::Waiting;
            modSwitch.sent = seen.now;
            modSwitch.initializedAtSend = seen.initialized;
            return Outcome::Send;
        }

        const bool reloaded = seen.loadedFsGame == modSwitch.fsGame && seen.initialized != modSwitch.initializedAtSend &&
                              seen.state == Types::GameState::MainMenu;
        if (!reloaded)
        {
            modSwitch.settled.reset();
            return gaveUp ? Outcome::GaveUp : Outcome::Waiting;
        }
        // (late is fine: ReShade can hold the frames for minutes as it compiles its effects for the new device)
        if (!modSwitch.settled)
        {
            modSwitch.settled = seen.now;
            modSwitch.mainFramesAtSettled = seen.mainFrames.value_or(0);
        }
        const bool mainRan = !seen.mainFrames || *seen.mainFrames >= modSwitch.mainFramesAtSettled + SETTLE_MAIN_FRAMES;
        return seen.now >= *modSwitch.settled + SETTLE_FOR && mainRan ? Outcome::Done : Outcome::Waiting;
    }
}  // namespace IWXMVM::UI::ModSwitchLogic
