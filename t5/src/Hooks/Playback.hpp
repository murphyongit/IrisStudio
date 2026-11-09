#pragma once

namespace IWXMVM::T5::Hooks::Playback
{
    void Install();

    // Why the game won't play a demo, in words, logged: recorded with another mod than it runs (core's DemoModLogic),
    // or on a campaign level or Zombies map whose zone the game folder doesn't have
    // (CampaignMaps); empty when it plays it or the demo can't be read
    std::string GetDemoRefusal(const std::filesystem::path& demoPath);

    // how many frames of demos were held to com_maxFrameTime, and the milliseconds they lost, since the mod started
    // (for the harness)
    struct FrameLimitStats
    {
        std::uint32_t frames = 0;
        std::uint64_t milliseconds = 0;
    };
    FrameLimitStats GetFrameLimitStats();
}  // namespace IWXMVM::T5::Hooks::Playback
