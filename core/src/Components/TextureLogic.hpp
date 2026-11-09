#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace IWXMVM::Components::TextureLogic
{
    // Texture packs, apart from the game and Direct3D so the tests in tests/actors-tests,
    // the mod and Iris Studio build it alike: the game's .iwi files and the pack's DDS files, which textures a pack
    // upscales and how much, the build's plan, the pack's index, the switches Iris Studio sets, the video memory the mod
    // gives the pack (its budget), the checks of an imported pack, and the line the Look tab shows.

    // ---- The game's textures

    // .iwi (version 13) formats: DXT1, DXT3, DXT5; the game's others (uncompressed) aren't upscaled
    inline constexpr std::uint8_t FORMAT_DXT1 = 11;
    inline constexpr std::uint8_t FORMAT_DXT3 = 12;
    inline constexpr std::uint8_t FORMAT_DXT5 = 13;
    // and its flags: one level only, a cube map, a volume, streamed in as drawn
    inline constexpr std::uint8_t IWI_NO_MIPS = 0x2;
    inline constexpr std::uint8_t IWI_CUBE = 0x4;
    inline constexpr std::uint8_t IWI_VOLUME = 0x8;
    inline constexpr std::uint8_t IWI_STREAMING = 0x10;
    inline constexpr std::size_t IWI_HEADER_SIZE = 48;

    // The header: "IWi" 13, format, flags, width, height, depth (u16 each), gamma (2.0 for colour maps, 0 for normal
    // maps) and fileSizeForPicmip[8] (the file's size with the levels from the smallest up to level i). The levels
    // follow, smallest first.
    struct IwiHeader
    {
        std::uint8_t format = 0;
        std::uint8_t flags = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t depth = 1;
        float gamma = 0.0f;
        std::array<std::uint32_t, 8> fileSizeForPicmip{};
    };
    // a level of a texture, its bytes as the file has them
    struct Level
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::span<const std::uint8_t> data;
    };
    struct Iwi
    {
        IwiHeader header;
        std::vector<Level> levels;  // largest first; none for a format that isn't DXT
    };
    std::optional<IwiHeader> ParseIwiHeader(std::span<const std::uint8_t> file);
    // a whole file: its header and, for DXT, its levels (cut short, none)
    std::optional<Iwi> ParseIwi(std::span<const std::uint8_t> file);
    // the file of a header and its levels (largest first), as the game's own files are written
    std::vector<std::uint8_t> WriteIwi(const IwiHeader& header, const std::vector<Level>& levels);

    bool IsDxt(std::uint8_t format);
    // the bytes of a level of DXT (blocks of 4 x 4: 8 bytes in DXT1, 16 in DXT3 and DXT5) or of 32-bit texels
    // (format 0)
    std::uint64_t GetLevelBytes(std::uint8_t format, std::uint32_t width, std::uint32_t height);
    // of every level down to 1 x 1
    std::uint64_t GetChainBytes(std::uint8_t format, std::uint32_t width, std::uint32_t height);
    // how many levels down to 1 x 1
    std::uint32_t GetLevelCount(std::uint32_t width, std::uint32_t height);

    // DDS (the pack's files): DXT1/3/5 with their legacy header, or 32-bit A8R8G8B8
    struct DdsInfo
    {
        std::uint8_t format = 0;  // FORMAT_DXT1/3/5, or 0 for 32-bit A8R8G8B8
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t levels = 1;
        std::size_t dataOffset = 128;
        bool complete = false;  // the file holds all its levels
    };
    inline constexpr std::size_t DDS_HEADER_SIZE = 128;
    // a DDS whose format is DXT1/3/5 or A8R8G8B8, from its first bytes (DDS_HEADER_SIZE at least) and its size
    std::optional<DdsInfo> ParseDdsHeader(std::span<const std::uint8_t> head, std::uint64_t fileSize);
    // a DDS of DXT levels (largest first), or of one level of 32-bit texels (format 0: R, G, B, A bytes, written as
    // A8R8G8B8)
    std::vector<std::uint8_t> WriteDds(std::uint8_t format, const std::vector<Level>& levels);
    // Where a level of a DDS is in its file: the level of that size, none when it has none
    struct DdsLevel
    {
        std::uint32_t index = 0;
        std::size_t offset = 0;
        std::size_t bytes = 0;
    };
    std::optional<DdsLevel> FindDdsLevel(const DdsInfo& info, std::uint32_t width, std::uint32_t height);
    // A pack's file with the game's own levels in place of its levels of those sizes, byte for byte, the game's size
    // and below. A spec map's gloss in alpha climbs level by level in the game's files, which a filter of the top level
    // can't give. None when the file isn't of the game's format or has no level of its size.
    std::optional<std::vector<std::uint8_t>> KeepGameLevels(std::span<const std::uint8_t> dds, const Iwi& game);
    // The same for the alpha alone of a colour map in DXT3 or DXT5, whose colour stays the pack's: the game's alpha
    // keeps how much of a fence or of grass shows at each level, where a filter thins it out to nothing. A
    // block of those formats has its alpha in its first 8 bytes.
    std::optional<std::vector<std::uint8_t>> KeepGameAlpha(std::span<const std::uint8_t> dds, const Iwi& game);
    // The bytes of an .iwi file up to and with that level (they are smallest first)
    std::uint64_t GetIwiBytesThrough(const IwiHeader& header, std::uint32_t level);

    // ---- What is upscaled

    enum class Kind
    {
        Colour,
        Normal,  // DXT5 with x in alpha and y in green (red and blue copy green)
        Spec,    // spec, gloss and masks
        Ui,      // HUD, menus, icons
        Effect,  // effects and sky
        Other,
    };
    // by the image's name, as the research's inventory counted them; a "normal" map that isn't DXT5 is Spec
    // (resized, not rebuilt)
    Kind GetKind(std::string_view name, std::uint8_t format = FORMAT_DXT5);
    std::string_view GetName(Kind kind);
    // whether a pack upscales an image (AD1): colour, normal and spec maps in DXT, flat (not cube maps or volumes)
    bool IsUpscaled(std::string_view name, const IwiHeader& header);
    // how a texture is upscaled: through the model (colour and normal maps, AC1) or a plain resize (spec maps)
    bool UsesModel(Kind kind);
    // The level of a game's texture a check of a pack compares, its index in Iwi::levels: the game's size, where a good
    // colour or normal map is within a few levels of 255 of the game's whatever its detail; a spec map's first with no
    // side above CHECK_SIDE, as what goes wrong in one is in its lower levels
    inline constexpr std::uint32_t CHECK_SIDE = 64;
    std::uint32_t GetCheckLevel(Kind kind, std::uint32_t width, std::uint32_t height, bool hasLevels);

    // AB1: 4x or 2x, no side past MAX_SIDE
    inline constexpr std::uint32_t MAX_SIDE = 4096;
    // the largest of 1, 2, 4 up to scale that keeps both sides at MAX_SIDE at most
    std::uint32_t GetScale(std::uint32_t width, std::uint32_t height, std::uint32_t scale);
    // a texture's bytes in a pack at a scale, every level
    std::uint64_t GetPackBytes(std::uint8_t format, std::uint32_t width, std::uint32_t height, std::uint32_t scale);
    // AB1: the scale the builder starts at on a card with that much video memory (2x under 8 GB, as Windows counts a
    // card of 8 GB: 8,192 MB less what the card keeps)
    std::uint32_t GetDefaultScale(std::uint64_t videoMemoryBytes);

    // ---- The parts a pack is built for: maps, and the guns and hands

    // a zone of the game a part is built from (mp_nuked, common_mp)
    inline constexpr std::string_view GUNS_ZONE = "common_mp";
    enum class PartKind
    {
        Guns,
        Multiplayer,
        Campaign,
        Zombies,
    };
    // the name Iris Studio and the Look tab show for a zone: "Nuketown", "Guns and hands", "Kino der Toten"; the zone
    // itself for one not known
    std::string GetPartName(std::string_view zone);
    // which list a zone is in; none for zones that aren't a map (ui_mp, patch_mp, frontend)
    std::optional<PartKind> GetPartKind(std::string_view zone);
    // the order Iris Studio lists them in: the guns and hands, then the multiplayer, campaign and Zombies maps, each by
    // name
    bool ListsBefore(std::string_view a, std::string_view b);

    // A texture of the game that a part's zone names, as the iwds have it
    struct Texture
    {
        IwiHeader header;
        std::string iwd;  // the iwd it is read from (the first in the game's search order)
        std::uint32_t size = 0;
        std::uint32_t crc = 0;  // the zip's CRC-32 of the .iwi: a game update that changes it builds it again
    };
    // what a pack built before has of an image (from its index)
    struct Built
    {
        std::uint32_t scale = 0;
        std::uint32_t size = 0;
        std::uint32_t crc = 0;
        bool wrong = false;  // a check found it wrong: made again
    };

    // What a build of some parts at a scale does: each texture once, however many of the parts use it, and those built
    // already at that scale or more (from the same game file) skipped
    struct Plan
    {
        std::vector<std::string> images;  // to build, by name
        std::uint64_t bytes = 0;          // they take in the pack
        std::uint64_t modelTexels = 0;    // going through the model (of the game's size)
        std::uint64_t plainTexels = 0;    // resized
        std::size_t shared = 0;           // of the parts' textures, those counted once already
        std::size_t built = 0;            // of the parts' textures, those built before
    };
    Plan MakePlan(const std::vector<std::vector<std::string>>& partImages,
                  const std::unordered_map<std::string, Texture>& textures, std::uint32_t scale,
                  const std::unordered_map<std::string, Built>& built);

    // The rates measured on one PC: Nomos8kSC 5.9 s a megatexel of input on an RTX 4070
    // Laptop, 5 s to load the model per run; texconv 26.5 s a GB of 4x output and 32 s of 2x on 12 threads, mostly
    // alongside the model
    inline constexpr double MODEL_SECONDS_PER_MEGATEXEL = 5.9;
    inline constexpr double MODEL_LOAD_SECONDS = 5.0;
    inline constexpr double PLAIN_SECONDS_PER_MEGATEXEL = 0.2;
    inline constexpr double COMPRESS_SECONDS_PER_GB_4X = 26.5;
    inline constexpr double COMPRESS_SECONDS_PER_GB_2X = 32.0;
    // the textures the model takes in one run (its model loaded once)
    inline constexpr std::size_t MODEL_BATCH = 48;
    double EstimateSeconds(const Plan& plan, std::uint32_t scale);
    // "about 25 minutes", "about 1.5 hours", "about a minute"
    std::string FormatDuration(double seconds);

    // ---- The pack on disk: <storage>\textures\v1\<image>.dds and index.json, the imported ones in
    // <storage>\textures\imported\<name>\ with an index of their own, and use.json (Iris Studio's switches)

    inline constexpr std::uint32_t INDEX_VERSION = 1;
    // Of the build that made an image. Before version 2 texconv lost the colour under low alpha
    // in every level but the first, a spec map's lower levels weren't the game's, and a run of the model that gave back
    // black went into the pack; an image of an older build is checked against the game's (TextureBuild::IsWrong) and
    // made again only when wrong.
    inline constexpr std::uint32_t BUILD_VERSION = 2;
    inline constexpr std::string_view BUILT_FOLDER = "v1";
    inline constexpr std::string_view IMPORTED_FOLDER = "imported";
    inline constexpr std::string_view INDEX_FILE = "index.json";
    inline constexpr std::string_view USE_FILE = "use.json";
    // AC1: the model's credit (CC-BY 4.0), in the index and Iris Studio's About
    inline constexpr std::string_view MODEL_NAME = "4xNomos8kSC";
    inline constexpr std::string_view MODEL_CREDIT = "4xNomos8kSC by Helaman (CC-BY 4.0)";

    struct PackImage
    {
        std::uint32_t scale = 1;
        std::uint8_t format = FORMAT_DXT1;  // of the pack's file (0 for 32-bit)
        std::uint32_t width = 0;            // of the pack's file
        std::uint32_t height = 0;
        std::uint64_t bytes = 0;  // the file's
        Built source;             // the game's file it was made from (scale is the pack's)
        std::vector<std::string> parts;  // the zones it was built for
        std::uint32_t build = 1;  // the BUILD_VERSION that made it, or that a check found it right under
    };
    struct PackPart
    {
        std::uint32_t scale = 0;
        std::size_t images = 0;
        std::uint64_t bytes = 0;  // of its images, those shared with other parts too
    };
    struct PackIndex
    {
        std::string name;       // an imported pack's (its folder's name); empty for the built one
        bool imported = false;
        std::string model;      // what made it: MODEL_CREDIT for a build
        std::map<std::string, PackImage> images;
        std::map<std::string, PackPart> parts;  // a built pack's, by zone
    };
    nlohmann::json ToJson(const PackIndex& index);
    // throws on an index it can't read
    PackIndex IndexFromJson(const nlohmann::json& json);
    // the bytes of a pack's images
    std::uint64_t GetPackBytes(const PackIndex& index);
    // the parts' figures again from the images (after a build or a removal)
    void CountParts(PackIndex& index);
    // a part taken out of a built pack: the images only it used go (their names returned, for their files)
    std::vector<std::string> RemovePart(PackIndex& index, std::string_view zone);

    // AJ1: what an image is drawn on, by the assets that use it
    enum class Group
    {
        Maps,
        Players,
        Weapons,  // and killstreaks
    };
    inline constexpr std::array<Group, 3> GROUPS{Group::Maps, Group::Players, Group::Weapons};
    std::string_view GetName(Group group);
    std::optional<Group> FindGroup(std::string_view name);
    // an image several groups use follows the first of Weapons, Players, Maps; one nothing found uses is
    // the map's
    Group GetGroup(bool weapons, bool players);

    // AK1 and AL1: Iris Studio's switches, read by the mod at each demo's load
    struct Use
    {
        std::set<std::string> partsOff;     // built parts (zones) ticked off
        std::set<std::string> importedOff;  // imported packs (their names) ticked off
        std::set<Group> groupsOff;

        bool operator==(const Use&) const = default;
    };
    nlohmann::json ToJson(const Use& use);
    // what doesn't read is on
    Use UseFromJson(const nlohmann::json& json);
    // "Weapons", "Players and Weapons"; empty when every group is on
    std::string DescribeGroupsOff(const Use& use);

    // ---- What the mod draws

    // a pack's texture for an image of the game
    struct Source
    {
        std::filesystem::path file;
        std::uint32_t scale = 1;
        std::string pack;  // the imported pack's name; empty for the built one
    };
    // A pack and where its files are
    struct Folder
    {
        std::filesystem::path path;
        PackIndex index;
    };
    // The textures to draw on a map, by image: the built pack's for the parts on (one of the zones loaded now that the
    // image was built for, or any of its parts when none of them is loaded), and the imported packs' on, which win
    // over the built one (AH2). Groups are applied by the mod, which knows them.
    std::unordered_map<std::string, Source> Resolve(const std::vector<Folder>& packs, const Use& use,
                                                    const std::vector<std::string>& loadedZones);

    // ---- The video memory the pack takes (AF1)

    // what the card has free when a demo loads, less a gigabyte for the game and the captures (shown in the Look tab)
    inline constexpr std::uint64_t BUDGET_RESERVE = 1ull << 30;
    inline constexpr std::uint64_t MIN_BUDGET = 256ull << 20;
    std::uint64_t GetBudget(std::uint64_t freeAtLoad);
    // a pack's texture the card holds
    struct Resident
    {
        std::size_t id = 0;
        std::uint64_t bytes = 0;
        std::uint64_t lastDrawn = 0;  // the frame it was last drawn in
    };
    // Which to let go for a texture of needed bytes to fit the budget: the least recently drawn first, never one drawn
    // in this frame; none when it fits already, nothing when it can't fit even so
    std::optional<std::vector<std::size_t>> ChooseEvictions(std::vector<Resident> residents, std::uint64_t inUse,
                                                            std::uint64_t needed, std::uint64_t budget,
                                                            std::uint64_t frame);

    // ---- Importing (AH2)

    enum class ImportVerdict
    {
        Fits,
        NotGameName,  // no image of the game has that name
        NotDxt,       // not DXT1/3/5 or 32-bit
        NoMips,       // without its levels
        NotSameShape, // not the game's width and height times a power of two
        TooBig,       // a side past MAX_SIDE
        Unreadable,
        Wrong,        // its colour gone or one flat colour, as an old build left them (TextureBuild::IsBroken)
    };
    // A DDS of an imported folder, named after an image (the name in lower case), its header if it reads, and the
    // game's texture of that name if there is one
    ImportVerdict CheckImport(const std::optional<DdsInfo>& dds, const std::optional<IwiHeader>& game);
    // its scale against the game's (1 for one of the same size)
    std::uint32_t GetImportScale(const DdsInfo& dds, const IwiHeader& game);
    struct ImportSummary
    {
        std::size_t fits = 0;
        std::uint64_t bytes = 0;  // of those that fit
        std::map<ImportVerdict, std::size_t> left;  // left out, why
        std::size_t overBuilt = 0;  // fitting images the built pack has too (the import wins)
    };
    // "14 not the game's names, 2 not DXT"; "None" when nothing is left out
    std::string DescribeLeftOut(const ImportSummary& summary);
    // a pack's name from its folder's: what Windows allows in a folder name, at most 40 characters; "Imported" when
    // nothing is left
    std::string GetImportName(std::string_view folderName);

    // ---- The Look tab's line under Textures (AG1)

    struct Status
    {
        std::string map;              // "Nuketown"
        std::vector<std::string> packs;  // "Nuketown 4x", "Guns and hands 4x", "HD Bricks (imported)"
        bool mapHasPack = false;      // the map has a built part or an imported pack draws on it
        bool on = false;              // the row's Pack
        std::size_t images = 0;       // the pack's images of the map
        std::size_t drawn = 0;        // of them, made on the card
        std::size_t waiting = 0;      // asked for, not made yet
        std::uint64_t bytes = 0;      // of the pack's images of the map, on disk
        std::uint64_t inUse = 0;      // on the card
        std::uint64_t budget = 0;
        std::string groupsOff;        // DescribeGroupsOff
        bool repair = false;          // a built image drawn here is of an older build
    };
    // the line's end while wrong images are left, drawn in the note's colour (AT1)
    inline constexpr std::string_view REPAIR_NOTE = "Repair in Iris Studio";
    // "Pack: Nuketown 4x, Guns and hands 4x · 1.9 GB / 5.0 GB · Weapons off", "Pack: Nuketown 4x · loading 214 / 922",
    // "Pack: Nuketown 4x · 3.4 GB" (Game), "No pack for Nuketown. Build one in Iris Studio.", and "· Repair in Iris
    // Studio" at the end while wrong images are left
    std::string GetStatusLine(const Status& status);
    // the line's start, drawn brighter: "Pack: Nuketown 4x" (its length in the line)
    std::size_t GetStatusLead(const Status& status);
    // "1.9 GB" (powers of 1024, one decimal)
    std::string FormatGigabytes(std::uint64_t bytes);
}  // namespace IWXMVM::Components::TextureLogic
