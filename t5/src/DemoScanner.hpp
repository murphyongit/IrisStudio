#pragma once
#include "Types/DemoEvents.hpp"

namespace IWXMVM::T5::DemoScanner
{
    // Reads the kills of a demo on a thread of its own, through the game's own message functions but into state of
    // its own, so playback isn't touched. The demo's previous result is dropped right away.
    void Start(const std::filesystem::path& demoPath, std::int32_t demoStartTime);

    // null until the demo has been read
    std::shared_ptr<const Types::DemoEvents> GetResult();

    // A demo's server info and system info (its gamestate's config strings CS_SERVERINFO, with its mapname, and
    // CS_SYSTEMINFO, with its fs_game), read on the calling thread through the game's message functions; none when the
    // file can't be read or has no gamestate at its start
    struct DemoInfo
    {
        std::string serverInfo;
        std::string systemInfo;
    };
    std::optional<DemoInfo> ReadInfo(const std::filesystem::path& demoPath);

    // The config strings of the weapons a film sets as it plays (each a gun the server registered after the film
    // began), by their place among the weapons' strings; read on the calling thread through the game's message
    // functions, from the whole file. Only the client's start reads those strings
    std::map<std::int32_t, std::string> ReadWeaponStrings(const std::filesystem::path& demoPath);

    void Install();
}  // namespace IWXMVM::T5::DemoScanner
