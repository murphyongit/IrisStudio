#include "StdInclude.hpp"
#include "MapMenuLogic.hpp"

namespace IWXMVM::Components::MapMenuLogic
{
    namespace
    {
        using CampaignMapLogic::Level;
        using CampaignMapLogic::Teamset;

        // packs the game's tabs never list
        constexpr std::int32_t CAMPAIGN_PACK = 6;
        constexpr std::int32_t ZOMBIES_PACK = 7;

        std::string Upper(std::string_view text)
        {
            std::string upper(text);
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return upper;
        }

        // the team names of the game's rows of each teamset's maps (mp/mapsTable.csv)
        std::array<std::string_view, 4> GetTeamNames(Teamset teamset)
        {
            switch (teamset)
            {
                case Teamset::JungleMarines:
                    return {"MPUI_MARINE_SHORT", "MPUI_NVA_SHORT", "sog", "nva"};
                case Teamset::Cubans:
                    return {"MPUI_REBELS_SHORT", "MPUI_TROPAS_SHORT", "op40", "tropas"};
                default:
                    return {"MPUI_SPECOPS_SHORT", "MPUI_RUSSIAN_SHORT", "ops", "spets"};
            }
        }

        constexpr std::string_view KEY_PREFIX = "IRIS_MAP_";
        constexpr std::string_view CAPS = "_CAPS";
    }  // namespace

    std::string_view GetTabLabel(Tab tab)
    {
        switch (tab)
        {
            case Tab::Campaign:
                return "CAMPAIGN";
            case Tab::Zombies:
                return "ZOMBIES";
            default:
                return "";
        }
    }

    std::string GetTabLabelKey(Tab tab)
    {
        return "@IRIS_TAB_" + std::string(GetTabLabel(tab));
    }

    Tab GetTab(const Level& level)
    {
        return level.mode == CampaignMapLogic::Mode::Zombies ? Tab::Zombies : Tab::Campaign;
    }

    std::vector<const Level*> ListTab(const CampaignMapLogic::Catalog& catalog, Tab tab,
                                      const std::function<bool(const Level&)>& has)
    {
        std::vector<const Level*> rows;
        for (const auto& level : catalog.levels)
            if (level.menu && GetTab(level) == tab && (!has || has(level)))
                rows.push_back(&level);
        std::stable_sort(rows.begin(), rows.end(),
                         [](const Level* a, const Level* b) { return a->menu->order < b->menu->order; });
        return rows;
    }

    std::int32_t GetPack(Tab tab)
    {
        return tab == Tab::Zombies ? ZOMBIES_PACK : tab == Tab::Campaign ? CAMPAIGN_PACK : 0;
    }

    std::optional<Tab> FindTabOfPack(std::int32_t pack)
    {
        if (pack == CAMPAIGN_PACK)
            return Tab::Campaign;
        if (pack == ZOMBIES_PACK)
            return Tab::Zombies;
        return std::nullopt;
    }

    std::vector<std::string> MakeTableRow(const Level& level, std::uint32_t index)
    {
        const auto teamset = std::string(CampaignMapLogic::GetTeamsetName(level.teamset));
        const auto teams = GetTeamNames(level.teamset);
        return {level.name,
                teamset,
                teamset,
                GetNameKey(level.name),
                GetPictureMaterial(level.name, false),
                std::to_string(index),
                level.menu ? level.menu->description : std::string(),
                "",
                "",
                "NO",
                "YES",
                std::to_string(GetPack(GetTab(level))),
                std::string(teams[0]),
                std::string(teams[1]),
                std::string(teams[2]),
                std::string(teams[3])};
    }

    std::string GetNameKey(std::string_view level)
    {
        return std::string(KEY_PREFIX) + Upper(level);
    }

    std::optional<std::string> ResolveKey(const CampaignMapLogic::Catalog& catalog, std::string_view key,
                                          const std::function<std::optional<std::string>(const std::string&)>& game)
    {
        if (const auto text = catalog.strings.find(key); text != catalog.strings.end())
            return text->second;
        for (const auto tab : {Tab::Campaign, Tab::Zombies})
            if (GetTabLabelKey(tab).substr(1) == key)
                return std::string(GetTabLabel(tab));
        if (!key.starts_with(KEY_PREFIX))
            return std::nullopt;
        const bool caps = key.ends_with(CAPS);
        const auto name = key.substr(KEY_PREFIX.size(), key.size() - KEY_PREFIX.size() - (caps ? CAPS.size() : 0));
        for (const auto& level : catalog.levels)
        {
            if (Upper(level.name) != name || !level.menu)
                continue;
            if (!caps)
                return level.title;
            if (const auto text = catalog.strings.find(level.menu->name); text != catalog.strings.end())
                return text->second;
            if (game)
                if (auto text = game(level.menu->name))
                    return text;
            return Upper(level.title);
        }
        return std::nullopt;
    }

    std::string GetPictureMaterial(std::string_view level, bool big)
    {
        return "menu_" + std::string(level) + (big ? "_map_select_big" : "_map_select_final");
    }

    std::optional<std::string> FindLevelOfPictureMaterial(const CampaignMapLogic::Catalog& catalog,
                                                          std::string_view material)
    {
        constexpr std::string_view prefix = "menu_", finalSuffix = "_map_select_final", bigSuffix = "_map_select_big";
        if (!material.starts_with(prefix))
            return std::nullopt;
        material.remove_prefix(prefix.size());
        if (material.ends_with(finalSuffix))
            material.remove_suffix(finalSuffix.size());
        else if (material.ends_with(bigSuffix))
            material.remove_suffix(bigSuffix.size());
        else
            return std::nullopt;
        const auto level = catalog.FindLevel(material);
        if (!level || !level->menu)
            return std::nullopt;
        return level->name;
    }

    std::string_view GetPictureImage(std::string_view picture)
    {
        const auto slash = picture.find('/');
        return slash == std::string_view::npos ? picture : picture.substr(slash + 1);
    }

    std::int32_t HashCell(std::string_view text)
    {
        std::uint32_t hash = 5381;
        for (const char c : text)
            hash = hash * 33 + static_cast<std::uint32_t>(std::tolower(static_cast<unsigned char>(c)));
        return static_cast<std::int32_t>(hash);
    }

    std::string ToGameText(std::string_view utf8)
    {
        std::string text;
        for (std::size_t i = 0; i < utf8.size();)
        {
            const auto c = static_cast<unsigned char>(utf8[i]);
            std::uint32_t code = c;
            std::size_t length = 1;
            if (c >= 0xF0)
                length = 4, code = c & 0x07;
            else if (c >= 0xE0)
                length = 3, code = c & 0x0F;
            else if (c >= 0xC0)
                length = 2, code = c & 0x1F;
            for (std::size_t k = 1; k < length && i + k < utf8.size(); ++k)
                code = (code << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
            text.push_back(code < 0x100 ? static_cast<char>(code) : '?');
            i += length;
        }
        return text;
    }
}  // namespace IWXMVM::Components::MapMenuLogic
