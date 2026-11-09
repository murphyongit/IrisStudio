#pragma once

namespace IWXMVM::Inflate
{
    // Inflates zlib data (RFC 1950: a deflate stream, RFC 1951, after a two-byte header and before the Adler-32 of what
    // it inflates to), as tools/t5-characters/animations.py writes each animation of the animation pack. Returns none
    // for data that isn't zlib's, is cut short, or doesn't inflate to its checksum or to the size expected.
    std::optional<std::vector<std::uint8_t>> Zlib(std::span<const std::uint8_t> data, std::size_t expectedSize);
    // The same for data of a size not known before (a fastfile's stream, which the launcher reads for the pack): any
    // size up to limit
    std::optional<std::vector<std::uint8_t>> ZlibUpTo(std::span<const std::uint8_t> data, std::size_t limit);
    // A bare deflate stream, without zlib's header and checksum (a zip's entry)
    std::optional<std::vector<std::uint8_t>> Raw(std::span<const std::uint8_t> data, std::size_t expectedSize);
    // The first bytes of a bare deflate stream, inflating no more of it than needed (a texture's header in an iwd);
    // data may be the stream's start alone. None when it doesn't inflate to that many.
    std::optional<std::vector<std::uint8_t>> RawPrefix(std::span<const std::uint8_t> data, std::size_t count);
    // zlib's checksum of data
    std::uint32_t Adler32(std::span<const std::uint8_t> data);
}  // namespace IWXMVM::Inflate
