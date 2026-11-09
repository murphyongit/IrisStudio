#include "StdInclude.hpp"
#include "Zip.hpp"

#include <cstring>

#include "Utilities/Inflate.hpp"

namespace IWXMVM::Setup::Zip
{
    namespace
    {
        template <typename T>
        std::optional<T> At(std::span<const std::uint8_t> bytes, std::size_t at)
        {
            if (at > bytes.size() || bytes.size() - at < sizeof(T))
                return std::nullopt;
            T value{};
            std::memcpy(&value, bytes.data() + at, sizeof(T));
            return value;
        }
    }  // namespace

    std::vector<std::uint8_t> WriteStored(const std::vector<Entry>& entries)
    {
        std::vector<std::uint8_t> out, directory;
        const auto put = [](std::vector<std::uint8_t>& to, auto value) {
            const auto at = to.size();
            to.resize(at + sizeof(value));
            std::memcpy(to.data() + at, &value, sizeof(value));
        };
        for (const auto& entry : entries)
        {
            const auto offset = static_cast<std::uint32_t>(out.size());
            const auto crc = Crc32(entry.data);
            const auto size = static_cast<std::uint32_t>(entry.data.size());
            const auto nameSize = static_cast<std::uint16_t>(entry.name.size());
            // the local header: version 10, no flags, stored, no time, the CRC and sizes, the name
            put(out, std::uint32_t{0x04034B50});
            for (const std::uint16_t field : {std::uint16_t{10}, std::uint16_t{0}, std::uint16_t{0}, std::uint16_t{0},
                                              std::uint16_t{0x21}})
                put(out, field);
            put(out, crc);
            put(out, size);
            put(out, size);
            put(out, nameSize);
            put(out, std::uint16_t{0});
            out.insert(out.end(), entry.name.begin(), entry.name.end());
            out.insert(out.end(), entry.data.begin(), entry.data.end());
            // and its central directory record
            put(directory, std::uint32_t{0x02014B50});
            for (const std::uint16_t field : {std::uint16_t{20}, std::uint16_t{10}, std::uint16_t{0}, std::uint16_t{0},
                                              std::uint16_t{0}, std::uint16_t{0x21}})
                put(directory, field);
            put(directory, crc);
            put(directory, size);
            put(directory, size);
            put(directory, nameSize);
            for (const std::uint16_t field : {std::uint16_t{0}, std::uint16_t{0}, std::uint16_t{0}, std::uint16_t{0}})
                put(directory, field);
            put(directory, std::uint32_t{0});
            put(directory, offset);
            directory.insert(directory.end(), entry.name.begin(), entry.name.end());
        }
        const auto directoryOffset = static_cast<std::uint32_t>(out.size());
        out.insert(out.end(), directory.begin(), directory.end());
        put(out, std::uint32_t{0x06054B50});
        put(out, std::uint16_t{0});
        put(out, std::uint16_t{0});
        put(out, static_cast<std::uint16_t>(entries.size()));
        put(out, static_cast<std::uint16_t>(entries.size()));
        put(out, static_cast<std::uint32_t>(directory.size()));
        put(out, directoryOffset);
        put(out, std::uint16_t{0});
        return out;
    }

    std::uint32_t Crc32(std::span<const std::uint8_t> data)
    {
        static const auto table = [] {
            std::array<std::uint32_t, 256> t{};
            for (std::uint32_t i = 0; i < 256; ++i)
            {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        std::uint32_t crc = 0xFFFFFFFFu;
        for (const auto byte : data)
            crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    bool IsSafeName(std::string_view name)
    {
        if (name.empty() || name.front() == '/' || name.front() == '\\' || name.find(':') != std::string_view::npos)
            return false;
        // no part may be ..
        std::size_t start = 0;
        while (start <= name.size())
        {
            auto end = name.find_first_of("/\\", start);
            if (end == std::string_view::npos)
                end = name.size();
            if (name.substr(start, end - start) == "..")
                return false;
            start = end + 1;
        }
        return true;
    }

    std::optional<std::vector<Entry>> Read(std::span<const std::uint8_t> archive, std::size_t limit,
                                           const std::function<bool(std::string_view name)>& want)
    {
        // the end of the central directory: its signature within the last 64 KB and 22 bytes
        constexpr std::uint32_t END = 0x06054B50, CENTRAL = 0x02014B50, LOCAL = 0x04034B50;
        if (archive.size() < 22)
            return std::nullopt;
        std::optional<std::size_t> end;
        const auto earliest = archive.size() > 22 + 65535 ? archive.size() - 22 - 65535 : 0;
        for (auto at = archive.size() - 22;; --at)
        {
            if (At<std::uint32_t>(archive, at) == END)
            {
                end = at;
                break;
            }
            if (at == earliest)
                break;
        }
        if (!end)
            return std::nullopt;
        const auto count = At<std::uint16_t>(archive, *end + 10);
        const auto directory = At<std::uint32_t>(archive, *end + 16);
        if (!count || !directory)
            return std::nullopt;

        std::vector<Entry> entries;
        std::size_t at = *directory;
        for (std::uint16_t i = 0; i < *count; ++i)
        {
            if (At<std::uint32_t>(archive, at) != CENTRAL)
                return std::nullopt;
            const auto method = At<std::uint16_t>(archive, at + 10);
            const auto crc = At<std::uint32_t>(archive, at + 16);
            const auto packedSize = At<std::uint32_t>(archive, at + 20);
            const auto size = At<std::uint32_t>(archive, at + 24);
            const auto nameLength = At<std::uint16_t>(archive, at + 28);
            const auto extraLength = At<std::uint16_t>(archive, at + 30);
            const auto commentLength = At<std::uint16_t>(archive, at + 32);
            const auto local = At<std::uint32_t>(archive, at + 42);
            if (!method || !crc || !packedSize || !size || !nameLength || !extraLength || !commentLength || !local ||
                at + 46 + *nameLength > archive.size())
                return std::nullopt;
            Entry entry;
            entry.name.assign(reinterpret_cast<const char*>(archive.data() + at + 46), *nameLength);
            at += 46 + std::size_t{*nameLength} + *extraLength + *commentLength;
            if (want && !want(entry.name))
                continue;
            if (*size > limit)
                return std::nullopt;

            // the data is after the local header, whose name and extra field may differ in size from the directory's
            if (At<std::uint32_t>(archive, *local) != LOCAL)
                return std::nullopt;
            const auto localName = At<std::uint16_t>(archive, *local + 26);
            const auto localExtra = At<std::uint16_t>(archive, *local + 28);
            if (!localName || !localExtra)
                return std::nullopt;
            const std::size_t dataAt = *local + 30 + std::size_t{*localName} + *localExtra;
            if (dataAt > archive.size() || archive.size() - dataAt < *packedSize)
                return std::nullopt;
            const auto packed = archive.subspan(dataAt, *packedSize);
            if (*method == 0)
            {
                if (*packedSize != *size)
                    return std::nullopt;
                entry.data.assign(packed.begin(), packed.end());
            }
            else if (*method == 8)
            {
                auto data = Inflate::Raw(packed, *size);
                if (!data)
                    return std::nullopt;
                entry.data = std::move(*data);
            }
            else
            {
                return std::nullopt;
            }
            if (Crc32(entry.data) != *crc)
                return std::nullopt;
            entries.push_back(std::move(entry));
        }
        return entries;
    }
}  // namespace IWXMVM::Setup::Zip
