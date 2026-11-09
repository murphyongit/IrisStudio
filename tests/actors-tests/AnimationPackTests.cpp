#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/AnimationPackLogic.hpp"
#include "Utilities/Inflate.hpp"

#include "nlohmann/json.hpp"

#include <cstring>
#include <malloc.h>
#include <memory>
#include <set>
#include <sstream>

using namespace IWXMVM;

// The animation pack and the store's part of it that needs no game (AnimationPackLogic, Inflate):
// reading the pack, inflating its entries, laying each out as the game's loader lays
// an XAnimParts out, and which animations the store unloads. On a slice of the user's pack (make_pack_slice.py in the
// step's notes) with what tools/t5-characters/animations.py reads of each entry.

namespace
{
    namespace Pack = AnimationPackLogic;
    namespace Offsets = AnimationPackLogic::Offsets;

    const std::filesystem::path DATA = std::filesystem::path(__FILE__).parent_path() / "data";
    const std::filesystem::path SHIPPED_CATALOG =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" / "resources" /
        "animations.json";

    nlohmann::json ReadJson(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        return nlohmann::json::parse(file);
    }

    const nlohmann::json& Expected()
    {
        static const auto json = ReadJson(DATA / "animation-pack-slice.json");
        return json;
    }

    std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::vector<std::uint8_t> FromHex(std::string_view hex)
    {
        std::vector<std::uint8_t> bytes;
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
            bytes.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
        return bytes;
    }

    std::uint32_t Fnv1a(std::span<const std::uint8_t> data)
    {
        std::uint32_t hash = 0x811C9DC5;
        for (const auto byte : data)
            hash = (hash ^ byte) * 0x01000193;
        return hash;
    }

    // an entry of the slice, inflated
    std::optional<std::vector<std::uint8_t>> Inflated(const std::string& key)
    {
        static const auto pack = ReadBytes(DATA / "animation-pack-slice.pack");
        std::ifstream stream(DATA / "animation-pack-slice.pack", std::ios::binary);
        const auto index = Pack::ReadIndex(stream);
        const auto it = index.find(key);
        if (it == index.end() || std::uint64_t{it->second.offset} + it->second.size > pack.size())
            return std::nullopt;
        return Inflate::Zlib(std::span(pack).subspan(it->second.offset, it->second.size), it->second.inflated);
    }

    template <typename T>
    T At(const std::uint8_t* memory, std::uint32_t offset)
    {
        T value{};
        std::memcpy(&value, memory + offset, sizeof(T));
        return value;
    }

    // what a pointer of laid out parts points to, relative to the memory; none for null
    std::optional<std::uint32_t> Target(const std::uint8_t* memory, std::uint32_t at)
    {
        const auto pointer = At<std::uint32_t>(memory, at);
        if (pointer == 0)
            return std::nullopt;
        return static_cast<std::uint32_t>(pointer -
                                          static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(memory)));
    }

    struct AlignedFree
    {
        void operator()(std::uint8_t* memory) const
        {
            _aligned_free(memory);
        }
    };
    using Memory = std::unique_ptr<std::uint8_t, AlignedFree>;

    // an entry laid out with the index of each name's string plus 1000 for its script string
    constexpr std::uint16_t STRING_BASE = 1000;
    struct LaidOut
    {
        Pack::Layout layout;
        Pack::Entry entry;
        std::vector<std::uint8_t> inflated;
        Memory memory;
    };
    std::optional<LaidOut> LayOut(const std::string& key)
    {
        auto inflated = Inflated(key);
        if (!inflated)
            return std::nullopt;
        LaidOut out;
        out.inflated = std::move(*inflated);
        auto entry = Pack::ReadEntry(out.inflated);
        if (!entry)
            return std::nullopt;
        out.entry = std::move(*entry);
        auto layout = Pack::Plan(out.entry.parts);
        if (!layout)
            return std::nullopt;
        out.layout = std::move(*layout);
        out.memory.reset(static_cast<std::uint8_t*>(_aligned_malloc(out.layout.size, Pack::MEMORY_ALIGNMENT)));
        if (!Pack::Write(out.layout, out.entry.parts, out.entry.strings.size(), out.memory.get(),
                         [](std::uint16_t index) { return static_cast<std::uint16_t>(STRING_BASE + index); }))
            return std::nullopt;
        return out;
    }
}  // namespace

TEST("Inflate: zlib streams of every kind of block inflate to what was packed, and nothing else does")
{
    const auto cases = ReadJson(DATA / "inflate-cases.json");
    CHECK(cases.size() == 6);
    std::set<int> blockTypes;
    for (const auto& each : cases)
    {
        const auto raw = FromHex(each["raw"].get<std::string>());
        auto packed = FromHex(each["zlib"].get<std::string>());
        blockTypes.insert(each["firstBlock"].get<int>());
        const auto inflated = Inflate::Zlib(packed, raw.size());
        CHECK(inflated && *inflated == raw);
        // the size expected is the size
        CHECK(!Inflate::Zlib(packed, raw.size() + 1));
        if (!raw.empty())
            CHECK(!Inflate::Zlib(packed, raw.size() - 1));
        // cut short
        CHECK(!Inflate::Zlib(std::span(packed).first(packed.size() - 1), raw.size()));
        // a byte changed: what it inflates to isn't its checksum's (or it isn't deflate)
        if (!raw.empty())
        {
            packed[packed.size() / 2] ^= 0x10;
            const auto changed = Inflate::Zlib(packed, raw.size());
            CHECK(!changed);
        }
    }
    // stored, fixed and dynamic blocks
    CHECK(blockTypes == std::set<int>({0, 1, 2}));
    // not zlib
    CHECK(!Inflate::Zlib(std::vector<std::uint8_t>{0x1F, 0x8B, 0x08, 0, 0, 0}, 0));
}

TEST("Animation pack: the index lists each entry with the catalog's hash")
{
    std::ifstream stream(DATA / "animation-pack-slice.pack", std::ios::binary);
    const auto index = Pack::ReadIndex(stream);
    const auto& hashes = Expected()["hashes"];
    CHECK(index.size() == hashes.size());
    CHECK(index.size() == 13);
    // as the shipped catalog has them, by name (and the copy that differs by its own)
    const auto catalog = ReadJson(SHIPPED_CATALOG);
    std::unordered_map<std::string, std::string> catalogHashes;
    for (const auto& entry : catalog["animations"])
        catalogHashes[entry["n"].get<std::string>()] = entry["h"].get<std::string>();
    for (const auto& [key, hash] : hashes.items())
    {
        const auto it = index.find(key);
        CHECK(it != index.end());
        if (it == index.end())
            continue;
        CHECK(Pack::ParseHash(hash.get<std::string>()) == it->second.hash);
        CHECK(catalogHashes.contains(key) && Pack::ParseHash(catalogHashes[key]) == it->second.hash);
    }
    CHECK(!Pack::ParseHash("8d838646368b9c2"));
    CHECK(!Pack::ParseHash("8d838646368b9c2x"));

    std::istringstream notAPack(std::string("IWXMVMAQ\x01\0\0\0\0\0\0\0", 16));
    bool threw = false;
    try
    {
        Pack::ReadIndex(notAPack);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);
}

TEST("Animation pack: each entry inflates to its size and reads back with its strings")
{
    for (const auto& [key, expected] : Expected()["entries"].items())
    {
        const auto inflated = Inflated(key);
        CHECK(inflated.has_value());
        if (!inflated)
            continue;
        CHECK(inflated->size() == expected["inflated"].get<std::size_t>());
        CHECK(Fnv1a(*inflated) == expected["fnv"].get<std::uint32_t>());
        const auto entry = Pack::ReadEntry(*inflated);
        CHECK(entry && entry->parts.size() == expected["partsSize"].get<std::size_t>());
        if (entry && expected.contains("strings"))
            CHECK(entry->strings == expected["strings"].get<std::vector<std::string>>());
        // with a byte too many or too few
        auto longer = *inflated;
        longer.push_back(0);
        CHECK(!Pack::ReadEntry(longer));
        CHECK(!Pack::ReadEntry(std::span(*inflated).first(inflated->size() - 1)));
    }
}

TEST("Animation pack: entries lay out as the game's loader lays out an XAnimParts, pointers and names set")
{
    std::size_t laidOut = 0;
    for (const auto& [key, expected] : Expected()["entries"].items())
    {
        if (key == "mp_cosmodrome")
            continue;
        const auto out = LayOut(key);
        CHECK(out.has_value());
        if (!out)
        {
            std::printf("    %s didn't lay out\n", key.c_str());
            continue;
        }
        ++laidOut;
        const auto memory = out->memory.get();
        const auto& layout = out->layout;
        CHECK(reinterpret_cast<std::uintptr_t>(memory) % Pack::MEMORY_ALIGNMENT == 0);
        CHECK(layout.name == expected["name"].get<std::string>());
        CHECK(layout.frames == expected["frames"].get<std::uint16_t>());
        CHECK(layout.loop == expected["loop"].get<bool>());
        CHECK(layout.delta == expected["delta"].get<bool>());
        CHECK_NEAR(layout.rate, expected["rate"].get<float>(), 1e-4f);

        // every pointer within the memory, aligned as the loader aligns what it points to, or null as in the pack
        const bool longIndices = layout.frames >= 256;
        const std::array<std::pair<std::uint32_t, std::uint32_t>, 11> alignments = {{
            {Offsets::NAME, 1},
            {Offsets::NAMES, 2},
            {Offsets::DATA_BYTE, 1},
            {Offsets::DATA_SHORT, 2},
            {Offsets::DATA_INT, 4},
            {Offsets::RANDOM_DATA_SHORT, 2},
            {Offsets::RANDOM_DATA_BYTE, 1},
            {Offsets::RANDOM_DATA_INT, 4},
            {Offsets::INDICES, longIndices ? 2u : 1u},
            {Offsets::NOTIFY, 4},
            {Offsets::DELTA_PART, 4},
        }};
        for (const auto& [at, alignment] : alignments)
        {
            const auto inPack = At<std::uint32_t>(out->entry.parts.data(), at);
            const auto target = Target(memory, at);
            CHECK((inPack == Pack::FOLLOWS) == target.has_value());
            if (target)
                CHECK(*target >= Pack::PARTS_SIZE && *target < layout.size && *target % alignment == 0);
        }
        CHECK(std::string(reinterpret_cast<const char*>(memory + *Target(memory, Offsets::NAME))) == layout.name);

        // the counts as they were
        const auto boneCount = At<std::array<std::uint8_t, 10>>(memory, Offsets::BONE_COUNT);
        CHECK(std::vector<std::uint8_t>(boneCount.begin(), boneCount.end()) ==
              expected["boneCount"].get<std::vector<std::uint8_t>>());

        // the bones' and notetracks' names are the script strings of theirs
        const auto bones = expected["bones"].get<std::vector<std::string>>();
        CHECK(bones.size() == boneCount[9]);
        const auto& strings = out->entry.strings;
        if (const auto names = Target(memory, Offsets::NAMES))
            for (std::size_t i = 0; i < bones.size(); ++i)
            {
                const auto name = At<std::uint16_t>(memory, *names + 2 * static_cast<std::uint32_t>(i));
                CHECK(name >= STRING_BASE && name - STRING_BASE < strings.size() &&
                      strings[name - STRING_BASE] == bones[i]);
            }
        const auto notetracks = expected["notetracks"];
        CHECK(notetracks.size() == memory[Offsets::NOTIFY_COUNT]);
        if (const auto notify = Target(memory, Offsets::NOTIFY))
            for (std::size_t i = 0; i < notetracks.size(); ++i)
            {
                const auto at = *notify + 8 * static_cast<std::uint32_t>(i);
                const auto name = At<std::uint16_t>(memory, at);
                CHECK(name >= STRING_BASE && strings[name - STRING_BASE] == notetracks[i][0].get<std::string>());
                CHECK(At<std::uint16_t>(memory, at + 2) == 0);
                CHECK_NEAR(At<float>(memory, at + 4), notetracks[i][1].get<float>(), 1e-6f);
            }

        // the delta part: the first and last keys of its translation and rotation as animations.py reads them
        if (const auto delta = Target(memory, Offsets::DELTA_PART))
        {
            const auto trans = Target(memory, *delta);
            CHECK(trans.has_value() == expected.contains("transSize"));
            if (trans)
            {
                CHECK(*trans % 4 == 0);
                const auto size = At<std::uint16_t>(memory, *trans);
                const bool smallKeys = memory[*trans + 2] != 0;
                CHECK(size == expected["transSize"].get<std::uint16_t>() && smallKeys == expected["small"].get<bool>());
                std::array<float, 3> first{}, last{};
                if (size == 0)
                {
                    first = last = At<std::array<float, 3>>(memory, *trans + 4);
                }
                else
                {
                    const auto mins = At<std::array<float, 3>>(memory, *trans + 4);
                    const auto scale = At<std::array<float, 3>>(memory, *trans + 16);
                    const auto keys = Target(memory, *trans + 0x1C);
                    CHECK(keys && *keys % (smallKeys ? 1u : 4u) == 0 && *keys < layout.size);
                    for (int axis = 0; axis < 3 && keys; ++axis)
                    {
                        const auto key = [&](std::uint32_t frame) -> float {
                            return smallKeys ? memory[*keys + frame * 3 + axis]
                                             : At<std::uint16_t>(memory, *keys + frame * 6 + 2 * axis);
                        };
                        first[axis] = mins[axis] + scale[axis] * key(0);
                        last[axis] = mins[axis] + scale[axis] * key(size);
                    }
                }
                for (int axis = 0; axis < 3; ++axis)
                {
                    CHECK_NEAR(first[axis], expected["transFirst"][axis].get<float>(), 1e-3f);
                    CHECK_NEAR(last[axis], expected["transLast"][axis].get<float>(), 1e-3f);
                }
            }
            const auto quat = Target(memory, *delta + 4);
            CHECK(quat.has_value() == expected.contains("quatSize"));
            if (quat)
            {
                const auto size = At<std::uint16_t>(memory, *quat);
                CHECK(size == expected["quatSize"].get<std::uint16_t>());
                std::array<std::int16_t, 2> first{}, last{};
                if (size == 0)
                {
                    first = last = At<std::array<std::int16_t, 2>>(memory, *quat + 4);
                }
                else
                {
                    const auto keys = Target(memory, *quat + 4);
                    CHECK(keys && *keys % 4 == 0);
                    if (keys)
                    {
                        first = At<std::array<std::int16_t, 2>>(memory, *keys);
                        last = At<std::array<std::int16_t, 2>>(memory, *keys + 4 * size);
                    }
                }
                CHECK(first[0] == expected["quatFirst"][0].get<int>() &&
                      first[1] == expected["quatFirst"][1].get<int>());
                CHECK(last[0] == expected["quatLast"][0].get<int>() && last[1] == expected["quatLast"][1].get<int>());
            }
        }
        else
        {
            CHECK(!expected.contains("transSize") && !expected.contains("quatSize"));
        }

        // the data streams as they were in the pack, from one byte after the other
        const auto counts = expected["counts"].get<std::vector<std::uint32_t>>();
        const std::array<std::pair<std::uint32_t, std::uint32_t>, 7> streams = {{
            {Offsets::DATA_BYTE, counts[0]},
            {Offsets::DATA_SHORT, counts[1] * 2},
            {Offsets::DATA_INT, counts[2] * 4},
            {Offsets::RANDOM_DATA_SHORT, counts[6] * 2},
            {Offsets::RANDOM_DATA_BYTE, counts[3]},
            {Offsets::RANDOM_DATA_INT, counts[4] * 4},
            {Offsets::INDICES, counts[7] * (longIndices ? 2u : 1u)},
        }};
        std::uint32_t end = 0;
        for (const auto& piece : layout.pieces)
            end = std::max(end, piece.from + piece.size);
        CHECK(end == out->entry.parts.size());
        std::uint32_t streamBytes = 0;
        for (const auto& [at, bytes] : streams)
        {
            streamBytes += bytes;
            const auto target = Target(memory, at);
            CHECK(target.has_value() == (bytes > 0));
            if (!target)
                continue;
            const auto piece = std::ranges::find(layout.pieces, *target, &Pack::Layout::Piece::to);
            CHECK(piece != layout.pieces.end() && piece->size == bytes);
            if (piece != layout.pieces.end())
                CHECK(std::memcmp(memory + *target, out->entry.parts.data() + piece->from, bytes) == 0);
        }
        // they are the zone's last pieces, in that order
        CHECK(layout.pieces.back().from + layout.pieces.back().size == out->entry.parts.size());
        CHECK(streamBytes <= out->entry.parts.size());
    }
    CHECK(laidOut == 12);
}

TEST("Animation pack: what doesn't read as an XAnimParts isn't laid out")
{
    // the zone reader took this for parts (its name is a map's): its bone counts don't add up
    const auto inflated = Inflated("mp_cosmodrome");
    CHECK(inflated.has_value());
    if (inflated)
    {
        const auto entry = Pack::ReadEntry(*inflated);
        std::string why;
        CHECK(entry && !Pack::Plan(entry->parts, &why) && why.find("bone counts") != std::string::npos);
    }

    const auto good = Inflated("pt_reload_stand_auto");
    CHECK(good.has_value());
    if (!good)
        return;
    const auto entry = *Pack::ReadEntry(*good);
    CHECK(Pack::Plan(entry.parts).has_value());
    std::vector<std::uint8_t> parts(entry.parts.begin(), entry.parts.end());
    std::string why;
    // cut short, or with a byte after its last piece
    CHECK(!Pack::Plan(std::span(parts).first(parts.size() - 1), &why) && why.find("past its end") != std::string::npos);
    auto longer = parts;
    longer.push_back(0);
    CHECK(!Pack::Plan(longer, &why) && why.find("after its last piece") != std::string::npos);
    // a pointer that is neither -1 nor 0, or one that doesn't go with its count
    auto shared = parts;
    const std::uint32_t elsewhere = 0x1234;
    std::memcpy(shared.data() + Offsets::DATA_SHORT, &elsewhere, 4);
    CHECK(!Pack::Plan(shared, &why) && why.find("doesn't go with") != std::string::npos);
    auto none = parts;
    std::memset(none.data() + Offsets::DATA_SHORT, 0, 4);
    CHECK(!Pack::Plan(none));
    CHECK(!Pack::Plan(std::span(parts).first(Pack::PARTS_SIZE - 1)));

    // a name's index past the entry's strings isn't written
    const auto layout = *Pack::Plan(entry.parts);
    Memory memory(static_cast<std::uint8_t*>(_aligned_malloc(layout.size, Pack::MEMORY_ALIGNMENT)));
    CHECK(!Pack::Write(layout, entry.parts, 1, memory.get(), [](std::uint16_t index) { return index; }));
    CHECK(Pack::Write(layout, entry.parts, entry.strings.size(), memory.get(),
                      [](std::uint16_t index) { return index; }));
}

TEST("Animation store: unloads the least recently used first, never one a clip uses or the scene names")
{
    using Cached = Pack::Cached;
    const std::vector<Cached> cached = {
        {.key = "a", .bytes = 400, .references = 0, .wanted = false, .lastUsed = 5},
        {.key = "b", .bytes = 300, .references = 2, .wanted = false, .lastUsed = 1},
        {.key = "c", .bytes = 200, .references = 0, .wanted = true, .lastUsed = 2},
        {.key = "d", .bytes = 100, .references = 0, .wanted = false, .lastUsed = 3},
        {.key = "e", .bytes = 100, .references = 0, .wanted = false, .lastUsed = 9},
    };
    // 1100 in all: within the budget, none
    CHECK(Pack::ChooseUnloads(cached, 1100).empty());
    // the oldest of those unused first, as few as bring it within
    CHECK(Pack::ChooseUnloads(cached, 1000) == std::vector<std::string>({"d"}));
    CHECK(Pack::ChooseUnloads(cached, 900) == std::vector<std::string>({"d", "a"}));
    CHECK(Pack::ChooseUnloads(cached, 600) == std::vector<std::string>({"d", "a"}));
    // with what is used alone over the budget, it stays over: b (referenced) and c (wanted) stay
    CHECK(Pack::ChooseUnloads(cached, 0) == std::vector<std::string>({"d", "a", "e"}));
    CHECK(Pack::ChooseUnloads({}, 0).empty());
}

TEST("Animation store: with a budget of 1 MB, the animations clips play stay however many are loaded")
{
    // the slice's animations as the store would hold them: what each takes laid out, the first four played by clips
    std::vector<Pack::Cached> cached;
    std::uint64_t played = 0, total = 0;
    std::uint64_t used = 0;
    for (const auto& [key, expected] : Expected()["entries"].items())
    {
        if (key == "mp_cosmodrome")
            continue;
        const auto out = LayOut(key);
        if (!out)
            continue;
        const bool clip = cached.size() < 4;
        cached.push_back({.key = key, .bytes = out->layout.size, .references = clip ? 1u : 0u, .lastUsed = ++used});
        played += clip ? out->layout.size : 0;
        total += out->layout.size;
    }
    CHECK(cached.size() == 12);
    // larger than a megabyte, with the copies a real scene would have
    for (int copy = 0; total < 3 * 1024 * 1024; ++copy)
        for (std::size_t i = 4; i < 12; ++i)
        {
            auto more = cached[i];
            more.key += std::format(" copy {}", copy);
            more.lastUsed = ++used;
            cached.push_back(more);
            total += more.bytes;
        }
    const auto unloads = Pack::ChooseUnloads(cached, 1024 * 1024);
    std::uint64_t left = total;
    for (const auto& key : unloads)
    {
        const auto it = std::ranges::find(cached, key, &Pack::Cached::key);
        CHECK(it != cached.end() && it->references == 0);
        left -= it->bytes;
    }
    CHECK(left <= 1024 * 1024 && left >= played);
    // what is left took less time ago than anything unloaded
    std::uint64_t newestUnloaded = 0;
    for (const auto& key : unloads)
        newestUnloaded = std::max(newestUnloaded, std::ranges::find(cached, key, &Pack::Cached::key)->lastUsed);
    for (const auto& each : cached)
        if (each.references == 0 && std::ranges::find(unloads, each.key) == unloads.end())
            CHECK(each.lastUsed > newestUnloaded);
}
