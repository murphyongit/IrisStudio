#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace IWXMVM::Sha1
{
    // SHA-1 (FIPS 180-4) of bytes. The animation pack keys each entry by the first 8 bytes of it, as
    // tools/t5-characters/animations.py does with hashlib.
    std::array<std::uint8_t, 20> Of(std::span<const std::uint8_t> bytes);
}  // namespace IWXMVM::Sha1
