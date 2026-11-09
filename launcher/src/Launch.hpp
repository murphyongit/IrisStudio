#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "Detect.hpp"

namespace Launcher
{
    enum class LaunchStage
    {
        Starting,          // Plutonium's bootstrapper is being started
        WaitingForWindow,  // for the game's window
        WaitingForMenu,    // for its D3D9 device, then its menu
        Loading,           // T5iris.dll is being loaded into it
        Running,           // the mod is in the game
        Closed,            // the game closed
        Failed,
    };

    struct LaunchStatus
    {
        LaunchStage stage = LaunchStage::Starting;
        std::string error;
    };

    // Starting T5: T5iris.dll written to the launcher's folder from the copy it embeds,
    // Plutonium's bootstrapper started in LAN as tools/t5-harness/launch.ps1 starts it, then the injector's wait,
    // checks and load (tools/injector/src/Injector.hpp), on a thread of its own. Closing the launcher leaves the game
    // running.
    class LaunchJob
    {
       public:
        LaunchJob(const Options& options, const Settings& settings, const Detection& detection);
        ~LaunchJob();

        LaunchStatus Status() const;
        std::filesystem::path LogFile() const;

       private:
        struct Work;
        std::shared_ptr<Work> work;
    };
}  // namespace Launcher
