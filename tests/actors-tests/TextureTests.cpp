#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/TextureLogic.hpp"
#include "Setup/TextureBuild.hpp"
#include "Utilities/Deflate.hpp"
#include "Utilities/Inflate.hpp"

using namespace IWXMVM::Components;
namespace Build = IWXMVM::Setup::TextureBuild;
using TextureLogic::Group;
using TextureLogic::ImportVerdict;
using TextureLogic::Kind;

// Texture packs: the game's iwi files, what is upscaled and how much, the build's plan,
// the index and Iris Studio's switches, the budget, the import checks, the Look tab's line, and the build's image work

namespace
{
    // a DXT texture's levels, largest first, each filled with its own byte
    std::vector<std::vector<std::uint8_t>> MakeLevels(std::uint8_t format, std::uint32_t width, std::uint32_t height,
                                                      bool mips)
    {
        std::vector<std::vector<std::uint8_t>> levels;
        std::uint8_t fill = 1;
        while (true)
        {
            levels.emplace_back(TextureLogic::GetLevelBytes(format, width, height), fill++);
            if (!mips || (width == 1 && height == 1))
                break;
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        return levels;
    }

    std::vector<TextureLogic::Level> AsLevels(const std::vector<std::vector<std::uint8_t>>& bytes, std::uint32_t width,
                                              std::uint32_t height)
    {
        std::vector<TextureLogic::Level> levels;
        for (const auto& level : bytes)
        {
            levels.push_back({width, height, level});
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        return levels;
    }

    TextureLogic::IwiHeader Header(std::uint8_t format, std::uint16_t width, std::uint16_t height,
                                   std::uint8_t flags = TextureLogic::IWI_STREAMING)
    {
        TextureLogic::IwiHeader header;
        header.format = format;
        header.flags = flags;
        header.width = width;
        header.height = height;
        return header;
    }

    TextureLogic::Texture GameTexture(std::uint8_t format, std::uint16_t width, std::uint16_t height,
                                      std::uint32_t crc = 7)
    {
        return {Header(format, width, height), "iw_11.iwd", 1000, crc};
    }
}  // namespace

TEST("texture pack: an iwi's 48-byte header and its levels, smallest first, written back byte for byte")
{
    auto header = Header(TextureLogic::FORMAT_DXT1, 64, 32);
    header.gamma = 2.0f;
    const auto bytes = MakeLevels(TextureLogic::FORMAT_DXT1, 64, 32, true);
    const auto file = TextureLogic::WriteIwi(header, AsLevels(bytes, 64, 32));
    // 7 levels: 64 x 32 (1,024 bytes) down to 1 x 1 (8 bytes)
    CHECK(bytes.size() == 7);
    CHECK(file.size() == 48 + 1024 + 256 + 64 + 16 + 8 + 8 + 8);
    // the smallest level right after the header, the largest at the end
    CHECK(file[48] == 7 && file.back() == 1);

    const auto iwi = TextureLogic::ParseIwi(file);
    CHECK(iwi.has_value());
    CHECK(iwi->header.width == 64 && iwi->header.height == 32 && iwi->header.gamma == 2.0f);
    CHECK(iwi->levels.size() == 7 && iwi->levels[0].width == 64 && iwi->levels[0].data.size() == 1024);
    CHECK(iwi->levels.back().width == 1 && iwi->levels.back().data[0] == 7);
    // fileSizeForPicmip: the whole file, then less each larger level; past the last level the smallest stays, as the
    // game's own small textures have it (35 of Nuketown's)
    CHECK(iwi->header.fileSizeForPicmip[0] == file.size());
    CHECK(iwi->header.fileSizeForPicmip[1] == file.size() - 1024);
    CHECK(iwi->header.fileSizeForPicmip[6] == 48 + 8);
    CHECK(iwi->header.fileSizeForPicmip[7] == 48 + 8);
    CHECK(TextureLogic::WriteIwi(iwi->header, iwi->levels) == file);

    // flags 0x2: one level only
    auto single = Header(TextureLogic::FORMAT_DXT5, 16, 16, TextureLogic::IWI_NO_MIPS);
    const auto one = MakeLevels(TextureLogic::FORMAT_DXT5, 16, 16, false);
    const auto singleFile = TextureLogic::WriteIwi(single, AsLevels(one, 16, 16));
    const auto parsed = TextureLogic::ParseIwi(singleFile);
    CHECK(parsed && parsed->levels.size() == 1 && parsed->levels[0].data.size() == 256);
    CHECK(TextureLogic::WriteIwi(parsed->header, parsed->levels) == singleFile);

    // not an iwi, another version, cut short
    CHECK(!TextureLogic::ParseIwi(std::vector<std::uint8_t>(64, 0)));
    auto version = file;
    version[3] = 14;
    CHECK(!TextureLogic::ParseIwi(version));
    CHECK(!TextureLogic::ParseIwi(std::span(file).first(file.size() - 2)));
}

TEST("texture pack: a DDS of the pack's levels reads back with its format, size and levels")
{
    const auto bytes = MakeLevels(TextureLogic::FORMAT_DXT5, 128, 64, true);
    const auto dds = TextureLogic::WriteDds(TextureLogic::FORMAT_DXT5, AsLevels(bytes, 128, 64));
    const auto info = TextureLogic::ParseDdsHeader(dds, dds.size());
    CHECK(info && info->format == TextureLogic::FORMAT_DXT5 && info->width == 128 && info->height == 64);
    CHECK(info->levels == 8 && info->complete);
    CHECK(dds.size() == 128 + TextureLogic::GetChainBytes(TextureLogic::FORMAT_DXT5, 128, 64));
    // cut short
    CHECK(!TextureLogic::ParseDdsHeader(dds, dds.size() - 1)->complete);
    // 32-bit, R G B A written as B G R A
    const std::vector<std::uint8_t> texel{10, 20, 30, 40};
    const auto rgba = TextureLogic::WriteDds(0, {{1, 1, texel}});
    const auto rgbaInfo = TextureLogic::ParseDdsHeader(rgba, rgba.size());
    CHECK(rgbaInfo && rgbaInfo->format == 0 && rgbaInfo->levels == 1);
    CHECK(rgba[128] == 30 && rgba[129] == 20 && rgba[130] == 10 && rgba[131] == 40);
    CHECK(!TextureLogic::ParseDdsHeader(std::vector<std::uint8_t>(128, 0), 128));
}

TEST("texture pack: kinds by name, as the research counted them; only flat DXT colour, normal and spec maps upscale")
{
    CHECK(TextureLogic::GetKind("~-geb_art_wall_redbrick_c", TextureLogic::FORMAT_DXT1) == Kind::Colour);
    CHECK(TextureLogic::GetKind("us_art_wall_woodplank_clean_c") == Kind::Colour);
    CHECK(TextureLogic::GetKind("eb_art_wall_redbrick_n") == Kind::Normal);
    CHECK(TextureLogic::GetKind("bus_windows_nml") == Kind::Normal);
    CHECK(TextureLogic::GetKind("c_usa_cia_mp_body_n2") == Kind::Normal);
    // a "normal" map that isn't DXT5 is resized like a mask
    CHECK(TextureLogic::GetKind("odd_n", TextureLogic::FORMAT_DXT1) == Kind::Spec);
    CHECK(TextureLogic::GetKind("~~-geb_art_wall_redbrick_s") == Kind::Spec);
    CHECK(TextureLogic::GetKind("c_gen_arm_clean_g") == Kind::Spec);
    CHECK(TextureLogic::GetKind("hud_compass_face") == Kind::Ui);
    CHECK(TextureLogic::GetKind("menu_mp_bar_shadow") == Kind::Ui);
    CHECK(TextureLogic::GetKind("$identitynormalmap") == Kind::Ui);
    CHECK(TextureLogic::GetKind("fxt_smk_puff") == Kind::Effect);
    CHECK(TextureLogic::GetKind("sky_nuked") == Kind::Effect);
    // a word, not a part of one: industrial isn't dust, firing range isn't fire
    CHECK(TextureLogic::GetKind("mtl_industrial_floor_c") == Kind::Colour);
    CHECK(TextureLogic::GetKind("firingrange_target_c") == Kind::Colour);
    CHECK(TextureLogic::GetKind("lookup") == Kind::Other);

    const auto flat = Header(TextureLogic::FORMAT_DXT1, 512, 512);
    CHECK(TextureLogic::IsUpscaled("brick_c", flat));
    CHECK(TextureLogic::IsUpscaled("brick_s", flat));
    CHECK(!TextureLogic::IsUpscaled("hud_icon_c", flat));
    CHECK(!TextureLogic::IsUpscaled("lookup", flat));
    CHECK(!TextureLogic::IsUpscaled("brick_c", Header(TextureLogic::FORMAT_DXT1, 512, 512, TextureLogic::IWI_CUBE)));
    CHECK(!TextureLogic::IsUpscaled("brick_c", Header(TextureLogic::FORMAT_DXT1, 512, 512, TextureLogic::IWI_VOLUME)));
    CHECK(!TextureLogic::IsUpscaled("brick_c", Header(1, 512, 512)));
    CHECK(TextureLogic::UsesModel(Kind::Colour) && TextureLogic::UsesModel(Kind::Normal));
    CHECK(!TextureLogic::UsesModel(Kind::Spec));
}

TEST("texture pack: 4x capped at 4,096 a side, 2x under 8 GB cards, and the sizes with every level")
{
    CHECK(TextureLogic::GetScale(512, 512, 4) == 4);
    CHECK(TextureLogic::GetScale(1024, 1024, 4) == 4);
    CHECK(TextureLogic::GetScale(2048, 2048, 4) == 2);
    CHECK(TextureLogic::GetScale(2048, 512, 4) == 2);
    CHECK(TextureLogic::GetScale(4096, 4096, 4) == 1);
    CHECK(TextureLogic::GetScale(512, 512, 2) == 2);
    // DXT1 512 x 512 at 4x: 2,048 x 2,048 with its levels (the smallest a whole block each), 2.67 MB; DXT5 twice that
    CHECK(TextureLogic::GetPackBytes(TextureLogic::FORMAT_DXT1, 512, 512, 4) == 2796216);
    CHECK(TextureLogic::GetPackBytes(TextureLogic::FORMAT_DXT5, 512, 512, 4) == 5592432);
    CHECK(TextureLogic::GetLevelBytes(TextureLogic::FORMAT_DXT1, 2, 2) == 8);
    CHECK(TextureLogic::GetLevelCount(2048, 1024) == 12);
    CHECK(TextureLogic::GetDefaultScale(8018ull << 20) == 4);
    CHECK(TextureLogic::GetDefaultScale(6144ull << 20) == 2);
}

TEST("texture pack: a build's plan counts a texture shared by maps once and skips what is built from the same file")
{
    std::unordered_map<std::string, TextureLogic::Texture> textures{
        {"brick_c", GameTexture(TextureLogic::FORMAT_DXT1, 512, 512)},
        {"brick_n", GameTexture(TextureLogic::FORMAT_DXT5, 512, 512)},
        {"brick_s", GameTexture(TextureLogic::FORMAT_DXT1, 256, 256)},
        {"deck_c", GameTexture(TextureLogic::FORMAT_DXT1, 1024, 1024)},
        {"hud_icon", GameTexture(TextureLogic::FORMAT_DXT5, 64, 64)},
    };
    // Nuketown and Array share the brick; the HUD image and a name no iwd has are left out
    const std::vector<std::vector<std::string>> parts{{"brick_c", "brick_n", "brick_s", "hud_icon", "gone_c"},
                                                      {"brick_c", "deck_c"}};
    const auto plan = TextureLogic::MakePlan(parts, textures, 4, {});
    CHECK(plan.images.size() == 4 && plan.shared == 1 && plan.built == 0);
    CHECK(plan.bytes == 2796216 + 5592432 + 699064 + 11184824);
    CHECK(plan.modelTexels == 512 * 512 * 2 + 1024 * 1024 && plan.plainTexels == 256 * 256);

    // built before: the same file at 4x stays; at 2x (a 4x wanted), or from a changed file, is built again
    std::unordered_map<std::string, TextureLogic::Built> built{
        {"brick_c", {4, 1000, 7}}, {"brick_n", {2, 1000, 7}}, {"deck_c", {4, 1000, 8}}};
    const auto again = TextureLogic::MakePlan(parts, textures, 4, built);
    CHECK(again.built == 1 && again.images.size() == 3);
    CHECK(std::ranges::find(again.images, "brick_c") == again.images.end());
    // a 2x asked for after a 4x: nothing to do for it
    CHECK(TextureLogic::MakePlan({{"brick_c"}}, textures, 2, built).images.empty());

    // about 6 s a megatexel through the model, compression mostly alongside
    const auto seconds = TextureLogic::EstimateSeconds(plan, 4);
    CHECK(seconds > (plan.modelTexels / 1e6) * 5.9 && seconds < (plan.modelTexels / 1e6) * 5.9 + 30.0);
    CHECK(TextureLogic::FormatDuration(25 * 60) == "about 25 minutes");
    CHECK(TextureLogic::FormatDuration(30) == "about a minute");
    CHECK(TextureLogic::FormatDuration(5.5 * 3600) == "about 5.5 hours");
}

TEST("texture pack: the parts' names and their order, the guns first")
{
    CHECK(TextureLogic::GetPartName("mp_nuked") == "Nuketown");
    CHECK(TextureLogic::GetPartName("common_mp") == "Guns and hands");
    CHECK(TextureLogic::GetPartName("zombie_theater") == "Kino der Toten");
    CHECK(TextureLogic::GetPartName("zombie_cod5_factory") == "Der Riese");
    CHECK(TextureLogic::GetPartName("mp_unknown") == "mp_unknown");
    CHECK(!TextureLogic::GetPartKind("ui_mp"));
    CHECK(TextureLogic::ListsBefore("common_mp", "mp_array"));
    CHECK(TextureLogic::ListsBefore("mp_array", "mp_nuked"));
    CHECK(TextureLogic::ListsBefore("mp_nuked", "cuba"));
    CHECK(TextureLogic::ListsBefore("cuba", "zombie_theater"));
    CHECK(!TextureLogic::ListsBefore("mp_villa", "mp_cracked"));
}

TEST("texture pack: the index and the switches read back; a removed map takes only the images it alone used")
{
    TextureLogic::PackIndex index;
    index.model = std::string(TextureLogic::MODEL_CREDIT);
    index.images["brick_c"] = {4, TextureLogic::FORMAT_DXT1, 2048, 2048, 2796328, {4, 1000, 7}, {"mp_nuked", "mp_array"}};
    index.images["deck_c"] = {4, TextureLogic::FORMAT_DXT1, 2048, 2048, 2796328, {4, 1000, 8}, {"mp_nuked"}};
    index.parts["mp_nuked"] = {4};
    index.parts["mp_array"] = {4};
    TextureLogic::CountParts(index);
    CHECK(index.parts["mp_nuked"].images == 2 && index.parts["mp_array"].images == 1);
    const auto back = TextureLogic::IndexFromJson(nlohmann::json::parse(TextureLogic::ToJson(index).dump()));
    CHECK(back.model == TextureLogic::MODEL_CREDIT && back.images.size() == 2);
    CHECK(back.images.at("brick_c").parts.size() == 2 && back.images.at("deck_c").source.crc == 8);
    CHECK(back.parts.at("mp_nuked").images == 2 && TextureLogic::GetPackBytes(back) == 2 * 2796328);
    bool threw = false;
    try
    {
        TextureLogic::IndexFromJson({{"version", 2}, {"images", nlohmann::json::object()}});
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    CHECK(threw);

    auto removed = index;
    const auto gone = TextureLogic::RemovePart(removed, "mp_nuked");
    CHECK(gone == std::vector<std::string>{"deck_c"});
    CHECK(removed.images.size() == 1 && removed.images.at("brick_c").parts == std::vector<std::string>{"mp_array"});
    CHECK(!removed.parts.contains("mp_nuked"));

    const TextureLogic::Use use{{"mp_array"}, {"HD Bricks"}, {Group::Weapons, Group::Players}};
    CHECK(TextureLogic::UseFromJson(nlohmann::json::parse(TextureLogic::ToJson(use).dump())) == use);
    CHECK(TextureLogic::UseFromJson(nlohmann::json::array()) == TextureLogic::Use{});
    CHECK(TextureLogic::UseFromJson({{"groupsOff", {"weapons", "Tanks", 3}}}).groupsOff == std::set{Group::Weapons});
    CHECK(TextureLogic::DescribeGroupsOff(use) == "Players and Weapons");
    CHECK(TextureLogic::DescribeGroupsOff({}).empty());
}

TEST("texture pack: what the mod draws: the parts on, the loaded map's say, imported packs win over the built one")
{
    TextureLogic::Folder built{"t\\v1", {}};
    built.index.images["brick_c"] = {4, TextureLogic::FORMAT_DXT1, 2048, 2048, 1, {}, {"mp_nuked", "mp_array"}};
    built.index.images["deck_c"] = {4, TextureLogic::FORMAT_DXT1, 2048, 2048, 1, {}, {"mp_nuked"}};
    built.index.images["gun_c"] = {2, TextureLogic::FORMAT_DXT1, 1024, 1024, 1, {}, {"common_mp"}};
    TextureLogic::Folder imported{"t\\imported\\HD Bricks", {}};
    imported.index.imported = true;
    imported.index.name = "HD Bricks";
    imported.index.images["brick_c"] = {8, TextureLogic::FORMAT_DXT1, 4096, 4096, 1, {}, {}};

    const std::vector<std::string> nuketown{"mp_nuked", "common_mp"};
    auto sources = TextureLogic::Resolve({built}, {}, nuketown);
    CHECK(sources.size() == 3 && sources.at("deck_c").file == std::filesystem::path("t\\v1\\deck_c.dds"));
    CHECK(sources.at("gun_c").scale == 2);
    // Nuketown off: on Nuketown its textures go, the one shared with Array too (the map loaded says)
    sources = TextureLogic::Resolve({built}, {{"mp_nuked"}, {}, {}}, nuketown);
    CHECK(sources.size() == 1 && sources.contains("gun_c"));
    // on a map the pack wasn't built for, an image any part on has is drawn (a character from another map)
    sources = TextureLogic::Resolve({built}, {{"mp_nuked"}, {}, {}}, {"mp_villa"});
    CHECK(sources.contains("brick_c") && !sources.contains("deck_c"));
    // the imported pack wins, and goes when ticked off
    sources = TextureLogic::Resolve({built, imported}, {}, nuketown);
    CHECK(sources.at("brick_c").pack == "HD Bricks" && sources.at("brick_c").scale == 8);
    sources = TextureLogic::Resolve({imported, built}, {{}, {"HD Bricks"}, {}}, nuketown);
    CHECK(sources.at("brick_c").pack.empty());
}

TEST("texture pack: groups by what uses an image, Weapons first, then Players, then Maps")
{
    CHECK(TextureLogic::GetGroup(true, true) == Group::Weapons);
    CHECK(TextureLogic::GetGroup(true, false) == Group::Weapons);
    CHECK(TextureLogic::GetGroup(false, true) == Group::Players);
    CHECK(TextureLogic::GetGroup(false, false) == Group::Maps);
    CHECK(TextureLogic::FindGroup("players") == Group::Players);
    CHECK(TextureLogic::GetName(Group::Weapons) == "Weapons");
    CHECK(!TextureLogic::FindGroup("Killstreaks"));
}

TEST("texture pack: the budget is the card's free memory less a gigabyte; the least recently drawn go first")
{
    constexpr std::uint64_t MB = 1ull << 20;
    CHECK(TextureLogic::GetBudget(6000 * MB) == 4976 * MB);
    CHECK(TextureLogic::GetBudget(900 * MB) == TextureLogic::MIN_BUDGET);

    const std::vector<TextureLogic::Resident> residents{
        {0, 40 * MB, 10}, {1, 30 * MB, 3}, {2, 20 * MB, 7}, {3, 50 * MB, 12}};
    // fits: nothing goes
    CHECK(TextureLogic::ChooseEvictions(residents, 140 * MB, 10 * MB, 200 * MB, 12)->empty());
    // 25 MB short: the oldest (frame 3, 30 MB) goes
    CHECK(*TextureLogic::ChooseEvictions(residents, 140 * MB, 85 * MB, 200 * MB, 12) == std::vector<std::size_t>{1});
    // 45 MB short: frames 3 then 7
    CHECK(*TextureLogic::ChooseEvictions(residents, 140 * MB, 105 * MB, 200 * MB, 12) ==
          (std::vector<std::size_t>{1, 2}));
    // what is drawn this frame stays: the other three going isn't enough
    CHECK(!TextureLogic::ChooseEvictions(residents, 140 * MB, 160 * MB, 200 * MB, 12));
    // bigger than the budget itself
    CHECK(!TextureLogic::ChooseEvictions({}, 0, 300 * MB, 200 * MB, 1));
}

TEST("texture pack: an imported DDS fits when the game has its name, it is DXT with mips, and its shape is the game's")
{
    const auto game = Header(TextureLogic::FORMAT_DXT1, 512, 256);
    const auto dds = [](std::uint8_t format, std::uint32_t width, std::uint32_t height, std::uint32_t levels) {
        return std::optional<TextureLogic::DdsInfo>({format, width, height, levels, 128, true});
    };
    CHECK(TextureLogic::CheckImport(dds(TextureLogic::FORMAT_DXT1, 2048, 1024, 12), game) == ImportVerdict::Fits);
    CHECK(TextureLogic::CheckImport(dds(0, 512, 256, 10), game) == ImportVerdict::Fits);
    CHECK(TextureLogic::CheckImport(dds(TextureLogic::FORMAT_DXT1, 2048, 1024, 1), game) == ImportVerdict::NoMips);
    CHECK(TextureLogic::CheckImport(dds(TextureLogic::FORMAT_DXT1, 2048, 2048, 12), game) ==
          ImportVerdict::NotSameShape);
    CHECK(TextureLogic::CheckImport(dds(TextureLogic::FORMAT_DXT1, 1536, 768, 11), game) ==
          ImportVerdict::NotSameShape);
    CHECK(TextureLogic::CheckImport(dds(TextureLogic::FORMAT_DXT1, 8192, 4096, 14), game) == ImportVerdict::TooBig);
    CHECK(TextureLogic::CheckImport(dds(TextureLogic::FORMAT_DXT1, 2048, 1024, 12), std::nullopt) ==
          ImportVerdict::NotGameName);
    CHECK(TextureLogic::CheckImport(std::nullopt, game) == ImportVerdict::NotDxt);
    auto cut = dds(TextureLogic::FORMAT_DXT1, 2048, 1024, 12);
    cut->complete = false;
    CHECK(TextureLogic::CheckImport(cut, game) == ImportVerdict::Unreadable);
    CHECK(TextureLogic::GetImportScale(*dds(TextureLogic::FORMAT_DXT1, 2048, 1024, 12), game) == 4);

    TextureLogic::ImportSummary summary;
    CHECK(TextureLogic::DescribeLeftOut(summary) == "None");
    summary.left[ImportVerdict::NotGameName] = 14;
    summary.left[ImportVerdict::NotDxt] = 2;
    CHECK(TextureLogic::DescribeLeftOut(summary) == "14 not the game's names, 2 not DXT");
    CHECK(TextureLogic::GetImportName("HD Bricks") == "HD Bricks");
    CHECK(TextureLogic::GetImportName("a<b>:c?. ") == "abc");
    CHECK(TextureLogic::GetImportName("...") == "Imported");

    // a real folder of good and bad files
    const auto folder = std::filesystem::temp_directory_path() / "iris-texture-import-test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto write = [&](const char* name, const std::vector<std::uint8_t>& bytes) {
        std::ofstream(folder / name, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    const auto good = MakeLevels(TextureLogic::FORMAT_DXT1, 2048, 1024, true);
    write("brick_c.dds", TextureLogic::WriteDds(TextureLogic::FORMAT_DXT1, AsLevels(good, 2048, 1024)));
    write("notes.txt", {'h', 'i'});
    write("odd_c.dds", {'D', 'D', 'S', ' '});
    std::size_t fits = 0, notDxt = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
    {
        if (entry.path().extension() != ".dds")
            continue;
        std::ifstream file(entry.path(), std::ios::binary);
        std::vector<std::uint8_t> head(TextureLogic::DDS_HEADER_SIZE);
        file.read(reinterpret_cast<char*>(head.data()), head.size());
        head.resize(file.gcount());
        const auto info = TextureLogic::ParseDdsHeader(head, std::filesystem::file_size(entry.path()));
        const auto verdict = TextureLogic::CheckImport(info, game);
        fits += verdict == ImportVerdict::Fits;
        notDxt += verdict == ImportVerdict::NotDxt;
    }
    std::filesystem::remove_all(folder);
    CHECK(fits == 1 && notDxt == 1);
}

TEST("texture pack: the Look tab's line under Textures")
{
    TextureLogic::Status status;
    status.map = "Nuketown";
    CHECK(TextureLogic::GetStatusLine(status) == "No pack for Nuketown. Build one in Iris Studio.");
    CHECK(TextureLogic::GetStatusLead(status) == 0);
    status.mapHasPack = true;
    CHECK(TextureLogic::GetStatusLine(status) == "Pack: Nuketown off");
    status.packs = {"Nuketown 4x"};
    status.images = 922;
    status.bytes = 3650722201ull;
    CHECK(TextureLogic::GetStatusLine(status) == "Pack: Nuketown 4x \xC2\xB7 3.4 GB");
    status.on = true;
    status.drawn = 214;
    status.waiting = 30;
    CHECK(TextureLogic::GetStatusLine(status) == "Pack: Nuketown 4x \xC2\xB7 loading 214 / 922");
    status.waiting = 0;
    status.inUse = 2040109466ull;
    status.budget = 5368709120ull;
    status.groupsOff = "Weapons";
    CHECK(TextureLogic::GetStatusLine(status) == "Pack: Nuketown 4x \xC2\xB7 1.9 GB / 5.0 GB \xC2\xB7 Weapons off");
    CHECK(TextureLogic::GetStatusLead(status) == std::string("Pack: Nuketown 4x").size());
    status.packs = {"Nuketown 4x", "Guns and hands 4x", "HD Bricks (imported)"};
    CHECK(TextureLogic::GetStatusLine(status).starts_with("Pack: Nuketown 4x, Guns and hands 4x, HD Bricks (imported) "));
    // while wrong images are left the line ends on the repair
    status.packs = {"Grid 2x"};
    status.groupsOff.clear();
    status.repair = true;
    CHECK(TextureLogic::GetStatusLine(status) ==
          "Pack: Grid 2x \xC2\xB7 1.9 GB / 5.0 GB \xC2\xB7 Repair in Iris Studio");
    CHECK(TextureLogic::GetStatusLine(status).ends_with(TextureLogic::REPAIR_NOTE));
    CHECK(TextureLogic::FormatGigabytes(50ull << 20) == "50 MB");
}

TEST("texture build: DXT1 and DXT5 blocks decode as the format says")
{
    // DXT1: c0 white, c1 black (c0 > c1: four colours), texel indices 0, 1, 2, 3 in the first row
    const std::vector<std::uint8_t> dxt1{0xFF, 0xFF, 0x00, 0x00, 0b11100100, 0, 0, 0};
    auto image = Build::DecodeDxt(TextureLogic::FORMAT_DXT1, 4, 4, dxt1);
    CHECK(image.At(0, 0)[0] == 255 && image.At(1, 0)[0] == 0);
    CHECK(image.At(2, 0)[0] == 170 && image.At(3, 0)[0] == 85);
    CHECK(image.At(0, 1)[1] == 255 && image.At(0, 0)[3] == 255);
    // c0 <= c1: the fourth is black and transparent
    const std::vector<std::uint8_t> punch{0x00, 0x00, 0xFF, 0xFF, 0b11000000, 0, 0, 0};
    image = Build::DecodeDxt(TextureLogic::FORMAT_DXT1, 4, 4, punch);
    CHECK(image.At(3, 0)[3] == 0 && image.At(0, 0)[3] == 255);
    CHECK(Build::HasAlpha(image));

    // DXT5: alpha ends 255 and 0, the first texel's index 1 (0), the second's 0 (255); colour white
    std::vector<std::uint8_t> dxt5{255, 0, 0b00000001, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0x00, 0x00, 0, 0, 0, 0};
    image = Build::DecodeDxt(TextureLogic::FORMAT_DXT5, 4, 4, dxt5);
    CHECK(image.At(0, 0)[3] == 0 && image.At(1, 0)[3] == 255 && image.At(1, 0)[0] == 255);
    // a texture smaller than a block
    image = Build::DecodeDxt(TextureLogic::FORMAT_DXT1, 2, 2, dxt1);
    CHECK(image.width == 2 && image.rgba.size() == 16);
}

TEST("texture build: wrap padding tiles the texture around itself and crops back to it")
{
    auto image = Build::MakeImage(4, 4);
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x)
            image.At(x, y)[0] = static_cast<std::uint8_t>(y * 4 + x);
    CHECK(Build::GetPad(512, 512) == 32 && Build::GetPad(16, 64) == 8);
    const auto padded = Build::WrapPad(image, 2);
    CHECK(padded.width == 8 && padded.height == 8);
    // the top left corner is the texture's bottom right
    CHECK(padded.At(0, 0)[0] == 10 && padded.At(1, 1)[0] == 15);
    CHECK(padded.At(2, 2)[0] == 0 && padded.At(7, 2)[0] == 1);
    const auto back = Build::Crop(padded, 2, 2, 4, 4);
    CHECK(back.rgba == image.rgba);
}

TEST("texture build: a normal map rebuilt as RGB and back keeps its slopes; halving keeps it unit length")
{
    auto stored = Build::MakeImage(2, 2);
    // x 0.5, y -0.25 as the game stores them
    for (std::size_t i = 0; i < stored.rgba.size(); i += 4)
    {
        stored.rgba[i] = stored.rgba[i + 1] = stored.rgba[i + 2] = 96;  // y = 96 / 127.5 - 1 = -0.247
        stored.rgba[i + 3] = 191;                                        // x = 0.498
    }
    const auto rgb = Build::ToNormal(stored);
    // z = sqrt(1 - 0.248 - 0.061) = 0.83
    CHECK(std::abs(rgb.At(0, 0)[2] / 127.5 - 1.0 - 0.831) < 0.01);
    const auto back = Build::FromNormal(rgb);
    CHECK(std::abs(int(back.At(0, 0)[3]) - 191) <= 1 && std::abs(int(back.At(0, 0)[1]) - 96) <= 1);
    CHECK(back.At(0, 0)[0] == back.At(0, 0)[1] && back.At(0, 0)[2] == back.At(0, 0)[1]);
    // a short normal from the model comes out unit length
    auto shortNormal = rgb;
    for (std::size_t i = 0; i < shortNormal.rgba.size(); i += 4)
        shortNormal.rgba[i + 2] = 128;  // z = 0
    const auto renormalized = Build::FromNormal(shortNormal);
    const double x = renormalized.At(0, 0)[3] / 127.5 - 1.0, y = renormalized.At(0, 0)[1] / 127.5 - 1.0;
    CHECK(std::abs(std::sqrt(x * x + y * y) - 1.0) < 0.02);
    const auto halved = Build::Halve(stored, true);
    CHECK(halved.width == 1 && std::abs(int(halved.At(0, 0)[3]) - 191) <= 1);
}

TEST("texture build: halving averages four texels; a plain resize keeps a flat texture flat and a tiling one seamless")
{
    auto image = Build::MakeImage(2, 2);
    image.At(0, 0)[0] = 0;
    image.At(1, 0)[0] = 100;
    image.At(0, 1)[0] = 200;
    image.At(1, 1)[0] = 100;
    CHECK(Build::Halve(image, false).At(0, 0)[0] == 100);

    auto flat = Build::MakeImage(8, 8);
    std::ranges::fill(flat.rgba, 77);
    const auto big = Build::Resize(flat, 32, 32);
    CHECK(big.width == 32 && std::ranges::all_of(big.rgba, [](std::uint8_t v) { return v == 77; }));
    // stripes across: the left and right edges of the 4x meet as the middle does
    auto stripes = Build::MakeImage(8, 1);
    for (std::uint32_t x = 0; x < 8; ++x)
        stripes.At(x, 0)[0] = x % 2 ? 200 : 40;
    const auto wide = Build::Resize(stripes, 32, 4);
    CHECK(std::abs(int(wide.At(0, 0)[0]) - int(wide.At(8, 0)[0])) <= 1);
    CHECK(std::abs(int(wide.At(31, 0)[0]) - int(wide.At(7, 0)[0])) <= 1);

    const auto alpha = Build::WithAlpha(Build::Opaque(image), Build::MakeImage(2, 2));
    CHECK(alpha.At(1, 1)[3] == 0 && alpha.At(1, 1)[0] == 100);
}

TEST("texture pack: a spec map's file keeps the game's own levels from the game's size down, byte for byte")
{
    constexpr auto DXT5 = TextureLogic::FORMAT_DXT5;
    // the game's 64 x 64 and the pack's 128 x 128 (its levels all 200, so a level kept shows)
    const auto game = MakeLevels(DXT5, 64, 64, true);
    const auto file = TextureLogic::WriteIwi(Header(DXT5, 64, 64), AsLevels(game, 64, 64));
    const auto iwi = TextureLogic::ParseIwi(file);
    CHECK(iwi.has_value());
    auto pack = MakeLevels(DXT5, 128, 128, true);
    for (auto& level : pack)
        std::ranges::fill(level, std::uint8_t{200});
    const auto dds = TextureLogic::WriteDds(DXT5, AsLevels(pack, 128, 128));

    const auto kept = TextureLogic::KeepGameLevels(dds, *iwi);
    CHECK(kept && kept->size() == dds.size());
    const auto info = TextureLogic::ParseDdsHeader(*kept, kept->size());
    CHECK(info && info->levels == 8 && info->complete);
    const auto at = [&](std::uint32_t side) {
        const auto place = TextureLogic::FindDdsLevel(*info, side, side);
        return std::vector<std::uint8_t>(kept->begin() + place->offset, kept->begin() + place->offset + place->bytes);
    };
    // the level above the game's stays the pack's; the game's size and every one below are the game's
    CHECK(at(128) == pack[0]);
    CHECK(at(64) == game[0]);
    CHECK(at(8) == game[3]);
    CHECK(at(1) == game.back());
    CHECK(!TextureLogic::FindDdsLevel(*info, 256, 256));

    // a texture of the game without levels: only its size is the game's
    const auto single = MakeLevels(DXT5, 64, 64, false);
    const auto flatFile =
        TextureLogic::WriteIwi(Header(DXT5, 64, 64, TextureLogic::IWI_NO_MIPS), AsLevels(single, 64, 64));
    const auto flat = TextureLogic::ParseIwi(flatFile);
    const auto keptFlat = TextureLogic::KeepGameLevels(dds, *flat);
    CHECK(keptFlat.has_value());
    const auto place = TextureLogic::FindDdsLevel(*TextureLogic::ParseDdsHeader(*keptFlat, keptFlat->size()), 32, 32);
    CHECK(place && (*keptFlat)[place->offset] == 200);

    // a colour map keeps the game's alpha alone: a block's first 8 bytes, its colour the pack's, from the game's size
    // down
    const auto alpha = TextureLogic::KeepGameAlpha(dds, *iwi);
    CHECK(alpha && alpha->size() == dds.size());
    const auto alphaInfo = TextureLogic::ParseDdsHeader(*alpha, alpha->size());
    const auto top = TextureLogic::FindDdsLevel(*alphaInfo, 128, 128);
    const auto same = TextureLogic::FindDdsLevel(*alphaInfo, 64, 64);
    CHECK((*alpha)[top->offset] == 200 && (*alpha)[top->offset + 8] == 200);
    CHECK((*alpha)[same->offset] == game[0][0] && (*alpha)[same->offset + 7] == game[0][7]);
    CHECK((*alpha)[same->offset + 8] == 200 && (*alpha)[same->offset + 15] == 200);
    CHECK((*alpha)[same->offset + 16] == game[0][16] && (*alpha)[same->offset + 24] == 200);
    const auto last = TextureLogic::FindDdsLevel(*alphaInfo, 1, 1);
    CHECK((*alpha)[last->offset] == game.back()[0] && (*alpha)[last->offset + 8] == 200);

    // nothing kept from another format or a pack without the game's size
    const auto dxt1 = MakeLevels(TextureLogic::FORMAT_DXT1, 128, 128, true);
    const auto dxt1Game = MakeLevels(TextureLogic::FORMAT_DXT1, 64, 64, true);
    const auto dxt1File = TextureLogic::WriteIwi(Header(TextureLogic::FORMAT_DXT1, 64, 64), AsLevels(dxt1Game, 64, 64));
    const auto dxt1Dds = TextureLogic::WriteDds(TextureLogic::FORMAT_DXT1, AsLevels(dxt1, 128, 128));
    CHECK(!TextureLogic::KeepGameAlpha(dxt1Dds, *TextureLogic::ParseIwi(dxt1File)));
    CHECK(!TextureLogic::KeepGameLevels(dxt1Dds, *iwi));
    const auto tall = MakeLevels(DXT5, 128, 64, true);
    CHECK(!TextureLogic::KeepGameLevels(TextureLogic::WriteDds(DXT5, AsLevels(tall, 128, 64)), *iwi));
}

TEST("texture pack: a check reads a colour or normal map at the game's size, a spec map's at a small level")
{
    CHECK(TextureLogic::GetCheckLevel(Kind::Colour, 1024, 1024, true) == 0);
    CHECK(TextureLogic::GetCheckLevel(Kind::Normal, 512, 256, true) == 0);
    CHECK(TextureLogic::GetCheckLevel(Kind::Spec, 1024, 1024, true) == 4);
    CHECK(TextureLogic::GetCheckLevel(Kind::Spec, 1024, 64, true) == 4);
    CHECK(TextureLogic::GetCheckLevel(Kind::Spec, 64, 64, true) == 0);
    CHECK(TextureLogic::GetCheckLevel(Kind::Spec, 1024, 1024, false) == 0);

    // an iwi's levels are smallest first: the bytes through a level are its header, that level and those below
    const auto header = Header(TextureLogic::FORMAT_DXT1, 64, 32);
    const auto levels = MakeLevels(TextureLogic::FORMAT_DXT1, 64, 32, true);
    const auto file = TextureLogic::WriteIwi(header, AsLevels(levels, 64, 32));
    CHECK(TextureLogic::GetIwiBytesThrough(header, 0) == file.size());
    CHECK(TextureLogic::GetIwiBytesThrough(header, 1) == file.size() - levels[0].size());
    auto flat = Header(TextureLogic::FORMAT_DXT5, 16, 16, TextureLogic::IWI_NO_MIPS);
    CHECK(TextureLogic::GetIwiBytesThrough(flat, 0) == 48 + 256);
}

TEST("texture build: the check tells a wrong image from a sharper one")
{
    // the game's: flat grey on the left half, white on the right, its alpha low on the left
    auto game = Build::MakeImage(64, 64);
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
        {
            const std::uint8_t value = x < 32 ? 100 : 255;
            const auto texel = game.At(x, y);
            texel[0] = texel[1] = texel[2] = value;
            texel[3] = x < 32 ? 16 : 255;
        }

    // the same picture four times the size: nothing to see, nothing under alpha compared
    const auto sharper = Build::Resize(game, 256, 256, false);
    auto difference = Build::Compare(game, sharper);
    CHECK(difference.underAlpha == 0.0f);
    CHECK(!Build::IsWrong(Kind::Colour, difference));
    CHECK(!Build::IsWrong(Kind::Normal, difference));

    // old texconv's levels: the colour under low alpha gone (ice and a bipod)
    auto lost = game;
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 32; ++x)
            lost.At(x, y)[0] = lost.At(x, y)[1] = lost.At(x, y)[2] = 6;
    difference = Build::Compare(game, lost);
    CHECK(difference.colour > 90.0f && difference.underAlpha > 90.0f && difference.alpha == 0.0f);
    CHECK(Build::IsWrong(Kind::Colour, difference));

    // a black run of the model (a wagon: a normal map flat)
    auto flat = game;
    std::ranges::fill(flat.rgba, std::uint8_t{50});
    difference = Build::Compare(game, flat);
    CHECK(Build::IsWrong(Kind::Normal, difference));

    // a normal map's alpha is its x: under it nothing is colour
    const Build::Difference underAlpha{10.0f, 0.0f, 40.0f};
    CHECK(!Build::IsWrong(Kind::Normal, underAlpha));
    CHECK(Build::IsWrong(Kind::Colour, underAlpha));

    // a spec map's levels are the game's own: a small change of its gloss is wrong there, and nowhere else
    auto gloss = game;
    for (std::size_t i = 3; i < gloss.rgba.size(); i += 4)
        gloss.rgba[i] = static_cast<std::uint8_t>(gloss.rgba[i] - 8);
    difference = Build::Compare(game, gloss);
    CHECK(difference.alpha == 8.0f);
    CHECK(Build::IsWrong(Kind::Spec, difference));
    CHECK(!Build::IsWrong(Kind::Colour, difference));
}

TEST("texture build: an imported image is left out only when it is broken, not when it looks different")
{
    auto game = Build::MakeImage(64, 64);
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 64; ++x)
        {
            const auto texel = game.At(x, y);
            texel[0] = texel[1] = texel[2] = static_cast<std::uint8_t>(60 + x * 2);
            texel[3] = x < 32 ? 16 : 255;
        }
    CHECK(!Build::IsBroken(game, game));
    CHECK(!Build::IsBroken(game, Build::Resize(game, 256, 256, false)));

    // a pack of another look: brighter, redder
    auto other = game;
    for (std::size_t i = 0; i < other.rgba.size(); i += 4)
    {
        other.rgba[i] = static_cast<std::uint8_t>(std::min(255, other.rgba[i] + 70));
        other.rgba[i + 2] = static_cast<std::uint8_t>(other.rgba[i + 2] / 2);
    }
    CHECK(!Build::IsBroken(game, other));

    // the colour gone under low alpha
    auto lost = game;
    for (std::uint32_t y = 0; y < 64; ++y)
        for (std::uint32_t x = 0; x < 32; ++x)
            lost.At(x, y)[0] = lost.At(x, y)[1] = lost.At(x, y)[2] = 4;
    CHECK(Build::IsBroken(game, lost));

    // one flat colour where the game's has a picture, at any size; a flat image of the game's stays
    auto flat = Build::MakeImage(128, 128);
    std::ranges::fill(flat.rgba, std::uint8_t{50});
    CHECK(Build::IsBroken(game, flat));
    auto flatGame = Build::MakeImage(64, 64);
    std::ranges::fill(flatGame.rgba, std::uint8_t{200});
    CHECK(!Build::IsBroken(flatGame, flat));
}

TEST("texture pack: an image found wrong is built again; its build number stays in the index")
{
    const std::unordered_map<std::string, TextureLogic::Texture> textures{
        {"~-ga_c", GameTexture(TextureLogic::FORMAT_DXT1, 512, 512)},
        {"a_n", GameTexture(TextureLogic::FORMAT_DXT5, 512, 512)}};
    std::unordered_map<std::string, TextureLogic::Built> built{{"~-ga_c", {4, 1000, 7}}, {"a_n", {4, 1000, 7}}};
    const std::vector<std::vector<std::string>> part{{"~-ga_c", "a_n"}};
    auto plan = TextureLogic::MakePlan(part, textures, 4, built);
    CHECK(plan.images.empty() && plan.built == 2);
    built["a_n"].wrong = true;
    plan = TextureLogic::MakePlan(part, textures, 4, built);
    CHECK(plan.images == std::vector<std::string>{"a_n"} && plan.built == 1);

    TextureLogic::PackIndex index;
    index.images["old"] = {2, TextureLogic::FORMAT_DXT1, 1024, 1024, 100, {2, 50, 9}, {"mp_duga"}};
    auto repaired = index.images["old"];
    repaired.build = TextureLogic::BUILD_VERSION;
    index.images["new"] = repaired;
    const auto json = TextureLogic::ToJson(index);
    // (an index of before is read as build 1, and an older mod reads this one)
    CHECK(!json["images"]["old"].contains("build"));
    const auto read = TextureLogic::IndexFromJson(json);
    CHECK(read.images.at("old").build == 1);
    CHECK(read.images.at("new").build == TextureLogic::BUILD_VERSION);
}

TEST("texture pack: an iwi's header comes out of the start of its deflated entry, without inflating the rest")
{
    auto header = Header(TextureLogic::FORMAT_DXT5, 256, 128);
    header.gamma = 0.0f;
    // texels that don't repeat much, so the stream is long
    auto bytes = MakeLevels(TextureLogic::FORMAT_DXT5, 256, 128, true);
    std::uint32_t seed = 12345;
    for (auto& level : bytes)
        for (auto& b : level)
            b = static_cast<std::uint8_t>((seed = seed * 1103515245 + 12345) >> 24);
    const auto file = TextureLogic::WriteIwi(header, AsLevels(bytes, 256, 128));
    // a zip's entry is the zlib stream without its 2-byte header and 4-byte checksum
    const auto zlib = IWXMVM::Deflate::Zlib(file);
    const std::span<const std::uint8_t> raw(zlib.data() + 2, zlib.size() - 6);
    const auto start = IWXMVM::Inflate::RawPrefix(raw.first(std::min<std::size_t>(raw.size(), 4096)), 48);
    CHECK(start && std::equal(start->begin(), start->end(), file.begin()));
    const auto parsed = TextureLogic::ParseIwiHeader(*start);
    CHECK(parsed && parsed->width == 256 && parsed->height == 128 && parsed->format == TextureLogic::FORMAT_DXT5);
    CHECK(raw.size() > 4096);
    // too little of the stream
    CHECK(!IWXMVM::Inflate::RawPrefix(raw.first(3), 48));
}
