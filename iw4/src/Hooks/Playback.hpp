#pragma once
#include "StdInclude.hpp"

namespace IWXMVM::IW4::Hooks::Playback
{
    void Install();

    // How many bytes come before the body of the demo message being read (9, or 13 for the kind with one more
    // number), and where in the file its body begins.
    std::uint32_t GetMessageHeaderSize();
}  // namespace IWXMVM::IW4::Hooks::Playback
