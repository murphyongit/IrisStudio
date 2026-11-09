#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace IWXMVM::Deflate
{
    // zlib data (RFC 1950) of bytes, as tools/t5-characters/animations.py writes each entry of the animation pack and
    // Inflate::Zlib reads it back: LZ77 over a 32 KB window (hash chains, one step of lazy matching), each block of
    // up to 16K symbols coded with Huffman codes of its own, or fixed ones, or stored, whichever is smallest. The
    // launcher writes the pack with it; its bytes differ from Python's zlib, what they
    // inflate to doesn't.
    std::vector<std::uint8_t> Zlib(std::span<const std::uint8_t> data);
}  // namespace IWXMVM::Deflate
