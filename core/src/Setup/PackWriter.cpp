#include "StdInclude.hpp"
#include "PackWriter.hpp"

#include <cstring>
#include <unordered_set>

#include "Utilities/Inflate.hpp"
#include "Utilities/Sha1.hpp"

namespace IWXMVM::Setup::PackWriter
{
    namespace
    {
        constexpr std::string_view FASTFILE_MAGIC = "IWffu100";
        constexpr std::uint32_t FASTFILE_VERSION = 0x1D9;
        constexpr std::size_t XFILE_SIZE = 36;  // {size, externalSize, blockSize[7]}
        constexpr std::uint32_t FOLLOWS = 0xFFFFFFFF;
        constexpr std::size_t PARTS_SIZE = 0x68;
        constexpr std::string_view PACK_MAGIC = "IWXMVMAP";
        constexpr std::uint32_t PACK_VERSION = 1;

        bool IsNameCharacter(std::uint8_t c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }

        // reads at offsets of the zone; one out of it fails the whole read, as Python's struct.error does
        struct Reader
        {
            std::span<const std::uint8_t> data;
            bool ok = true;

            template <typename T>
            T At(std::size_t at)
            {
                T value{};
                if (at > data.size() || data.size() - at < sizeof(T))
                {
                    ok = false;
                    return value;
                }
                std::memcpy(&value, data.data() + at, sizeof(T));
                return value;
            }
        };

        // read_delta: where the delta part that starts at p ends (its motion isn't needed for the pack)
        std::optional<std::size_t> DeltaEnd(Reader& in, std::size_t p, std::uint32_t frames)
        {
            const auto translation = in.At<std::uint32_t>(p);
            const auto rotation = in.At<std::uint32_t>(p + 4);
            p += 8;
            const std::size_t indexSize = frames < 256 ? 1 : 2;
            if (translation == FOLLOWS)
            {
                const auto size = in.At<std::uint16_t>(p);
                const auto small = in.At<std::uint8_t>(p + 2);
                if (size == 0)
                {
                    in.At<std::array<float, 3>>(p + 4);
                    p += 16;
                }
                else
                {
                    in.At<std::array<float, 3>>(p + 4);
                    in.At<std::array<float, 3>>(p + 16);
                    const auto keys = in.At<std::uint32_t>(p + 28);
                    p += 32 + (std::size_t{size} + 1) * indexSize;
                    if (keys == FOLLOWS)
                    {
                        const std::size_t width = small ? 3 : 6;
                        in.At<std::array<std::uint8_t, 3>>(p);
                        if (width == 6)
                            in.At<std::array<std::uint16_t, 3>>(p);
                        in.At<std::array<std::uint8_t, 3>>(p + size * width);
                        if (width == 6)
                            in.At<std::array<std::uint16_t, 3>>(p + size * width);
                        p += (std::size_t{size} + 1) * width;
                    }
                }
            }
            else if (translation != 0)
            {
                return std::nullopt;  // a shared translation
            }
            if (rotation == FOLLOWS)
            {
                const auto size = in.At<std::uint16_t>(p);
                if (size == 0)
                {
                    in.At<std::array<std::int16_t, 2>>(p + 4);
                    p += 8;
                }
                else
                {
                    const auto keys = in.At<std::uint32_t>(p + 4);
                    p += 8 + (std::size_t{size} + 1) * indexSize;
                    if (keys == FOLLOWS)
                    {
                        in.At<std::array<std::int16_t, 2>>(p);
                        in.At<std::array<std::int16_t, 2>>(p + 4 * std::size_t{size});
                        p += (std::size_t{size} + 1) * 4;
                    }
                }
            }
            else if (rotation != 0)
            {
                return std::nullopt;  // a shared rotation
            }
            if (!in.ok)
                return std::nullopt;
            return p;
        }

        // read_parts and normalized: the entry of the XAnimParts at s, or none when what is there isn't one
        bool ReadParts(std::span<const std::uint8_t> data, std::size_t s, const std::vector<std::string>& strings,
                       std::string& name, std::vector<std::uint8_t>& entry)
        {
            Reader in{data};
            const auto loop = in.At<std::uint8_t>(s + 0x10);
            const auto delta = in.At<std::uint8_t>(s + 0x11);
            const auto rate = in.At<float>(s + 0x30);
            if (!in.ok || loop > 1 || delta > 1 || !(rate >= 0.0f && rate <= 1000.0f))
                return false;
            const auto counts16 = in.At<std::array<std::uint16_t, 6>>(s + 4);
            const auto boneCounts = in.At<std::array<std::uint8_t, 10>>(s + 0x18);
            const auto notifyCount = in.At<std::uint8_t>(s + 0x22);
            const auto randomShortCount = in.At<std::uint32_t>(s + 0x28);
            const auto indexCount = in.At<std::uint32_t>(s + 0x2C);
            const auto pointers = in.At<std::array<std::uint32_t, 10>>(s + 0x40);
            if (!in.ok)
                return false;
            const std::uint32_t frames = counts16[5];
            // bones, then dataByte, dataShort, dataInt, randomDataShort, randomDataByte, randomDataInt, indices, notify
            const std::array<std::uint32_t, 9> counts = {boneCounts[9], counts16[0],       counts16[1],
                                                         counts16[2],   randomShortCount, counts16[3],
                                                         counts16[4],   indexCount,       notifyCount};
            // every pointer follows exactly when it has something to point to (a zone of the game never shares them)
            for (std::size_t i = 0; i < 10; ++i)
            {
                if (pointers[i] != 0 && pointers[i] != FOLLOWS)
                    return false;
                if (i < 9 && (pointers[i] == FOLLOWS) != (counts[i] > 0))
                    return false;
            }

            // the name: up to its NUL within 128 bytes
            const auto nameStart = s + PARTS_SIZE;
            std::size_t end = nameStart;
            const auto nameLimit = std::min(data.size(), nameStart + 128);
            while (end < nameLimit && data[end] != 0)
                ++end;
            if (end >= nameLimit || end == nameStart)
                return false;
            for (auto i = nameStart; i < end; ++i)
                if (!IsNameCharacter(data[i]))
                    return false;

            std::size_t p = end + 1;
            const auto namesAt = p;
            const std::size_t boneCount = boneCounts[9];
            std::vector<std::uint16_t> bones(boneCount);
            for (std::size_t i = 0; i < boneCount; ++i)
                bones[i] = in.At<std::uint16_t>(p + 2 * i);
            p += 2 * boneCount;
            const auto notifyAt = p;
            std::vector<std::uint16_t> notes(notifyCount);
            for (std::size_t i = 0; i < notifyCount; ++i)
            {
                notes[i] = in.At<std::uint16_t>(p + 8 * i);
                in.At<float>(p + 8 * i + 4);
            }
            p += 8 * std::size_t{notifyCount};
            if (!in.ok)
                return false;
            if (pointers[9] == FOLLOWS)
            {
                const auto deltaEnd = DeltaEnd(in, p, frames);
                if (!deltaEnd)
                    return false;
                p = *deltaEnd;
            }
            const std::array<std::size_t, 7> widths = {1, 2, 4, 2, 1, 4, frames < 256 ? 1u : 2u};
            for (std::size_t i = 0; i < 7; ++i)
                if (pointers[1 + i] == FOLLOWS)
                    p += std::size_t{counts[1 + i]} * widths[i];
            if (p > data.size())
                return false;

            name.assign(reinterpret_cast<const char*>(data.data() + nameStart), end - nameStart);

            // normalized: names as indices into the entry's own strings, in the order they first come
            std::vector<std::string> own;
            const auto stringOf = [&strings](std::uint16_t id) {
                return id < strings.size() ? strings[id] : "?" + std::to_string(id);
            };
            const auto indexOf = [&own](const std::string& text) {
                const auto it = std::find(own.begin(), own.end(), text);
                if (it != own.end())
                    return static_cast<std::uint16_t>(it - own.begin());
                own.push_back(text);
                return static_cast<std::uint16_t>(own.size() - 1);
            };
            const auto partsSize = static_cast<std::uint32_t>(p - s);
            entry.assign(4, 0);
            std::memcpy(entry.data(), &partsSize, 4);
            entry.insert(entry.end(), data.begin() + s, data.begin() + p);
            const auto blob = entry.data() + 4;
            blob[0x25] = blob[0x26] = blob[0x27] = 0;
            for (std::size_t i = 0; i < boneCount; ++i)
            {
                const auto id = indexOf(stringOf(bones[i]));
                std::memcpy(blob + (namesAt - s) + 2 * i, &id, 2);
            }
            for (std::size_t i = 0; i < notifyCount; ++i)
            {
                const std::array<std::uint16_t, 2> note = {indexOf(stringOf(notes[i])), 0};
                std::memcpy(blob + (notifyAt - s) + 8 * i, note.data(), 4);
            }
            const auto stringCount = static_cast<std::uint16_t>(own.size());
            entry.push_back(static_cast<std::uint8_t>(stringCount));
            entry.push_back(static_cast<std::uint8_t>(stringCount >> 8));
            for (const auto& text : own)
            {
                entry.insert(entry.end(), text.begin(), text.end());
                entry.push_back(0);
            }
            return true;
        }
    }  // namespace

    std::optional<std::vector<std::uint8_t>> InflateFastfile(std::span<const std::uint8_t> file, std::size_t limit)
    {
        if (file.size() < 12 || std::memcmp(file.data(), FASTFILE_MAGIC.data(), FASTFILE_MAGIC.size()) != 0)
            return std::nullopt;
        std::uint32_t version = 0;
        std::memcpy(&version, file.data() + 8, 4);
        if (version != FASTFILE_VERSION)
            return std::nullopt;
        return Inflate::ZlibUpTo(file.subspan(12), limit);
    }

    std::vector<std::string> ScriptStrings(std::span<const std::uint8_t> zone)
    {
        Reader in{zone};
        const auto count = in.At<std::uint32_t>(XFILE_SIZE);
        std::vector<std::string> strings;
        if (!in.ok || count > zone.size() / 4)
            return strings;
        std::size_t at = XFILE_SIZE + 16 + std::size_t{count} * 4;
        strings.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (in.At<std::uint32_t>(XFILE_SIZE + 16 + std::size_t{i} * 4) != FOLLOWS)
            {
                strings.emplace_back();
                continue;
            }
            auto end = at;
            while (end < zone.size() && zone[end] != 0)
                ++end;
            strings.emplace_back(reinterpret_cast<const char*>(zone.data() + at), end - at);
            at = end + 1;
        }
        return strings;
    }

    void ForEachAnimation(std::span<const std::uint8_t> zone,
                          const std::function<void(std::string_view name, std::span<const std::uint8_t> entry)>& found)
    {
        const auto strings = ScriptStrings(zone);
        std::unordered_set<std::string> seen;
        std::string name;
        std::vector<std::uint8_t> entry;
        // CANDIDATE_RE: four 0xFF, 100 bytes, then 1 to 100 name characters and a NUL; matches don't overlap
        const auto size = zone.size();
        for (std::size_t s = 0; s + PARTS_SIZE < size;)
        {
            if (zone[s] != 0xFF || zone[s + 1] != 0xFF || zone[s + 2] != 0xFF || zone[s + 3] != 0xFF)
            {
                ++s;
                continue;
            }
            std::size_t run = 0;
            while (run <= 100 && s + PARTS_SIZE + run < size && IsNameCharacter(zone[s + PARTS_SIZE + run]))
                ++run;
            if (run < 1 || run > 100 || s + PARTS_SIZE + run >= size || zone[s + PARTS_SIZE + run] != 0)
            {
                ++s;
                continue;
            }
            if (ReadParts(zone, s, strings, name, entry) && seen.insert(name).second)
                found(name, entry);
            s += 4;
        }
    }

    std::array<std::uint8_t, 8> EntryHash(std::span<const std::uint8_t> entry)
    {
        const auto digest = Sha1::Of(entry);
        std::array<std::uint8_t, 8> hash{};
        std::copy_n(digest.begin(), hash.size(), hash.begin());
        return hash;
    }

    std::vector<std::uint8_t> Header(const std::vector<IndexRecord>& records)
    {
        std::vector<std::uint8_t> header(PACK_MAGIC.begin(), PACK_MAGIC.end());
        const auto put = [&header](std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
                header.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        };
        put(PACK_VERSION);
        put(static_cast<std::uint32_t>(records.size()));
        std::size_t offset = header.size();
        for (const auto& record : records)
            offset += 2 + record.key.size() + 12 + 8;
        for (const auto& record : records)
        {
            header.push_back(static_cast<std::uint8_t>(record.key.size()));
            header.push_back(static_cast<std::uint8_t>(record.key.size() >> 8));
            header.insert(header.end(), record.key.begin(), record.key.end());
            put(static_cast<std::uint32_t>(offset));
            put(record.size);
            put(record.inflated);
            header.insert(header.end(), record.hash.begin(), record.hash.end());
            offset += record.size;
        }
        return header;
    }
}  // namespace IWXMVM::Setup::PackWriter
