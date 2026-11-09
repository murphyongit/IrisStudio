#include "StdInclude.hpp"
#include "PlayerLookLogic.hpp"

#include "ActorLogic.hpp"

namespace IWXMVM::Components::PlayerLookLogic
{
    std::string_view GetFacePaintName(std::int32_t number)
    {
        return number >= 0 && static_cast<std::size_t>(number) < FACE_PAINTS.size() ? FACE_PAINTS[number]
                                                                                   : std::string_view{};
    }

    ResolvedCharacter Resolve(const Types::Loadout* player, const Types::Loadout* team, const Types::Loadout& all)
    {
        const auto pick = [&](const auto Types::Loadout::*field) {
            if (player && player->*field)
                return player->*field;
            return team && team->*field ? team->*field : all.*field;
        };
        return {pick(&Types::Loadout::body), pick(&Types::Loadout::head), pick(&Types::Loadout::worn),
                pick(&Types::Loadout::facePaint), pick(&Types::Loadout::stowed)};
    }

    std::string GetTeamName(std::int32_t team, const std::vector<std::string>& bodies)
    {
        // the factions as the game's bodies name them, the allies' first
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> ALLIES{
            {{"c_usa_cia", "CIA"}, {"c_usa_ciawin", "CIA"}, {"c_usa_sog", "SOG"}, {"c_cub_rebels", "Rebels"}}};
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> AXIS{
            {{"c_rus_spet", "Spetsnaz"}, {"c_rus_spetwin", "Spetsnaz"}, {"c_vtn_nva", "NVA"}, {"c_cub_tropas", "Tropas"}}};
        const auto find = [&](const auto& factions) {
            for (const auto& [prefix, name] : factions)
                if (std::ranges::any_of(bodies, [&](const std::string& body) {
                        return body.starts_with(std::string(prefix) + "_mp_body");
                    }))
                    return std::string(name);
            return std::string();
        };
        auto name = team == TEAM_ALLIES ? find(ALLIES) : team == TEAM_AXIS ? find(AXIS) : std::string();
        return !name.empty() ? name : std::format("Team {}", team);
    }

    namespace
    {
        nlohmann::json RuleToJson(const Types::WeaponRule& rule)
        {
            nlohmann::json node{{"from", rule.from}};
            if (rule.weapon)
                node["weapon"] = *rule.weapon;
            if (rule.attachments)
                node["attachments"] = *rule.attachments;
            if (rule.camo)
                node["camo"] = *rule.camo;
            return node;
        }

        Types::WeaponRule RuleFromJson(const nlohmann::json& node)
        {
            Types::WeaponRule rule{node.value("from", "")};
            if (node.contains("weapon"))
                rule.weapon = node["weapon"].get<std::string>();
            if (node.contains("attachments"))
                rule.attachments = node["attachments"].get<std::vector<std::string>>();
            if (node.contains("camo"))
                rule.camo = node["camo"].get<std::int32_t>();
            return rule;
        }

        nlohmann::json LoadoutToJson(const Types::Loadout& loadout)
        {
            nlohmann::json node = nlohmann::json::object();
            if (loadout.body)
                node["body"] = *loadout.body;
            if (loadout.head)
                node["head"] = *loadout.head;
            if (loadout.worn)
                node["worn"] = *loadout.worn;
            if (loadout.character)
                node["character"] = *loadout.character;
            if (loadout.facePaint)
                node["facePaint"] = *loadout.facePaint;
            if (loadout.stowed)
                node["stowed"] = *loadout.stowed;
            auto rules = nlohmann::json::array();
            for (const auto& rule : loadout.weaponRules)
                rules.push_back(RuleToJson(rule));
            node["weaponRules"] = std::move(rules);
            return node;
        }

        Types::Loadout LoadoutFromJson(const nlohmann::json& node)
        {
            Types::Loadout loadout;
            if (node.contains("body"))
                loadout.body = node["body"].get<std::string>();
            if (node.contains("head"))
                loadout.head = node["head"].get<std::string>();
            if (node.contains("worn"))
                loadout.worn = node["worn"].get<std::vector<std::string>>();
            if (node.contains("character"))
                loadout.character = node["character"].get<std::string>();
            if (node.contains("facePaint"))
                loadout.facePaint = node["facePaint"].get<std::int32_t>();
            if (node.contains("stowed"))
                loadout.stowed = node["stowed"].get<std::string>();
            for (const auto& rule : node.value("weaponRules", nlohmann::json::array()))
                loadout.weaponRules.push_back(RuleFromJson(rule));
            return loadout;
        }

        // (by number as text, as json's keys are)
        template <typename T, typename Convert>
        nlohmann::json MapToJson(const std::map<std::int32_t, T>& values, Convert convert)
        {
            auto node = nlohmann::json::object();
            for (const auto& [key, value] : values)
                node[std::to_string(key)] = convert(value);
            return node;
        }

        template <typename T, typename Convert>
        std::map<std::int32_t, T> MapFromJson(const nlohmann::json& node, const char* name, Convert convert)
        {
            std::map<std::int32_t, T> values;
            const auto entries = node.value(name, nlohmann::json::object());
            for (const auto& [key, value] : entries.items())
                values[std::stoi(key)] = convert(value);
            return values;
        }
    }  // namespace

    nlohmann::json ChoicesToJson(const Types::PlayerChoices& choices)
    {
        const auto same = [](const std::string& death) { return death; };
        return {{"all", LoadoutToJson(choices.loadoutForAll)},
                {"teams", MapToJson(choices.teamLoadouts, LoadoutToJson)},
                {"players", MapToJson(choices.playerLoadouts, LoadoutToJson)},
                {"deathForAll", choices.deathForAll},
                {"teamDeaths", MapToJson(choices.teamDeaths, same)},
                {"playerDeaths", MapToJson(choices.playerDeaths, same)}};
    }

    Types::PlayerChoices ChoicesFromJson(const nlohmann::json& node)
    {
        Types::PlayerChoices choices;
        if (!node.is_object())
            return choices;
        const auto death = [](const nlohmann::json& value) { return value.get<std::string>(); };
        if (node.contains("all"))
            choices.loadoutForAll = LoadoutFromJson(node["all"]);
        choices.teamLoadouts = MapFromJson<Types::Loadout>(node, "teams", LoadoutFromJson);
        choices.playerLoadouts = MapFromJson<Types::Loadout>(node, "players", LoadoutFromJson);
        choices.deathForAll = node.value("deathForAll", "");
        choices.teamDeaths = MapFromJson<std::string>(node, "teamDeaths", death);
        choices.playerDeaths = MapFromJson<std::string>(node, "playerDeaths", death);
        return choices;
    }

    void SetCharacter(Types::Loadout& loadout, const Types::Character& character)
    {
        loadout.character = character.id;
        loadout.body = character.body;
        loadout.head = character.head;
        loadout.worn.emplace();
        for (const auto& model : {character.hat, character.gear})
            if (!model.empty())
                loadout.worn->push_back(model);
    }

    void SetModels(Types::Loadout& loadout, const std::vector<std::string>& models, std::string_view character,
                   const std::function<bool(const std::string&)>& isHead)
    {
        if (models.empty())
            return;
        loadout.character = std::string(character);
        loadout.body = models.front();
        loadout.head = std::string();
        loadout.worn.emplace();
        for (std::size_t i = 1; i < models.size(); ++i)
        {
            if (loadout.head->empty() && isHead(models[i]))
                loadout.head = models[i];
            else
                loadout.worn->push_back(models[i]);
        }
    }

    std::vector<std::string> GetModels(const Types::Loadout& loadout)
    {
        std::vector<std::string> models;
        for (const auto& model : {loadout.body, loadout.head})
            if (model && !model->empty())
                models.push_back(*model);
        if (loadout.worn)
            models.insert(models.end(), loadout.worn->begin(), loadout.worn->end());
        return models;
    }

    Models GetWanted(const Models& original, const ResolvedCharacter& chosen, std::size_t mostModels,
                     const std::optional<std::string>& stowedModel)
    {
        const auto stowed = std::ranges::find_if(original.begin() + 1, original.end(),
                                                 [](const Attached& model) { return !model.tag.empty(); });
        const bool hasHead = original.begin() + 1 != stowed;

        Models wanted{{chosen.body.value_or(original.front().name), {}}};
        const auto head = chosen.head.value_or(hasHead ? original[1].name : std::string());
        if (!head.empty())
            wanted.push_back({head, {}});
        if (chosen.worn)
            for (const auto& model : *chosen.worn)
                wanted.push_back({model, {}});
        else if (hasHead)
            wanted.insert(wanted.end(), original.begin() + 2, stowed);
        for (auto model = stowed; model != original.end(); ++model)
            if (!stowedModel || model->tag != STOWED_TAG)
                wanted.push_back(*model);
        if (stowedModel && !stowedModel->empty())
            wanted.push_back({*stowedModel, std::string(STOWED_TAG)});
        wanted.resize(std::min(wanted.size(), mostModels));
        return wanted;
    }

    Models Fit(const Models& models, std::span<const std::uint32_t> bones, std::uint32_t addedBones,
               std::uint32_t maxBones)
    {
        // (the head and what is added go first after the body)
        std::vector<std::uint32_t> order(bones.begin(), bones.end());
        const auto added = std::min<std::size_t>(2, order.size());
        order.insert(order.begin() + added, addedBones);
        const auto kept = ActorLogic::FitBones(order, maxBones);

        Models fitted;
        for (std::size_t i = 0; i < models.size(); ++i)
            if (kept[i < added ? i : i + 1])
                fitted.push_back(models[i]);
        return fitted;
    }
}  // namespace IWXMVM::Components::PlayerLookLogic
