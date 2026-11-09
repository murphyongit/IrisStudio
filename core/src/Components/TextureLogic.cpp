#include "StdInclude.hpp"
#include "TextureLogic.hpp"

#include <cctype>
#include <cstring>
#include <ranges>
#include <set>

namespace IWXMVM::Components::TextureLogic
{
    namespace
    {
        constexpr std::string_view IWI_MAGIC = "IWi";
        constexpr std::uint8_t IWI_VERSION = 13;

        std::string Lower(std::string_view text)
        {
            std::string lower(text);
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower;
        }

        template <typename T>
        T Read(std::span<const std::uint8_t> bytes, std::size_t at)
        {
            T value{};
            std::memcpy(&value, bytes.data() + at, sizeof(T));
            return value;
        }

        template <typename T>
        void Append(std::vector<std::uint8_t>& out, T value)
        {
            const auto at = out.size();
            out.resize(at + sizeof(T));
            std::memcpy(out.data() + at, &value, sizeof(T));
        }

        bool EndsWith(std::string_view text, std::string_view end)
        {
            return text.size() >= end.size() && text.substr(text.size() - end.size()) == end;
        }

        // the name without a last digit (brick_n2 -> brick_n)
        std::string_view WithoutDigit(std::string_view name)
        {
            if (!name.empty() && std::isdigit(static_cast<unsigned char>(name.back())))
                name.remove_suffix(1);
            return name;
        }

        bool EndsWithAny(std::string_view name, std::initializer_list<std::string_view> ends)
        {
            name = WithoutDigit(name);
            return std::ranges::any_of(ends, [&](std::string_view end) { return EndsWith(name, end); });
        }

        // a word of the name starts with one of these: at its start, or after _ or /
        bool HasWordStarting(std::string_view name, std::initializer_list<std::string_view> starts)
        {
            for (std::size_t at = 0; at < name.size(); ++at)
            {
                if (at > 0 && name[at - 1] != '_' && name[at - 1] != '/')
                    continue;
                const auto rest = name.substr(at);
                if (std::ranges::any_of(starts, [&](std::string_view start) { return rest.starts_with(start); }))
                    return true;
            }
            return false;
        }

        // one of the name's words (split at _) is one of these
        bool HasWord(std::string_view name, std::initializer_list<std::string_view> words)
        {
            while (!name.empty())
            {
                const auto end = name.find('_');
                const auto word = WithoutDigit(name.substr(0, end));
                if (std::ranges::find(words, word) != words.end())
                    return true;
                if (end == std::string_view::npos)
                    break;
                name.remove_prefix(end + 1);
            }
            return false;
        }

        struct PartName
        {
            std::string_view zone;
            std::string_view name;
            PartKind kind;
        };
        // multiplayer's maps (with the map packs), then the campaign's and Zombies' as campaign-maps.json names them
        constexpr std::array<PartName, 55> PARTS{{
            {GUNS_ZONE, "Guns and hands", PartKind::Guns},
            {"mp_array", "Array", PartKind::Multiplayer},
            {"mp_cracked", "Cracked", PartKind::Multiplayer},
            {"mp_crisis", "Crisis", PartKind::Multiplayer},
            {"mp_firingrange", "Firing Range", PartKind::Multiplayer},
            {"mp_duga", "Grid", PartKind::Multiplayer},
            {"mp_hanoi", "Hanoi", PartKind::Multiplayer},
            {"mp_cairo", "Havana", PartKind::Multiplayer},
            {"mp_havoc", "Jungle", PartKind::Multiplayer},
            {"mp_cosmodrome", "Launch", PartKind::Multiplayer},
            {"mp_nuked", "Nuketown", PartKind::Multiplayer},
            {"mp_radiation", "Radiation", PartKind::Multiplayer},
            {"mp_mountain", "Summit", PartKind::Multiplayer},
            {"mp_villa", "Villa", PartKind::Multiplayer},
            {"mp_russianbase", "WMD", PartKind::Multiplayer},
            {"mp_berlinwall2", "Berlin Wall", PartKind::Multiplayer},
            {"mp_discovery", "Discovery", PartKind::Multiplayer},
            {"mp_kowloon", "Kowloon", PartKind::Multiplayer},
            {"mp_stadium", "Stadium", PartKind::Multiplayer},
            {"mp_gridlock", "Convoy", PartKind::Multiplayer},
            {"mp_hotel", "Hotel", PartKind::Multiplayer},
            {"mp_outskirts", "Stockpile", PartKind::Multiplayer},
            {"mp_zoo", "Zoo", PartKind::Multiplayer},
            {"mp_area51", "Hangar 18", PartKind::Multiplayer},
            {"mp_drivein", "Drive-In", PartKind::Multiplayer},
            {"mp_golfcourse", "Hazard", PartKind::Multiplayer},
            {"mp_silo", "Silo", PartKind::Multiplayer},
            {"frontend", "Interrogation", PartKind::Campaign},
            {"cuba", "Operation 40", PartKind::Campaign},
            {"vorkuta", "Vorkuta", PartKind::Campaign},
            {"pentagon", "U.S.D.D.", PartKind::Campaign},
            {"flashpoint", "Executive Order", PartKind::Campaign},
            {"khe_sanh", "S.O.G.", PartKind::Campaign},
            {"hue_city", "The Defector", PartKind::Campaign},
            {"kowloon", "Numbers", PartKind::Campaign},
            {"fullahead", "Project Nova", PartKind::Campaign},
            {"creek_1", "Victor Charlie", PartKind::Campaign},
            {"river", "Crash Site", PartKind::Campaign},
            {"wmd_sr71", "WMD (SR-71)", PartKind::Campaign},
            {"wmd", "WMD (campaign)", PartKind::Campaign},
            {"pow", "Payback", PartKind::Campaign},
            {"rebirth", "Rebirth", PartKind::Campaign},
            {"int_escape", "Revelations", PartKind::Campaign},
            {"underwaterbase", "Redemption", PartKind::Campaign},
            {"outro", "Epilogue", PartKind::Campaign},
            {"zombie_theater", "Kino der Toten", PartKind::Zombies},
            {"zombie_pentagon", "Five", PartKind::Zombies},
            {"zombie_cosmodrome", "Ascension", PartKind::Zombies},
            {"zombie_coast", "Call of the Dead", PartKind::Zombies},
            {"zombie_temple", "Shangri-La", PartKind::Zombies},
            {"zombie_moon", "Moon", PartKind::Zombies},
            {"zombie_cod5_prototype", "Nacht der Untoten", PartKind::Zombies},
            {"zombie_cod5_asylum", "Verruckt", PartKind::Zombies},
            {"zombie_cod5_sumpf", "Shi No Numa", PartKind::Zombies},
            {"zombie_cod5_factory", "Der Riese", PartKind::Zombies},
        }};

        const PartName* FindPart(std::string_view zone)
        {
            const auto lower = Lower(zone);
            const auto found = std::ranges::find(PARTS, std::string_view(lower), &PartName::zone);
            return found == PARTS.end() ? nullptr : &*found;
        }

        constexpr std::array<std::pair<Group, std::string_view>, 3> GROUP_NAMES{
            {{Group::Maps, "Maps"}, {Group::Players, "Players"}, {Group::Weapons, "Weapons"}}};

        std::string JoinPacks(const std::vector<std::string>& packs)
        {
            std::string text;
            for (std::size_t i = 0; i < packs.size(); ++i)
                text += (i ? ", " : "") + packs[i];
            return text;
        }

        std::string JoinAnd(const std::vector<std::string>& items)
        {
            std::string text;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                if (i > 0)
                    text += i + 1 == items.size() ? " and " : ", ";
                text += items[i];
            }
            return text;
        }
    }  // namespace

    std::optional<IwiHeader> ParseIwiHeader(std::span<const std::uint8_t> file)
    {
        if (file.size() < IWI_HEADER_SIZE || std::memcmp(file.data(), IWI_MAGIC.data(), 3) != 0 ||
            file[3] != IWI_VERSION)
            return std::nullopt;
        IwiHeader header;
        header.format = file[4];
        header.flags = file[5];
        header.width = Read<std::uint16_t>(file, 6);
        header.height = Read<std::uint16_t>(file, 8);
        header.depth = Read<std::uint16_t>(file, 10);
        header.gamma = Read<float>(file, 12);
        for (std::size_t i = 0; i < header.fileSizeForPicmip.size(); ++i)
            header.fileSizeForPicmip[i] = Read<std::uint32_t>(file, 16 + i * 4);
        return header;
    }

    std::optional<Iwi> ParseIwi(std::span<const std::uint8_t> file)
    {
        const auto header = ParseIwiHeader(file);
        if (!header)
            return std::nullopt;
        Iwi iwi{*header, {}};
        if (!IsDxt(header->format) || header->width == 0 || header->height == 0)
            return iwi;
        // the levels, largest first, taken from the end of the file (smallest first there)
        std::uint32_t width = header->width, height = header->height;
        std::size_t end = file.size();
        while (true)
        {
            const auto bytes = static_cast<std::size_t>(GetLevelBytes(header->format, width, height));
            if (bytes > end - IWI_HEADER_SIZE)
                return std::nullopt;
            iwi.levels.push_back({width, height, file.subspan(end - bytes, bytes)});
            end -= bytes;
            if ((width == 1 && height == 1) || (header->flags & IWI_NO_MIPS))
                break;
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        return iwi;
    }

    std::vector<std::uint8_t> WriteIwi(const IwiHeader& header, const std::vector<Level>& levels)
    {
        std::vector<std::uint8_t> out(IWI_MAGIC.begin(), IWI_MAGIC.end());
        out.push_back(IWI_VERSION);
        out.push_back(header.format);
        out.push_back(header.flags);
        const auto width = static_cast<std::uint16_t>(levels.empty() ? header.width : levels.front().width);
        const auto height = static_cast<std::uint16_t>(levels.empty() ? header.height : levels.front().height);
        Append(out, width);
        Append(out, height);
        Append(out, header.depth);
        Append(out, header.gamma);
        // the file's size with the levels from the smallest up to level i: the whole file, less each larger level; past
        // the last level, the smallest stays (a 64 x 32 texture's 7 levels: the last two are the same)
        std::size_t body = 0;
        for (const auto& level : levels)
            body += level.data.size();
        std::size_t size = IWI_HEADER_SIZE + body;
        for (std::size_t i = 0; i < 8; ++i)
        {
            Append(out, static_cast<std::uint32_t>(size));
            if (i + 1 < levels.size())
                size -= levels[i].data.size();
        }
        for (auto level = levels.rbegin(); level != levels.rend(); ++level)
            out.insert(out.end(), level->data.begin(), level->data.end());
        return out;
    }

    bool IsDxt(std::uint8_t format)
    {
        return format == FORMAT_DXT1 || format == FORMAT_DXT3 || format == FORMAT_DXT5;
    }

    std::uint64_t GetLevelBytes(std::uint8_t format, std::uint32_t width, std::uint32_t height)
    {
        if (!IsDxt(format))
            return std::uint64_t{width} * height * 4;
        const std::uint64_t blocks = std::uint64_t{std::max(1u, (width + 3) / 4)} * std::max(1u, (height + 3) / 4);
        return blocks * (format == FORMAT_DXT1 ? 8 : 16);
    }

    std::uint64_t GetChainBytes(std::uint8_t format, std::uint32_t width, std::uint32_t height)
    {
        std::uint64_t bytes = 0;
        while (true)
        {
            bytes += GetLevelBytes(format, width, height);
            if (width <= 1 && height <= 1)
                return bytes;
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
    }

    std::uint32_t GetLevelCount(std::uint32_t width, std::uint32_t height)
    {
        std::uint32_t count = 1;
        while (width > 1 || height > 1)
        {
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
            ++count;
        }
        return count;
    }

    std::optional<DdsInfo> ParseDdsHeader(std::span<const std::uint8_t> head, std::uint64_t fileSize)
    {
        if (head.size() < DDS_HEADER_SIZE || std::memcmp(head.data(), "DDS ", 4) != 0 ||
            Read<std::uint32_t>(head, 4) != 124)
            return std::nullopt;
        DdsInfo info;
        info.height = Read<std::uint32_t>(head, 12);
        info.width = Read<std::uint32_t>(head, 16);
        info.levels = std::max(1u, Read<std::uint32_t>(head, 28));
        const auto pixelFlags = Read<std::uint32_t>(head, 80);
        const auto fourcc = std::string_view(reinterpret_cast<const char*>(head.data()) + 84, 4);
        if (pixelFlags & 0x4)
        {
            if (fourcc == "DXT1")
                info.format = FORMAT_DXT1;
            else if (fourcc == "DXT3")
                info.format = FORMAT_DXT3;
            else if (fourcc == "DXT5")
                info.format = FORMAT_DXT5;
            else
                return std::nullopt;
        }
        else
        {
            // A8R8G8B8: 32 bits, red 0xFF0000, alpha 0xFF000000
            if (Read<std::uint32_t>(head, 88) != 32 || Read<std::uint32_t>(head, 92) != 0x00FF0000 ||
                Read<std::uint32_t>(head, 104) != 0xFF000000)
                return std::nullopt;
            info.format = 0;
        }
        if (info.width == 0 || info.height == 0 || info.width > 16384 || info.height > 16384)
            return std::nullopt;
        std::uint64_t bytes = 0;
        std::uint32_t width = info.width, height = info.height;
        for (std::uint32_t level = 0; level < info.levels; ++level)
        {
            bytes += GetLevelBytes(info.format, width, height);
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        info.complete = fileSize >= DDS_HEADER_SIZE + bytes;
        return info;
    }

    std::vector<std::uint8_t> WriteDds(std::uint8_t format, const std::vector<Level>& levels)
    {
        std::vector<std::uint8_t> out;
        const auto width = levels.empty() ? 0u : levels.front().width;
        const auto height = levels.empty() ? 0u : levels.front().height;
        const bool dxt = IsDxt(format);
        out.insert(out.end(), {'D', 'D', 'S', ' '});
        Append<std::uint32_t>(out, 124);
        // caps, height, width, pixel format, mip count, and linear size (DXT) or pitch
        Append<std::uint32_t>(out, 0x1 | 0x2 | 0x4 | 0x1000 | (dxt ? 0x80000 : 0x8) | (levels.size() > 1 ? 0x20000 : 0));
        Append<std::uint32_t>(out, height);
        Append<std::uint32_t>(out, width);
        Append<std::uint32_t>(out, dxt ? static_cast<std::uint32_t>(GetLevelBytes(format, width, height)) : width * 4);
        Append<std::uint32_t>(out, 0);
        Append<std::uint32_t>(out, static_cast<std::uint32_t>(levels.size()));
        out.resize(out.size() + 44);
        // the pixel format
        Append<std::uint32_t>(out, 32);
        if (dxt)
        {
            Append<std::uint32_t>(out, 0x4);
            const char* fourcc = format == FORMAT_DXT1 ? "DXT1" : format == FORMAT_DXT3 ? "DXT3" : "DXT5";
            out.insert(out.end(), fourcc, fourcc + 4);
            out.resize(out.size() + 20);
        }
        else
        {
            Append<std::uint32_t>(out, 0x40 | 0x1);
            Append<std::uint32_t>(out, 0);
            Append<std::uint32_t>(out, 32);
            Append<std::uint32_t>(out, 0x00FF0000);
            Append<std::uint32_t>(out, 0x0000FF00);
            Append<std::uint32_t>(out, 0x000000FF);
            Append<std::uint32_t>(out, 0xFF000000);
        }
        Append<std::uint32_t>(out, 0x1000 | (levels.size() > 1 ? 0x8 | 0x400000 : 0));
        out.resize(DDS_HEADER_SIZE);
        for (const auto& level : levels)
        {
            if (dxt)
            {
                out.insert(out.end(), level.data.begin(), level.data.end());
                continue;
            }
            // R, G, B, A to B, G, R, A
            const auto at = out.size();
            out.insert(out.end(), level.data.begin(), level.data.end());
            for (std::size_t i = at; i + 3 < out.size(); i += 4)
                std::swap(out[i], out[i + 2]);
        }
        return out;
    }

    std::optional<DdsLevel> FindDdsLevel(const DdsInfo& info, std::uint32_t width, std::uint32_t height)
    {
        std::size_t offset = info.dataOffset;
        std::uint32_t w = info.width, h = info.height;
        for (std::uint32_t level = 0; level < info.levels; ++level)
        {
            const auto bytes = static_cast<std::size_t>(GetLevelBytes(info.format, w, h));
            if (w == width && h == height)
                return DdsLevel{level, offset, bytes};
            offset += bytes;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        return std::nullopt;
    }

    namespace
    {
        // the game's levels over a pack's of their sizes, whole or a block's alpha alone
        std::optional<std::vector<std::uint8_t>> KeepLevels(std::span<const std::uint8_t> dds, const Iwi& game,
                                                            bool alphaOnly)
        {
            constexpr std::size_t BLOCK_BYTES = 16, ALPHA_BYTES = 8;
            const auto info = ParseDdsHeader(dds, dds.size());
            if (!info || !info->complete || info->format != game.header.format || game.levels.empty() ||
                (alphaOnly && info->format != FORMAT_DXT3 && info->format != FORMAT_DXT5))
                return std::nullopt;
            std::vector<std::uint8_t> out(dds.begin(), dds.end());
            bool first = true;
            for (const auto& level : game.levels)
            {
                const auto place = FindDdsLevel(*info, level.width, level.height);
                if (!place)
                {
                    if (first)
                        return std::nullopt;
                    break;
                }
                if (place->bytes != level.data.size())
                    return std::nullopt;
                if (alphaOnly)
                    for (std::size_t at = 0; at < place->bytes; at += BLOCK_BYTES)
                        std::memcpy(out.data() + place->offset + at, level.data.data() + at, ALPHA_BYTES);
                else
                    std::memcpy(out.data() + place->offset, level.data.data(), place->bytes);
                first = false;
            }
            return out;
        }
    }  // namespace

    std::optional<std::vector<std::uint8_t>> KeepGameLevels(std::span<const std::uint8_t> dds, const Iwi& game)
    {
        return KeepLevels(dds, game, false);
    }

    std::optional<std::vector<std::uint8_t>> KeepGameAlpha(std::span<const std::uint8_t> dds, const Iwi& game)
    {
        return KeepLevels(dds, game, true);
    }

    std::uint32_t GetCheckLevel(Kind kind, std::uint32_t width, std::uint32_t height, bool hasLevels)
    {
        std::uint32_t level = 0;
        while (kind == Kind::Spec && hasLevels && std::max(width, height) > CHECK_SIDE)
        {
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
            ++level;
        }
        return level;
    }

    std::uint64_t GetIwiBytesThrough(const IwiHeader& header, std::uint32_t level)
    {
        // the levels below it, smallest first in the file, then it
        std::uint32_t width = header.width, height = header.height;
        for (std::uint32_t i = 0; i < level; ++i)
        {
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        const bool mips = !(header.flags & IWI_NO_MIPS);
        return IWI_HEADER_SIZE + (mips ? GetChainBytes(header.format, width, height)
                                       : GetLevelBytes(header.format, width, height));
    }

    Kind GetKind(std::string_view name, std::uint8_t format)
    {
        const auto n = Lower(name);
        // the game's own names say what an image is; the order matters (a HUD image's _c is still the HUD's)
        if (n.starts_with("$") || n.starts_with("hud") || n.starts_with("menu") || n.starts_with("ui_") ||
            n.starts_with("compass") || n.starts_with("loadscreen") || n.starts_with("cac_") ||
            n.starts_with("em_") || n.starts_with("rank_") || n.starts_with("killicon") || n.starts_with("reticle") ||
            n.starts_with("mtl_hud") || n.starts_with("mp_hud") ||
            HasWordStarting(n, {"hud", "menu", "ui", "compass", "loadscreen", "cac", "emblem", "rank", "killicon",
                                "minimap", "font", "gamefont", "scoreboard", "headicon", "playlist", "lui", "flag_",
                                "faction", "logo", "cursor", "reticle", "overlay", "splash", "white", "black",
                                "gradient", "button"}))
            return Kind::Ui;
        if (n.starts_with("fx") || n.starts_with("sky") ||
            HasWord(n, {"fx", "fxt", "smoke", "fire", "flame", "flames", "spark", "sparks", "explosion", "muzzle",
                        "muzzleflash", "blood", "tracer", "glow", "flare", "lens", "lensflare", "splash", "ripple",
                        "ripples", "steam", "cloud", "clouds", "sky", "skybox", "lightning", "beam", "shockwave",
                        "distort", "distortion"}))
            return Kind::Effect;
        if (EndsWithAny(n, {"_n", "_nml", "_normal", "_nrm"}) || n.find("_n_") != std::string::npos)
            return format == FORMAT_DXT5 ? Kind::Normal : Kind::Spec;
        if (EndsWithAny(n, {"_s", "_g", "_spc", "_spec", "_gloss", "_sc", "_q", "_ao", "_m", "_o", "_mask", "_cos"}) ||
            n.starts_with("~~") || n.starts_with("~$"))
            return Kind::Spec;
        if (EndsWithAny(n, {"_c", "_col", "_color", "_d", "_diffuse", "_c_d"}) || n.starts_with("~-g"))
            return Kind::Colour;
        return Kind::Other;
    }

    std::string_view GetName(Kind kind)
    {
        switch (kind)
        {
            case Kind::Colour:
                return "colour";
            case Kind::Normal:
                return "normal";
            case Kind::Spec:
                return "spec";
            case Kind::Ui:
                return "ui";
            case Kind::Effect:
                return "effect";
            default:
                return "other";
        }
    }

    bool IsUpscaled(std::string_view name, const IwiHeader& header)
    {
        if (!IsDxt(header.format) || header.depth > 1 || (header.flags & (IWI_CUBE | IWI_VOLUME)) ||
            header.width < 4 || header.height < 4)
            return false;
        const auto kind = GetKind(name, header.format);
        return kind == Kind::Colour || kind == Kind::Normal || kind == Kind::Spec;
    }

    bool UsesModel(Kind kind)
    {
        return kind == Kind::Colour || kind == Kind::Normal;
    }

    std::uint32_t GetScale(std::uint32_t width, std::uint32_t height, std::uint32_t scale)
    {
        scale = std::max(1u, scale);
        while (scale > 1 && (std::uint64_t{width} * scale > MAX_SIDE || std::uint64_t{height} * scale > MAX_SIDE))
            scale /= 2;
        return scale;
    }

    std::uint64_t GetPackBytes(std::uint8_t format, std::uint32_t width, std::uint32_t height, std::uint32_t scale)
    {
        const auto s = GetScale(width, height, scale);
        return GetChainBytes(format, width * s, height * s);
    }

    std::uint32_t GetDefaultScale(std::uint64_t videoMemoryBytes)
    {
        // an 8 GB card reports a little under 8,192 MB (8,018 here); a 6 GB one about 6,000
        return videoMemoryBytes >= (7ull << 30) ? 4 : 2;
    }

    std::string GetPartName(std::string_view zone)
    {
        const auto part = FindPart(zone);
        return part ? std::string(part->name) : std::string(zone);
    }

    std::optional<PartKind> GetPartKind(std::string_view zone)
    {
        const auto part = FindPart(zone);
        return part ? std::optional(part->kind) : std::nullopt;
    }

    bool ListsBefore(std::string_view a, std::string_view b)
    {
        const auto kindA = GetPartKind(a), kindB = GetPartKind(b);
        const auto rank = [](std::optional<PartKind> kind) { return kind ? static_cast<int>(*kind) : 99; };
        if (rank(kindA) != rank(kindB))
            return rank(kindA) < rank(kindB);
        return Lower(GetPartName(a)) < Lower(GetPartName(b));
    }

    Plan MakePlan(const std::vector<std::vector<std::string>>& partImages,
                  const std::unordered_map<std::string, Texture>& textures, std::uint32_t scale,
                  const std::unordered_map<std::string, Built>& built)
    {
        Plan plan;
        std::set<std::string, std::less<>> seen;
        for (const auto& images : partImages)
            for (const auto& name : images)
            {
                const auto texture = textures.find(name);
                if (texture == textures.end() || !IsUpscaled(name, texture->second.header))
                    continue;
                if (!seen.insert(name).second)
                {
                    ++plan.shared;
                    continue;
                }
                const auto& header = texture->second.header;
                const auto wanted = GetScale(header.width, header.height, scale);
                if (const auto done = built.find(name); done != built.end() && !done->second.wrong &&
                                                        done->second.scale >= wanted &&
                                                        done->second.size == texture->second.size &&
                                                        done->second.crc == texture->second.crc)
                {
                    ++plan.built;
                    continue;
                }
                plan.images.push_back(name);
                plan.bytes += GetPackBytes(header.format, header.width, header.height, scale);
                const std::uint64_t texels = std::uint64_t{header.width} * header.height;
                if (UsesModel(GetKind(name, header.format)))
                    plan.modelTexels += texels;
                else
                    plan.plainTexels += texels;
            }
        return plan;
    }

    double EstimateSeconds(const Plan& plan, std::uint32_t scale)
    {
        if (plan.images.empty())
            return 0.0;
        const double model = plan.modelTexels / 1e6 * MODEL_SECONDS_PER_MEGATEXEL;
        const auto batches = static_cast<double>((plan.images.size() + MODEL_BATCH - 1) / MODEL_BATCH);
        const double load = plan.modelTexels ? batches * MODEL_LOAD_SECONDS : 0.0;
        const double plain = plan.plainTexels / 1e6 * PLAIN_SECONDS_PER_MEGATEXEL;
        const double compress = plan.bytes / double(1ull << 30) *
                                (scale >= 4 ? COMPRESS_SECONDS_PER_GB_4X : COMPRESS_SECONDS_PER_GB_2X);
        // compression runs on the processor while the card upscales: what is longer, and a tenth of the other
        return std::max(model + load, compress) + std::min(model + load, compress) * 0.1 + plain;
    }

    std::string FormatDuration(double seconds)
    {
        const auto minutes = std::lround(seconds / 60.0);
        if (minutes <= 1)
            return "about a minute";
        if (minutes < 90)
            return std::format("about {} minutes", minutes);
        return std::format("about {:.1f} hours", seconds / 3600.0);
    }

    nlohmann::json ToJson(const PackIndex& index)
    {
        nlohmann::json json{{"version", INDEX_VERSION}, {"model", index.model}};
        if (index.imported)
        {
            json["imported"] = true;
            json["name"] = index.name;
        }
        auto& parts = json["parts"] = nlohmann::json::object();
        for (const auto& [zone, part] : index.parts)
            parts[zone] = {{"scale", part.scale}, {"images", part.images}, {"bytes", part.bytes}};
        auto& images = json["images"] = nlohmann::json::object();
        for (const auto& [name, image] : index.images)
        {
            nlohmann::json entry{{"scale", image.scale},  {"format", image.format}, {"width", image.width},
                                 {"height", image.height}, {"bytes", image.bytes}};
            if (image.source.size || image.source.crc)
                entry["source"] = {{"size", image.source.size}, {"crc", image.source.crc}};
            if (image.build > 1)
                entry["build"] = image.build;
            if (!image.parts.empty())
                entry["parts"] = image.parts;
            images[name] = std::move(entry);
        }
        return json;
    }

    PackIndex IndexFromJson(const nlohmann::json& json)
    {
        if (json.at("version").get<std::uint32_t>() != INDEX_VERSION)
            throw std::runtime_error("another version of the texture pack's index");
        PackIndex index;
        index.model = json.value("model", "");
        index.imported = json.value("imported", false);
        index.name = json.value("name", "");
        if (const auto parts = json.find("parts"); parts != json.end())
            for (const auto& [zone, part] : parts->items())
                index.parts[zone] = {part.at("scale").get<std::uint32_t>(), part.value("images", std::size_t{0}),
                                     part.value("bytes", std::uint64_t{0})};
        for (const auto& [name, entry] : json.at("images").items())
        {
            PackImage image;
            image.scale = entry.at("scale").get<std::uint32_t>();
            image.format = entry.at("format").get<std::uint8_t>();
            image.width = entry.at("width").get<std::uint32_t>();
            image.height = entry.at("height").get<std::uint32_t>();
            image.bytes = entry.value("bytes", std::uint64_t{0});
            if (const auto source = entry.find("source"); source != entry.end())
                image.source = {image.scale, source->value("size", 0u), source->value("crc", 0u)};
            image.parts = entry.value("parts", std::vector<std::string>{});
            image.build = entry.value("build", 1u);
            index.images[Lower(name)] = std::move(image);
        }
        return index;
    }

    std::uint64_t GetPackBytes(const PackIndex& index)
    {
        std::uint64_t bytes = 0;
        for (const auto& image : index.images | std::views::values)
            bytes += image.bytes;
        return bytes;
    }

    void CountParts(PackIndex& index)
    {
        for (auto& part : index.parts | std::views::values)
        {
            part.images = 0;
            part.bytes = 0;
        }
        for (const auto& image : index.images | std::views::values)
            for (const auto& zone : image.parts)
                if (const auto part = index.parts.find(zone); part != index.parts.end())
                {
                    ++part->second.images;
                    part->second.bytes += image.bytes;
                }
    }

    std::vector<std::string> RemovePart(PackIndex& index, std::string_view zone)
    {
        std::vector<std::string> gone;
        for (auto image = index.images.begin(); image != index.images.end();)
        {
            auto& parts = image->second.parts;
            std::erase(parts, std::string(zone));
            if (parts.empty())
            {
                gone.push_back(image->first);
                image = index.images.erase(image);
            }
            else
                ++image;
        }
        index.parts.erase(std::string(zone));
        CountParts(index);
        return gone;
    }

    std::string_view GetName(Group group)
    {
        return std::ranges::find(GROUP_NAMES, group, &std::pair<Group, std::string_view>::first)->second;
    }

    std::optional<Group> FindGroup(std::string_view name)
    {
        const auto lower = Lower(name);
        for (const auto& [group, groupName] : GROUP_NAMES)
            if (Lower(groupName) == lower)
                return group;
        return std::nullopt;
    }

    Group GetGroup(bool weapons, bool players)
    {
        return weapons ? Group::Weapons : players ? Group::Players : Group::Maps;
    }

    nlohmann::json ToJson(const Use& use)
    {
        nlohmann::json groups = nlohmann::json::array();
        for (const auto group : use.groupsOff)
            groups.push_back(GetName(group));
        return {{"partsOff", use.partsOff}, {"importedOff", use.importedOff}, {"groupsOff", std::move(groups)}};
    }

    Use UseFromJson(const nlohmann::json& json)
    {
        Use use;
        if (!json.is_object())
            return use;
        const auto strings = [&](const char* key) {
            std::set<std::string> values;
            if (const auto found = json.find(key); found != json.end() && found->is_array())
                for (const auto& value : *found)
                    if (value.is_string())
                        values.insert(value.get<std::string>());
            return values;
        };
        use.partsOff = strings("partsOff");
        use.importedOff = strings("importedOff");
        for (const auto& name : strings("groupsOff"))
            if (const auto group = FindGroup(name))
                use.groupsOff.insert(*group);
        return use;
    }

    std::string DescribeGroupsOff(const Use& use)
    {
        std::vector<std::string> names;
        for (const auto group : GROUPS)
            if (use.groupsOff.contains(group))
                names.emplace_back(GetName(group));
        return JoinAnd(names);
    }

    std::unordered_map<std::string, Source> Resolve(const std::vector<Folder>& packs, const Use& use,
                                                    const std::vector<std::string>& loadedZones)
    {
        std::unordered_map<std::string, Source> sources;
        std::set<std::string> loaded;
        for (const auto& zone : loadedZones)
            loaded.insert(Lower(zone));
        // the built pack first, the imported ones over it
        for (const auto& pack : packs)
        {
            if (pack.index.imported)
                continue;
            for (const auto& [name, image] : pack.index.images)
            {
                bool anyLoaded = false, loadedOn = false, anyOn = false;
                for (const auto& zone : image.parts)
                {
                    const bool on = !use.partsOff.contains(zone);
                    anyOn |= on;
                    if (loaded.contains(Lower(zone)))
                    {
                        anyLoaded = true;
                        loadedOn |= on;
                    }
                }
                if (anyLoaded ? loadedOn : anyOn)
                    sources[name] = {pack.path / (name + ".dds"), image.scale, {}};
            }
        }
        for (const auto& pack : packs)
        {
            if (!pack.index.imported || use.importedOff.contains(pack.index.name))
                continue;
            for (const auto& [name, image] : pack.index.images)
                sources[name] = {pack.path / (name + ".dds"), image.scale, pack.index.name};
        }
        return sources;
    }

    std::uint64_t GetBudget(std::uint64_t freeAtLoad)
    {
        return freeAtLoad > BUDGET_RESERVE + MIN_BUDGET ? freeAtLoad - BUDGET_RESERVE : MIN_BUDGET;
    }

    std::optional<std::vector<std::size_t>> ChooseEvictions(std::vector<Resident> residents, std::uint64_t inUse,
                                                            std::uint64_t needed, std::uint64_t budget,
                                                            std::uint64_t frame)
    {
        std::vector<std::size_t> chosen;
        if (inUse + needed <= budget)
            return chosen;
        if (needed > budget)
            return std::nullopt;
        std::ranges::stable_sort(residents, {}, &Resident::lastDrawn);
        for (const auto& resident : residents)
        {
            if (resident.lastDrawn >= frame)
                break;
            chosen.push_back(resident.id);
            inUse -= std::min(inUse, resident.bytes);
            if (inUse + needed <= budget)
                return chosen;
        }
        return std::nullopt;
    }

    ImportVerdict CheckImport(const std::optional<DdsInfo>& dds, const std::optional<IwiHeader>& game)
    {
        if (!game)
            return ImportVerdict::NotGameName;
        if (!dds)
            return ImportVerdict::NotDxt;
        if (!dds->complete)
            return ImportVerdict::Unreadable;
        if (dds->width > MAX_SIDE || dds->height > MAX_SIDE)
            return ImportVerdict::TooBig;
        // the same shape at a power of two (or the game's size)
        const auto fits = [](std::uint32_t mine, std::uint32_t theirs) {
            for (std::uint32_t scale = 1; scale <= 16; scale *= 2)
                if (mine == theirs * scale)
                    return true;
            return false;
        };
        if (!fits(dds->width, game->width) || !fits(dds->height, game->height) ||
            dds->width / std::max<std::uint32_t>(1, game->width) !=
                dds->height / std::max<std::uint32_t>(1, game->height))
            return ImportVerdict::NotSameShape;
        if (dds->levels < GetLevelCount(dds->width, dds->height) && !(game->flags & IWI_NO_MIPS))
            return ImportVerdict::NoMips;
        return ImportVerdict::Fits;
    }

    std::uint32_t GetImportScale(const DdsInfo& dds, const IwiHeader& game)
    {
        return std::max<std::uint32_t>(1, dds.width / std::max<std::uint32_t>(1, game.width));
    }

    std::string DescribeLeftOut(const ImportSummary& summary)
    {
        std::vector<std::string> parts;
        for (const auto& [verdict, count] : summary.left)
        {
            if (count == 0)
                continue;
            const char* why = verdict == ImportVerdict::NotGameName    ? "not the game's names"
                              : verdict == ImportVerdict::NotDxt       ? "not DXT"
                              : verdict == ImportVerdict::NoMips       ? "without mipmaps"
                              : verdict == ImportVerdict::NotSameShape ? "not the game's shape"
                              : verdict == ImportVerdict::TooBig       ? "over 4,096 a side"
                              : verdict == ImportVerdict::Wrong        ? "wrong"
                                                                       : "cut short";
            parts.push_back(std::format("{} {}", count, why));
        }
        if (parts.empty())
            return "None";
        std::string text;
        for (std::size_t i = 0; i < parts.size(); ++i)
            text += (i ? ", " : "") + parts[i];
        return text;
    }

    std::string GetImportName(std::string_view folderName)
    {
        std::string name;
        for (const auto c : folderName)
            if (static_cast<unsigned char>(c) >= 0x20 && std::string_view("<>:\"/\\|?*").find(c) == std::string_view::npos)
                name += c;
        while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
            name.pop_back();
        while (!name.empty() && name.front() == ' ')
            name.erase(name.begin());
        if (name.size() > 40)
            name.resize(40);
        return name.empty() ? "Imported" : name;
    }

    std::string FormatGigabytes(std::uint64_t bytes)
    {
        const double gb = bytes / double(1ull << 30);
        if (gb < 0.095)
            return std::format("{:.0f} MB", bytes / double(1ull << 20));
        return std::format("{:.1f} GB", gb);
    }

    std::string GetStatusLine(const Status& status)
    {
        // (kept short)
        if (!status.mapHasPack)
            return std::format("No pack for {}. Build one in Iris Studio.", status.map);
        if (status.packs.empty())
            return std::format("Pack: {} off", status.map);
        std::string line = "Pack: " + JoinPacks(status.packs);
        if (!status.on)
            line += std::format(" \xC2\xB7 {}", FormatGigabytes(status.bytes));
        else if (status.waiting > 0)
            line += std::format(" \xC2\xB7 loading {} / {}", status.drawn, status.images);
        else
            line += std::format(" \xC2\xB7 {} / {}", FormatGigabytes(status.inUse), FormatGigabytes(status.budget));
        if (!status.groupsOff.empty())
            line += std::format(" \xC2\xB7 {} off", status.groupsOff);
        if (status.repair)
            line += std::format(" \xC2\xB7 {}", REPAIR_NOTE);
        return line;
    }

    std::size_t GetStatusLead(const Status& status)
    {
        return status.mapHasPack && status.on ? std::string("Pack: ").size() + JoinPacks(status.packs).size() : 0;
    }
}  // namespace IWXMVM::Components::TextureLogic
