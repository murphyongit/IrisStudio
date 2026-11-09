#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/AnimationPackLogic.hpp"
#include "Setup/PackWriter.hpp"
#include "Utilities/Deflate.hpp"
#include "Utilities/Inflate.hpp"
#include "Utilities/Sha1.hpp"

#include <cstring>
#include <random>
#include <set>
#include <sstream>

using namespace IWXMVM;

// Writing the animation pack in C++ (Setup/PackWriter, Utilities/Deflate and Sha1),
// as tools/t5-characters/animations.py writes it: the entries are found in a zone made of the pack slice's own
// entries (from the user's pack, see AnimationPackTests.cpp), normalized back to the same bytes and hashes

namespace
{
    namespace Pack = AnimationPackLogic;
    namespace Writer = Setup::PackWriter;

    const std::filesystem::path DATA = std::filesystem::path(__FILE__).parent_path() / "data";

    std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    std::string Hex(std::span<const std::uint8_t> bytes)
    {
        std::string text;
        for (const auto byte : bytes)
            text += std::format("{:02x}", byte);
        return text;
    }

    std::span<const std::uint8_t> Bytes(std::string_view text)
    {
        return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
    }

    bool RoundTrips(const std::vector<std::uint8_t>& data)
    {
        const auto packed = Deflate::Zlib(data);
        const auto back = Inflate::Zlib(packed, data.size());
        return back && *back == data;
    }

    // every entry of the slice, inflated, by key
    std::map<std::string, std::vector<std::uint8_t>> SliceEntries()
    {
        const auto pack = ReadBytes(DATA / "animation-pack-slice.pack");
        std::ifstream stream(DATA / "animation-pack-slice.pack", std::ios::binary);
        std::map<std::string, std::vector<std::uint8_t>> entries;
        for (const auto& [key, entry] : Pack::ReadIndex(stream))
            if (auto inflated = Inflate::Zlib(std::span(pack).subspan(entry.offset, entry.size), entry.inflated))
                entries.emplace(key, std::move(*inflated));
        return entries;
    }

    // A zone as the game writes one: the XFile header, the script strings, then each entry's parts with its names
    // as indices into the zone's strings, with bytes between them that aren't parts
    std::vector<std::uint8_t> ZoneOf(const std::vector<std::vector<std::uint8_t>>& entries)
    {
        std::vector<std::string> strings = {"", "tag_origin"};
        std::vector<std::vector<std::uint8_t>> parts;
        for (const auto& inflated : entries)
        {
            const auto entry = Pack::ReadEntry(inflated);
            std::vector<std::uint8_t> bytes(entry->parts.begin(), entry->parts.end());
            // the names point into the zone's strings: the entry's own after those of the entries before it
            const auto base = static_cast<std::uint16_t>(strings.size());
            strings.insert(strings.end(), entry->strings.begin(), entry->strings.end());
            const auto bones = bytes[0x18 + 9];
            const auto notes = bytes[0x22];
            auto at = Pack::PARTS_SIZE;
            while (bytes[at] != 0)
                ++at;
            ++at;
            for (std::size_t i = 0; i < bones; ++i, at += 2)
            {
                std::uint16_t id = 0;
                std::memcpy(&id, bytes.data() + at, 2);
                id = static_cast<std::uint16_t>(id + base);
                std::memcpy(bytes.data() + at, &id, 2);
            }
            for (std::size_t i = 0; i < notes; ++i, at += 8)
            {
                std::uint16_t id = 0;
                std::memcpy(&id, bytes.data() + at, 2);
                id = static_cast<std::uint16_t>(id + base);
                std::memcpy(bytes.data() + at, &id, 2);
                bytes[at + 2] = 0x7F;  // the zone's half of {u16 name, u16}, which the pack zeroes
            }
            bytes[0x25] = 0x11;  // the struct's padding, which the pack zeroes too
            parts.push_back(std::move(bytes));
        }

        std::vector<std::uint8_t> zone(36, 0);
        const auto put = [&zone](std::uint32_t value) {
            for (int i = 0; i < 4; ++i)
                zone.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        };
        put(static_cast<std::uint32_t>(strings.size()));
        put(0);
        put(static_cast<std::uint32_t>(parts.size()));
        put(0);
        for (std::size_t i = 0; i < strings.size(); ++i)
            put(i == 0 ? 0 : 0xFFFFFFFF);
        for (std::size_t i = 1; i < strings.size(); ++i)
        {
            zone.insert(zone.end(), strings[i].begin(), strings[i].end());
            zone.push_back(0);
        }
        // a run of 0xFF before the first parts, and assets that look like them but aren't
        zone.insert(zone.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x01});
        for (const auto& bytes : parts)
        {
            zone.insert(zone.end(), bytes.begin(), bytes.end());
            zone.insert(zone.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x2A});
        }
        return zone;
    }
}  // namespace

TEST("SHA-1 of the standard's examples")
{
    CHECK(Hex(Sha1::Of({})) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(Hex(Sha1::Of(Bytes("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(Hex(Sha1::Of(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    const std::string million(1000000, 'a');
    CHECK(Hex(Sha1::Of(Bytes(million))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    // the lengths about a block's end, where the padding takes a second block
    for (std::size_t length = 54; length <= 66; ++length)
    {
        const std::string text(length, 'x');
        CHECK(Sha1::Of(Bytes(text)) != Sha1::Of(Bytes(text + "x")));
    }
}

TEST("deflate round-trips through the mod's inflate: empty, tiny, random, repeated, long runs")
{
    CHECK(RoundTrips({}));
    CHECK(RoundTrips({42}));
    CHECK(RoundTrips({1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3}));
    std::mt19937 random(5);
    std::vector<std::uint8_t> noise(200000);
    for (auto& byte : noise)
        byte = static_cast<std::uint8_t>(random());
    CHECK(RoundTrips(noise));
    // noise doesn't grow by more than its stored blocks' headers
    CHECK(Deflate::Zlib(noise).size() < noise.size() + noise.size() / 1000 + 64);
    std::vector<std::uint8_t> zeros(3000000, 0);
    CHECK(RoundTrips(zeros));
    CHECK(Deflate::Zlib(zeros).size() < 10000);
    std::string text;
    for (int i = 0; i < 20000; ++i)
        text += std::format("j_spine{} tag_weapon_{} {}\n", i % 7, i % 13, i * 31 % 1000);
    const std::vector<std::uint8_t> repeated(text.begin(), text.end());
    CHECK(RoundTrips(repeated));
    CHECK(Deflate::Zlib(repeated).size() < repeated.size() / 4);
    // a few symbols only, so a block's codes are short or single
    std::vector<std::uint8_t> two(70000);
    for (std::size_t i = 0; i < two.size(); ++i)
        two[i] = random() % 50 == 0 ? 'b' : 'a';
    CHECK(RoundTrips(two));
}

TEST("deflate round-trips the slice's entries, smaller than they are")
{
    std::size_t inflated = 0, packed = 0;
    for (const auto& [key, entry] : SliceEntries())
    {
        CHECK(RoundTrips(entry));
        inflated += entry.size();
        packed += Deflate::Zlib(entry).size();
    }
    CHECK(inflated > 0);
    CHECK(packed < inflated);
}

TEST("the pack's entries are found in a zone, as animations.py normalizes them, with the index's hashes")
{
    const auto slice = SliceEntries();
    std::ifstream stream(DATA / "animation-pack-slice.pack", std::ios::binary);
    const auto index = Pack::ReadIndex(stream);
    // one entry a name, as a zone has them
    std::vector<std::string> keys;
    std::vector<std::vector<std::uint8_t>> entries;
    std::set<std::string> names;
    for (const auto& [key, entry] : slice)
    {
        const auto name = key.substr(0, key.find(' '));
        if (names.insert(name).second)
        {
            keys.push_back(key);
            entries.push_back(entry);
        }
    }
    CHECK(entries.size() >= 10);

    const auto zone = ZoneOf(entries);
    std::map<std::string, std::vector<std::uint8_t>> found;
    Writer::ForEachAnimation(zone, [&found](std::string_view name, std::span<const std::uint8_t> entry) {
        found.emplace(std::string(name), std::vector<std::uint8_t>(entry.begin(), entry.end()));
    });
    CHECK(found.size() == entries.size());
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        const auto name = keys[i].substr(0, keys[i].find(' '));
        const auto it = found.find(name);
        CHECK(it != found.end());
        if (it == found.end())
            continue;
        CHECK(it->second == entries[i]);
        CHECK(Writer::EntryHash(it->second) == index.at(keys[i]).hash);
    }

    // a second copy of a name in the same zone is a false find
    auto twice = entries;
    twice.push_back(entries[0]);
    std::size_t count = 0;
    Writer::ForEachAnimation(ZoneOf(twice), [&count](std::string_view, std::span<const std::uint8_t>) { ++count; });
    CHECK(count == entries.size());
}

TEST("a fastfile inflates to its zone; other files don't")
{
    const auto zone = ZoneOf({SliceEntries().begin()->second});
    std::vector<std::uint8_t> file = {'I', 'W', 'f', 'f', 'u', '1', '0', '0', 0xD9, 0x01, 0x00, 0x00};
    const auto packed = Deflate::Zlib(zone);
    file.insert(file.end(), packed.begin(), packed.end());
    const auto inflated = Writer::InflateFastfile(file, 1 << 24);
    CHECK(inflated && *inflated == zone);
    CHECK(!Writer::InflateFastfile(file, zone.size() - 1).has_value());
    auto otherVersion = file;
    otherVersion[8] = 0xDA;
    CHECK(!Writer::InflateFastfile(otherVersion, 1 << 24).has_value());
    CHECK(!Writer::InflateFastfile(std::vector<std::uint8_t>(packed), 1 << 24).has_value());
    CHECK(Writer::ScriptStrings(zone).size() > 2);
    CHECK(Writer::ScriptStrings(zone)[1] == "tag_origin");
}

TEST("the pack's header and index read back with the offsets of the entries after them")
{
    std::vector<Writer::IndexRecord> records = {{"pb_stand_alert", 100, 300, {1, 2, 3, 4, 5, 6, 7, 8}},
                                                {"pb_stand_alert (cuba)", 50, 120, {9, 9, 9, 9, 9, 9, 9, 9}},
                                                {"ai_dog_idle", 7, 9, {}}};
    const auto header = Writer::Header(records);
    std::istringstream stream(std::string(header.begin(), header.end()));
    const auto index = Pack::ReadIndex(stream);
    CHECK(index.size() == 3);
    CHECK(index.at("pb_stand_alert").offset == header.size());
    CHECK(index.at("pb_stand_alert (cuba)").offset == header.size() + 100);
    CHECK(index.at("ai_dog_idle").offset == header.size() + 150);
    CHECK(index.at("pb_stand_alert (cuba)").size == 50);
    CHECK(index.at("pb_stand_alert (cuba)").inflated == 120);
    CHECK(index.at("pb_stand_alert").hash == records[0].hash);
}
