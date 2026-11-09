#include "StdInclude.hpp"
#include "Test.hpp"

#include "Setup/Zip.hpp"
#include "Utilities/Deflate.hpp"

#include <cstring>

using namespace IWXMVM;

// Unpacking OpenAssetTools' release zip (Setup/Zip): a zip made here with an entry
// stored, one deflated (by Deflate, as PowerShell's Compress-Archive and GitHub's release zips deflate), and a folder

namespace
{
    struct ZipFile
    {
        std::string name;
        std::vector<std::uint8_t> data;
        bool deflate = false;
    };

    void Put16(std::vector<std::uint8_t>& out, std::uint32_t value)
    {
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
    }

    void Put32(std::vector<std::uint8_t>& out, std::uint32_t value)
    {
        Put16(out, value & 0xFFFF);
        Put16(out, value >> 16);
    }

    std::vector<std::uint8_t> MakeZip(const std::vector<ZipFile>& files)
    {
        std::vector<std::uint8_t> zip, directory;
        for (const auto& file : files)
        {
            std::vector<std::uint8_t> packed = file.data;
            if (file.deflate)
            {
                // a bare deflate stream: zlib's without its 2 byte header and 4 byte checksum
                const auto zlib = Deflate::Zlib(file.data);
                packed.assign(zlib.begin() + 2, zlib.end() - 4);
            }
            const auto crc = Setup::Zip::Crc32(file.data);
            const auto local = static_cast<std::uint32_t>(zip.size());
            for (auto* out : {&zip, &directory})
            {
                const bool central = out == &directory;
                Put32(*out, central ? 0x02014B50 : 0x04034B50);
                if (central)
                    Put16(*out, 20);  // made by
                Put16(*out, 20);      // needed
                Put16(*out, 0);       // flags
                Put16(*out, file.deflate ? 8 : 0);
                Put32(*out, 0);  // time and date
                Put32(*out, crc);
                Put32(*out, static_cast<std::uint32_t>(packed.size()));
                Put32(*out, static_cast<std::uint32_t>(file.data.size()));
                Put16(*out, static_cast<std::uint32_t>(file.name.size()));
                Put16(*out, central ? 0 : 4);  // the local header has an extra field the directory hasn't
                if (central)
                {
                    Put16(*out, 0);  // comment
                    Put16(*out, 0);  // disk
                    Put16(*out, 0);  // internal attributes
                    Put32(*out, 0);  // external attributes
                    Put32(*out, local);
                }
                out->insert(out->end(), file.name.begin(), file.name.end());
                if (!central)
                {
                    Put32(*out, 0xCAFE0000);
                    out->insert(out->end(), packed.begin(), packed.end());
                }
            }
        }
        const auto directoryAt = static_cast<std::uint32_t>(zip.size());
        zip.insert(zip.end(), directory.begin(), directory.end());
        Put32(zip, 0x06054B50);
        Put16(zip, 0);
        Put16(zip, 0);
        Put16(zip, static_cast<std::uint32_t>(files.size()));
        Put16(zip, static_cast<std::uint32_t>(files.size()));
        Put32(zip, static_cast<std::uint32_t>(directory.size()));
        Put32(zip, directoryAt);
        Put16(zip, 0);
        return zip;
    }

    std::vector<std::uint8_t> Text(std::string_view text)
    {
        return {text.begin(), text.end()};
    }
}  // namespace

TEST("a zip's entries, stored and deflated, with their names and data")
{
    std::string linker;
    for (int i = 0; i < 5000; ++i)
        linker += std::format("Linker {} ", i % 97);
    const auto zip = MakeZip({{"raw/", {}}, {"Linker.exe", Text(linker), true}, {"raw/readme.txt", Text("hi"), false}});
    const auto entries = Setup::Zip::Read(zip, 1 << 20);
    CHECK(entries.has_value());
    if (!entries)
        return;
    CHECK(entries->size() == 3);
    CHECK((*entries)[0].name == "raw/");
    CHECK((*entries)[0].data.empty());
    CHECK((*entries)[1].name == "Linker.exe");
    CHECK((*entries)[1].data == Text(linker));
    CHECK((*entries)[2].name == "raw/readme.txt");
    CHECK((*entries)[2].data == Text("hi"));
}

TEST("a zip that doesn't check out gives nothing: a wrong CRC, cut short, too big, not a zip")
{
    const auto zip = MakeZip({{"a.txt", Text("some text, some text, some text"), true}});
    CHECK(Setup::Zip::Read(zip, 1 << 20).has_value());
    // the CRC of the central directory's entry
    auto badCrc = zip;
    constexpr std::array<std::uint8_t, 4> CENTRAL = {0x50, 0x4B, 0x01, 0x02};
    const auto central = std::search(badCrc.begin(), badCrc.end(), CENTRAL.begin(), CENTRAL.end());
    central[16] ^= 0xFF;
    CHECK(!Setup::Zip::Read(badCrc, 1 << 20).has_value());
    CHECK(!Setup::Zip::Read(std::span(zip).first(zip.size() / 2), 1 << 20).has_value());
    CHECK(!Setup::Zip::Read(zip, 8).has_value());
    CHECK(!Setup::Zip::Read(Text("PK not really a zip, just text that is long enough"), 1 << 20).has_value());
    CHECK(Setup::Zip::Crc32(Text("123456789")) == 0xCBF43926u);
}

TEST("only names that stay inside the folder are written")
{
    CHECK(Setup::Zip::IsSafeName("Linker.exe"));
    CHECK(Setup::Zip::IsSafeName("raw/t5/shader_bin/x.cso"));
    CHECK(!Setup::Zip::IsSafeName("../evil.exe"));
    CHECK(!Setup::Zip::IsSafeName("raw/../../evil.exe"));
    CHECK(!Setup::Zip::IsSafeName("raw\\..\\evil.exe"));
    CHECK(!Setup::Zip::IsSafeName("/abs.exe"));
    CHECK(!Setup::Zip::IsSafeName("C:/abs.exe"));
    CHECK(!Setup::Zip::IsSafeName(""));
}

TEST("only the entries wanted are unpacked, as ffmpeg.exe from FFmpeg's zip")
{
    const auto zip = MakeZip({{"ffmpeg-9.0.2-essentials_build/bin/ffplay.exe", Text("not this one"), true},
                              {"ffmpeg-9.0.2-essentials_build/bin/ffmpeg.exe", Text("this one"), true},
                              {"ffmpeg-9.0.2-essentials_build/README.txt", Text("nor this"), false}});
    const auto entries = Setup::Zip::Read(zip, 64, [](std::string_view name) { return name.ends_with("/bin/ffmpeg.exe"); });
    CHECK(entries && entries->size() == 1);
    if (entries && entries->size() == 1)
        CHECK((*entries)[0].data == Text("this one"));
    // what isn't wanted may be bigger than the limit
    CHECK(Setup::Zip::Read(zip, 8, [](std::string_view name) { return name.ends_with("README.txt"); }).has_value());
}

TEST("a zip written stored reads back, entries in order with their data (the weapons' textures, sp-maps plan 7)")
{
    std::vector<Setup::Zip::Entry> entries{{"images/w_tommy_n.iwi", {1, 2, 3, 4, 5}},
                                           {"images/hud_thompson.iwi", std::vector<std::uint8_t>(70000, 7)},
                                           {"images/empty.iwi", {}}};
    const auto archive = Setup::Zip::WriteStored(entries);
    const auto read = Setup::Zip::Read(archive, 1 << 20);
    CHECK(read && read->size() == 3);
    for (std::size_t i = 0; read && i < read->size(); ++i)
        CHECK((*read)[i].name == entries[i].name && (*read)[i].data == entries[i].data);
    // nothing but its own: no entries at all
    const auto none = Setup::Zip::Read(Setup::Zip::WriteStored({}), 1 << 20);
    CHECK(none && none->empty());
}
