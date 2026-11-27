#pragma once
#include "StdInclude.hpp"

namespace IWXMVM::IW4::DemoParser
{
    // Reads the demo that has just been started (GetDemoInfo's path) for its first and last server times.
    void Run();
    std::pair<std::int32_t, std::int32_t> GetDemoTickRange();
}  // namespace IWXMVM::IW4::DemoParser
