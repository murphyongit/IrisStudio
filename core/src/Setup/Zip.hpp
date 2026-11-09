#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Reading a zip archive (PKWARE's APPNOTE: the central directory, entries stored or deflated, their CRC-32), as the
// launcher unpacks OpenAssetTools' release after checking the zip's SHA-256. The mod
// doesn't build this file; the launcher and the unit tests do.
namespace IWXMVM::Setup::Zip
{
    struct Entry
    {
        std::string name;  // as the archive has it, folders with /; a folder's ends with /
        std::vector<std::uint8_t> data;
    };

    // Every entry of an archive (or those want takes, by name), or none for one that isn't a zip, uses another method,
    // is cut short or doesn't match its CRC-32. No entry taken is bigger than limit.
    std::optional<std::vector<Entry>> Read(std::span<const std::uint8_t> archive, std::size_t limit,
                                           const std::function<bool(std::string_view name)>& want = {});

    // whether an entry's name may be written under a folder: relative, without .. or a drive
    bool IsSafeName(std::string_view name);

    std::uint32_t Crc32(std::span<const std::uint8_t> data);

    // An archive of entries stored (not deflated), as the game's file system reads an IWD, with their CRC-32 (the
    // weapons' textures)
    std::vector<std::uint8_t> WriteStored(const std::vector<Entry>& entries);
}  // namespace IWXMVM::Setup::Zip
