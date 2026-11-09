#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The animation pack's writing, as tools/t5-characters/animations.py does it (pack): reading a PC fastfile offline,
// finding its XAnimParts, normalizing each as the pack has it, and the pack's header and index. The launcher writes
// the pack with it from the user's own zones; AnimationPackLogic reads it back in the
// mod. The mod doesn't build this file; the launcher and the unit tests do.
namespace IWXMVM::Setup::PackWriter
{
    // A PC zone: "IWffu100", version 0x1D9, then one zlib stream (fastfile.py). The inflated stream, or none for a
    // file that isn't one; limit is the most it may inflate to.
    std::optional<std::vector<std::uint8_t>> InflateFastfile(std::span<const std::uint8_t> file, std::size_t limit);

    // the zone's script strings (bone and notetrack names index into them), as their bytes
    std::vector<std::string> ScriptStrings(std::span<const std::uint8_t> zone);

    // Every XAnimParts of an inflated zone, as animations.py's read_zone finds them (a name pointer that follows, the
    // 0x68 byte struct, a plain name; read_parts checks the rest), the first of each name only (a repeat is a false
    // find). Each is given to found as its name and its entry: the parts with their bone and notetrack names as
    // indices into strings of its own, the struct's padding zeroed, then those strings (normalized()).
    void ForEachAnimation(std::span<const std::uint8_t> zone,
                          const std::function<void(std::string_view name, std::span<const std::uint8_t> entry)>& found);

    // the 8 bytes of an entry's hash: the first of its SHA-1
    std::array<std::uint8_t, 8> EntryHash(std::span<const std::uint8_t> entry);

    // an entry of the pack's index, before the offsets are known
    struct IndexRecord
    {
        std::string key;
        std::uint32_t size = 0;      // zlib'd
        std::uint32_t inflated = 0;  // the entry
        std::array<std::uint8_t, 8> hash{};
    };
    // the pack's header and index (version 1): "IWXMVMAP", the version, the count, then each record with the offset
    // its entry will have when the entries follow in this order
    std::vector<std::uint8_t> Header(const std::vector<IndexRecord>& records);
}  // namespace IWXMVM::Setup::PackWriter
