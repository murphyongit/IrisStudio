#include "StdInclude.hpp"
#include "AnimationPackLogic.hpp"

#include <charconv>
#include <cstring>

namespace IWXMVM::AnimationPackLogic
{
    namespace
    {
        constexpr std::string_view PACK_MAGIC = "IWXMVMAP";

        template <typename T>
        T Read(std::span<const std::uint8_t> bytes, std::size_t at)
        {
            T value{};
            std::memcpy(&value, bytes.data() + at, sizeof(T));
            return value;
        }

        template <typename T>
        T ReadStream(std::istream& stream)
        {
            T value{};
            if (!stream.read(reinterpret_cast<char*>(&value), sizeof(T)))
                throw std::runtime_error("the pack ends in its index");
            return value;
        }

        std::uint32_t Align(std::uint32_t at, std::uint32_t alignment)
        {
            return (at + alignment - 1) / alignment * alignment;
        }
    }  // namespace

    std::unordered_map<std::string, IndexEntry> ReadIndex(std::istream& stream)
    {
        std::array<char, 8> magic{};
        if (!stream.read(magic.data(), magic.size()) || std::string_view(magic.data(), magic.size()) != PACK_MAGIC)
            throw std::runtime_error("it isn't an animation pack");
        const auto version = ReadStream<std::uint32_t>(stream);
        if (version != PACK_VERSION)
            throw std::runtime_error(std::format("it is of version {}, not {}", version, PACK_VERSION));
        const auto count = ReadStream<std::uint32_t>(stream);
        std::unordered_map<std::string, IndexEntry> index;
        index.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::string key(ReadStream<std::uint16_t>(stream), '\0');
            if (!stream.read(key.data(), static_cast<std::streamsize>(key.size())))
                throw std::runtime_error("the pack ends in its index");
            IndexEntry entry;
            entry.offset = ReadStream<std::uint32_t>(stream);
            entry.size = ReadStream<std::uint32_t>(stream);
            entry.inflated = ReadStream<std::uint32_t>(stream);
            entry.hash = ReadStream<std::array<std::uint8_t, 8>>(stream);
            index.emplace(std::move(key), entry);
        }
        return index;
    }

    std::optional<std::array<std::uint8_t, 8>> ParseHash(std::string_view hex)
    {
        if (hex.size() != 16)
            return std::nullopt;
        std::array<std::uint8_t, 8> hash{};
        for (std::size_t i = 0; i < hash.size(); ++i)
        {
            std::uint32_t byte = 0;
            const auto [end, error] = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, byte, 16);
            if (error != std::errc{} || end != hex.data() + 2 * i + 2)
                return std::nullopt;
            hash[i] = static_cast<std::uint8_t>(byte);
        }
        return hash;
    }

    std::optional<Entry> ReadEntry(std::span<const std::uint8_t> inflated)
    {
        if (inflated.size() < 4)
            return std::nullopt;
        const auto partsSize = Read<std::uint32_t>(inflated, 0);
        if (std::uint64_t{4} + partsSize + 2 > inflated.size())
            return std::nullopt;
        Entry entry{.parts = inflated.subspan(4, partsSize)};
        std::size_t at = 4 + std::size_t{partsSize};
        const auto count = Read<std::uint16_t>(inflated, at);
        at += 2;
        entry.strings.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto end = std::find(inflated.begin() + static_cast<std::ptrdiff_t>(at), inflated.end(), 0);
            if (end == inflated.end())
                return std::nullopt;
            entry.strings.emplace_back(reinterpret_cast<const char*>(inflated.data() + at),
                                       static_cast<std::size_t>(end - inflated.begin()) - at);
            at = static_cast<std::size_t>(end - inflated.begin()) + 1;
        }
        if (at != inflated.size())
            return std::nullopt;
        return entry;
    }

    std::optional<Layout> Plan(std::span<const std::uint8_t> parts, std::string* why)
    {
        const auto fail = [&](std::string reason) -> std::optional<Layout> {
            if (why)
                *why = std::move(reason);
            return std::nullopt;
        };
        if (parts.size() < PARTS_SIZE)
            return fail("it is shorter than an XAnimParts");
        using namespace Offsets;
        Layout layout;
        layout.frames = Read<std::uint16_t>(parts, NUMFRAMES);
        layout.rate = Read<float>(parts, FRAMERATE);
        layout.loop = parts[LOOP] != 0;
        layout.delta = parts[DELTA] != 0;
        const auto bones = Read<std::array<std::uint8_t, 10>>(parts, BONE_COUNT);
        const auto byRotation = bones[0] + bones[1] + bones[2] + bones[3] + bones[4];
        const auto byTranslation = bones[5] + bones[6] + bones[7] + bones[8];
        if (byRotation != bones[9] || byTranslation != bones[9])
            return fail(std::format("its bone counts by rotation ({}) and translation ({}) aren't its {} bones",
                                    byRotation, byTranslation, bones[9]));

        // each pointer follows exactly when it has something to point to (the game's zones never share them)
        const bool longIndices = layout.frames >= 256;
        struct Stream
        {
            std::uint32_t pointer;  // where the struct has it
            std::uint32_t count;
            std::uint32_t width;  // of an element, which it is aligned to
        };
        const std::array<Stream, 9> streams = {{
            {NAMES, bones[9], 2},
            {NOTIFY, parts[NOTIFY_COUNT], 8},
            {DATA_BYTE, Read<std::uint16_t>(parts, DATA_BYTE_COUNT), 1},
            {DATA_SHORT, Read<std::uint16_t>(parts, DATA_SHORT_COUNT), 2},
            {DATA_INT, Read<std::uint16_t>(parts, DATA_INT_COUNT), 4},
            {RANDOM_DATA_SHORT, Read<std::uint32_t>(parts, RANDOM_DATA_SHORT_COUNT), 2},
            {RANDOM_DATA_BYTE, Read<std::uint16_t>(parts, RANDOM_DATA_BYTE_COUNT), 1},
            {RANDOM_DATA_INT, Read<std::uint16_t>(parts, RANDOM_DATA_INT_COUNT), 4},
            {INDICES, Read<std::uint32_t>(parts, INDEX_COUNT), longIndices ? 2u : 1u},
        }};
        for (const auto& stream : streams)
        {
            const auto pointer = Read<std::uint32_t>(parts, stream.pointer);
            if ((pointer != 0 && pointer != FOLLOWS) || (pointer == FOLLOWS) != (stream.count > 0))
                return fail(
                    std::format("its pointer at {:#x} doesn't go with its count {}", stream.pointer, stream.count));
        }
        const auto deltaPart = Read<std::uint32_t>(parts, DELTA_PART);
        if (Read<std::uint32_t>(parts, NAME) != FOLLOWS || (deltaPart != 0 && deltaPart != FOLLOWS))
            return fail("its name or delta part pointer isn't one");

        // the pieces, each where the one before ends in the parts and aligned in the memory
        std::uint32_t from = 0, to = 0;
        bool overrun = false;
        const auto take = [&](std::uint64_t size, std::uint32_t alignment,
                              std::optional<std::uint32_t> pointer) -> std::uint32_t {
            if (from + size > parts.size())
            {
                overrun = true;
                return 0;
            }
            to = Align(to, alignment);
            layout.pieces.push_back({from, static_cast<std::uint32_t>(size), to});
            if (pointer)
                layout.pointers.push_back({*pointer, to});
            const auto at = to;
            from += static_cast<std::uint32_t>(size);
            to += static_cast<std::uint32_t>(size);
            return at;
        };
        take(PARTS_SIZE, MEMORY_ALIGNMENT, std::nullopt);
        const auto nameEnd = std::find(parts.begin() + PARTS_SIZE, parts.end(), 0);
        if (nameEnd == parts.end())
            return fail("its name doesn't end");
        layout.name.assign(reinterpret_cast<const char*>(parts.data() + PARTS_SIZE),
                           static_cast<std::size_t>(nameEnd - parts.begin()) - PARTS_SIZE);
        take(static_cast<std::uint32_t>(layout.name.size()) + 1, 1, NAME);
        if (bones[9] != 0)
        {
            layout.names = take(bones[9] * 2u, 2, NAMES);
            layout.nameCount = bones[9];
        }
        if (parts[NOTIFY_COUNT] != 0)
        {
            layout.notify = take(parts[NOTIFY_COUNT] * 8u, 4, NOTIFY);
            layout.notifyCount = parts[NOTIFY_COUNT];
        }
        if (deltaPart == FOLLOWS && !overrun)
        {
            // {translation, rotation about z}, each {u16 size, ...} with size + 1 keys, or one when size is 0
            if (std::uint64_t{from} + 8 > parts.size())
                return fail("its delta part is cut short");
            const auto transPointer = Read<std::uint32_t>(parts, from);
            const auto quatPointer = Read<std::uint32_t>(parts, from + 4);
            if ((transPointer != 0 && transPointer != FOLLOWS) || (quatPointer != 0 && quatPointer != FOLLOWS))
                return fail("its delta part's pointers aren't ones");
            const auto delta = take(8, 4, DELTA_PART);
            const std::uint32_t indexSize = longIndices ? 2 : 1;
            if (transPointer == FOLLOWS)
            {
                // {u16 size, u8 smallTrans, u8, then a vec3, or mins, size, the keys' pointer and the frames' indices}
                if (std::uint64_t{from} + 4 > parts.size())
                    return fail("its delta translation is cut short");
                const std::uint32_t size = Read<std::uint16_t>(parts, from);
                const bool smallKeys = parts[from + 2] != 0;
                if (size == 0)
                {
                    take(16, 4, delta);
                }
                else
                {
                    if (std::uint64_t{from} + 0x20 > parts.size())
                        return fail("its delta translation is cut short");
                    const auto keys = Read<std::uint32_t>(parts, from + 0x1C);
                    if (keys != 0 && keys != FOLLOWS)
                        return fail("its delta translation's keys pointer isn't one");
                    const auto trans = take(0x20 + (size + 1) * indexSize, 4, delta);
                    if (keys == FOLLOWS)
                        take((size + 1) * (smallKeys ? 3 : 6), smallKeys ? 1 : 4, trans + 0x1C);
                }
            }
            if (quatPointer == FOLLOWS && !overrun)
            {
                // {u16 size, u16, then two s16, or the keys' pointer and the frames' indices}
                if (std::uint64_t{from} + 8 > parts.size())
                    return fail("its delta rotation is cut short");
                const std::uint32_t size = Read<std::uint16_t>(parts, from);
                if (size == 0)
                {
                    take(8, 4, delta + 4);
                }
                else
                {
                    const auto keys = Read<std::uint32_t>(parts, from + 4);
                    if (keys != 0 && keys != FOLLOWS)
                        return fail("its delta rotation's keys pointer isn't one");
                    const auto quat = take(8 + (size + 1) * indexSize, 4, delta + 4);
                    if (keys == FOLLOWS)
                        take((size + 1) * 4, 4, quat + 4);
                }
            }
        }
        // the data streams, in the order the zone has them (names and notetracks came first)
        for (const auto& stream : std::span(streams).subspan(2))
            if (stream.count > 0)
                take(std::uint64_t{stream.count} * stream.width, stream.width, stream.pointer);
        if (overrun)
            return fail("its pieces go past its end");
        if (from != parts.size())
            return fail(std::format("it has {} bytes after its last piece", parts.size() - from));
        layout.size = Align(to, 4);
        return layout;
    }

    bool Write(const Layout& layout, std::span<const std::uint8_t> parts, std::size_t stringCount, std::uint8_t* memory,
               const std::function<std::uint16_t(std::uint16_t)>& scriptString)
    {
        static_assert(sizeof(std::uintptr_t) == 4, "the game's pointers are 32 bits");
        std::memset(memory, 0, layout.size);
        for (const auto& piece : layout.pieces)
            std::memcpy(memory + piece.to, parts.data() + piece.from, piece.size);
        for (const auto& pointer : layout.pointers)
        {
            const auto address = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(memory + pointer.to));
            std::memcpy(memory + pointer.at, &address, sizeof(address));
        }
        // the names: {u16} each, then the notetracks' {u16 name, u16, float time}
        const auto rename = [&](std::uint32_t at) {
            std::uint16_t index = 0;
            std::memcpy(&index, memory + at, sizeof(index));
            if (index >= stringCount)
                return false;
            const auto name = scriptString(index);
            std::memcpy(memory + at, &name, sizeof(name));
            return true;
        };
        for (std::uint32_t i = 0; i < layout.nameCount; ++i)
            if (!rename(layout.names + 2 * i))
                return false;
        for (std::uint32_t i = 0; i < layout.notifyCount; ++i)
            if (!rename(layout.notify + 8 * i))
                return false;
        return true;
    }

    std::vector<std::string> ChooseUnloads(std::span<const Cached> cached, std::uint64_t budget)
    {
        std::uint64_t total = 0;
        for (const auto& each : cached)
            total += each.bytes;
        if (total <= budget)
            return {};
        std::vector<const Cached*> candidates;
        for (const auto& each : cached)
            if (each.references == 0 && !each.wanted)
                candidates.push_back(&each);
        std::ranges::sort(candidates, [](const Cached* a, const Cached* b) {
            return std::tie(a->lastUsed, a->key) < std::tie(b->lastUsed, b->key);
        });
        std::vector<std::string> unloads;
        for (const auto candidate : candidates)
        {
            if (total <= budget)
                break;
            unloads.push_back(candidate->key);
            total -= candidate->bytes;
        }
        return unloads;
    }
}  // namespace IWXMVM::AnimationPackLogic
