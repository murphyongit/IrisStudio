#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "Detect.hpp"

namespace Launcher
{
    enum class SetupStage
    {
        Download,  // OpenAssetTools, after the user said yes
        FFmpeg,    // the pinned FFmpeg build, when the mod has none
        Zones,     // the character zones, a level at a time with the Linker
        Pack,      // the animation pack, read from the game's zones
        Done,
    };

    struct SetupStatus
    {
        SetupStage stage = SetupStage::Download;
        std::size_t done = 0;   // of the stage: zones built, zones read
        std::size_t total = 0;
        float fraction = 0.0f;  // of the whole setup
        std::optional<double> secondsLeft;
        bool finished = false;
        bool cancelled = false;
        std::string error;  // why it stopped, when it failed
        std::size_t zonesBuilt = 0;
        std::size_t zonesFailed = 0;
        std::size_t animationsWritten = 0;
        std::size_t animationsMissing = 0;
        std::uint64_t downloaded = 0;  // of the download running
        std::uint64_t downloadSize = 0;
    };

    // The setup on a thread of its own: OpenAssetTools when asked for, the character
    // zones (zones.py) and the animation pack (animations.py pack), into the mod's folder, then setup.json. Nothing of
    // the game's folder is written. Cancel stops it between zones and between the pack's zones.
    class SetupJob
    {
       public:
        struct Plan
        {
            bool download = false;
            bool ffmpeg = false;
            bool zones = false;
            bool pack = false;
            bool rebuild = false;  // every zone, not only those missing
        };

        SetupJob(const Options& options, const Detection& detection, Plan plan);
        ~SetupJob();

        void Cancel();
        SetupStatus Status() const;
        std::filesystem::path LogFile() const;

       private:
        void Run();

        struct Work;
        std::unique_ptr<Work> work;
        std::thread thread;
    };

    // --setup: what is missing (or everything with --rebuild) set up without a window; 0 when it succeeded, 1 when
    // it failed or was cancelled, 2 when there is no game to set up from
    int RunSetup(const Options& options);
}  // namespace Launcher
