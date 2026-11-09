#include "StdInclude.hpp"
#include "CharacterLogic.hpp"

#include "KillstreakLogic.hpp"

#include <regex>

namespace IWXMVM::Components::CharacterLogic
{
    namespace
    {
        std::string StringOrEmpty(const nlohmann::json& node, const char* key)
        {
            const auto it = node.find(key);
            return it != node.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        std::vector<std::string> Strings(const nlohmann::json& node, const char* key)
        {
            std::vector<std::string> strings;
            if (const auto it = node.find(key); it != node.end() && it->is_array())
                for (const auto& entry : *it)
                    if (entry.is_string())
                        strings.push_back(entry.get<std::string>());
            return strings;
        }

        // a slot's choices: the game's array for it if it picks from one, else the slot's model alone
        std::vector<std::string> Choices(const nlohmann::json& variants, const char* slot, const std::string& model)
        {
            auto choices = Strings(variants, slot);
            if (choices.empty() && !model.empty())
                choices.push_back(model);
            return choices;
        }

        constexpr std::array<std::string_view, 3> DOG_MODELS{"german_shepherd", "german_sheperd_dog", "zombie_wolf"};

        Character::Kind ParseKind(std::string_view kind)
        {
            if (kind == "dog")
                return Character::Kind::Dog;
            if (kind == "zombie")
                return Character::Kind::Zombie;
            return Character::Kind::Human;
        }

        bool Contains(const std::vector<std::string>& list, std::string_view model)
        {
            return std::ranges::find(list, model) != list.end();
        }

        // c_<country>_<faction>_mp_body_<kind> and c_<country>_<faction>_mp_head_<n>, by faction
        const std::regex MP_BODY(R"(^(c_[a-z0-9]+_[a-z0-9]+_mp)_body_(\w+)$)");
        const std::regex MP_HEAD(R"(^(c_[a-z0-9]+_[a-z0-9]+_mp)_head_?(\w+)$)");
    }  // namespace

    std::string GetOwnHead(std::string_view body)
    {
        // mpbody/<class>_mp.gsc's get_default_head() and mphead/head_<class>_mp.gsc
        static const std::unordered_map<std::string_view, std::string_view> HEADS{
            {"armor", "1"}, {"camo", "2"}, {"flak", "3"}, {"standard", "4"}, {"utility", "5"}};
        const std::string name(body);
        std::smatch match;
        if (!std::regex_match(name, match, MP_BODY))
            return {};
        const auto head = HEADS.find(match[2].str());
        return head != HEADS.end() ? std::format("{}_head_{}", match[1].str(), head->second) : std::string();
    }

    bool GiveOwnHead(std::vector<std::string>& models, std::string_view character)
    {
        if (models.size() != 2 || character != "map/" + models.front())
            return false;
        const auto own = GetOwnHead(models.front());
        if (own.empty())
            return false;
        const auto first = own.substr(0, own.rfind('_')) + "_1";
        if (models[1] != first || own == first)
            return false;
        models[1] = own;
        return true;
    }

    CampaignCatalog ParseCatalog(const nlohmann::json& root)
    {
        if (root.value("version", 0) != 2)
            throw std::runtime_error("characters.json isn't of version 2");
        CampaignCatalog catalog;
        for (const auto& level : root.at("levels"))
            catalog.levels.push_back(
                {level.at("zone").get<std::string>(), level.value("name", ""), level.value("group", "")});
        for (const auto& node : root.at("characters"))
        {
            Character character;
            character.id = node.at("id").get<std::string>();
            character.name = node.value("name", character.id);
            character.level = node.value("level", "");
            character.kind = ParseKind(node.value("kind", "human"));
            character.team = node.value("team", "");
            character.body = node.at("body").get<std::string>();
            character.head = StringOrEmpty(node, "head");
            character.hat = StringOrEmpty(node, "hat");
            character.gear = StringOrEmpty(node, "gear");
            const auto variants = node.value("variants", nlohmann::json::object());
            character.bodies = Choices(variants, "body", character.body);
            character.heads = Choices(variants, "head", character.head);
            character.hats = Choices(variants, "hat", character.hat);
            character.gears = Choices(variants, "gear", character.gear);
            const auto damage = node.value("damage", nlohmann::json::object());
            character.woundedTorsos = Strings(damage, "torso");
            character.woundedLegs = Strings(damage, "legs");
            character.burnt = StringOrEmpty(node, "burnt");
            character.burntOf = StringOrEmpty(node, "burntOf");
            character.zone = node.value("zone", "");
            character.alsoIn = Strings(node, "alsoIn");
            character.loose = node.value("loose", false);
            // (kept: the items of a temporary would outlive it in the loop)
            const auto bones = node.value("bones", nlohmann::json::object());
            for (const auto& [model, count] : bones.items())
                if (count.is_number_unsigned())
                    character.bones[model] = count.get<std::uint32_t>();
            catalog.characters.push_back(std::move(character));
        }
        return catalog;
    }

    std::unordered_map<std::string, ModelKind> GetModelKinds(const std::vector<Character>& characters)
    {
        std::unordered_map<std::string, ModelKind> kinds;
        // bodies first, then heads, then what is worn: a model keeps the first of those it is used as
        for (const auto& character : characters)
        {
            const auto bodyKind = character.head.empty() ? ModelKind::FullBody : ModelKind::Body;
            for (const auto& body : character.bodies)
                kinds.try_emplace(body, bodyKind);
            for (const auto& torso : character.woundedTorsos)
                kinds.try_emplace(torso, ModelKind::Body);
        }
        for (const auto& character : characters)
            for (const auto& head : character.heads)
                kinds.try_emplace(head, ModelKind::Head);
        for (const auto& character : characters)
        {
            for (const auto* worn : {&character.hats, &character.gears, &character.woundedLegs})
                for (const auto& model : *worn)
                    kinds.try_emplace(model, ModelKind::Gear);
        }
        return kinds;
    }

    std::vector<Character> MakeMapCharacters(const std::vector<std::string>& models)
    {
        static const std::unordered_map<std::string_view, std::string_view> FACTIONS{
            {"cia", "CIA"},           {"spet", "Spetsnaz"},  {"nva", "NVA"},   {"vc", "Viet Cong"},
            {"tropas", "Tropas"},     {"opforce", "OP40"},   {"sog", "SOG"},   {"blackops", "Black Ops"},
            {"spetsnaz", "Spetsnaz"}, {"marine", "Marines"}, {"op40", "OP40"}, {"ussr", "Soviets"}};

        std::map<std::string, std::vector<std::string>> bodies, heads;
        for (const auto& model : models)
        {
            std::smatch match;
            if (std::regex_match(model, match, MP_BODY))
                bodies[match[1].str()].push_back(model);
            else if (std::regex_match(model, match, MP_HEAD))
                heads[match[1].str()].push_back(model);
        }

        std::vector<Character> characters;
        for (auto& [faction, factionBodies] : bodies)
        {
            std::ranges::sort(factionBodies);
            auto& factionHeads = heads[faction];
            std::ranges::sort(factionHeads);
            // c_usa_cia_mp -> cia
            const auto first = faction.find('_', 2);
            const auto last = faction.rfind("_mp");
            const auto key = faction.substr(first + 1, last - first - 1);
            const auto known = FACTIONS.find(key);
            auto factionName = known != FACTIONS.end() ? std::string(known->second) : key;
            if (known == FACTIONS.end() && !factionName.empty())
                factionName[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(factionName[0])));
            for (const auto& body : factionBodies)
            {
                Character character;
                character.id = "map/" + body;
                auto kind = body.substr(body.rfind("_body_") + 6);
                std::ranges::replace(kind, '_', ' ');
                character.name = std::format("{} ({})", factionName, kind);
                character.body = body;
                character.bodies = {body};
                // the head made for its cap or hood (the faction's first is a bare head's)
                const auto own = GetOwnHead(body);
                character.head = Contains(factionHeads, own) ? own
                                 : factionHeads.empty()      ? std::string()
                                                             : factionHeads.front();
                character.heads = factionHeads;
                characters.push_back(std::move(character));
            }
        }
        // its dogs (multiplayer's killstreak), which have no head
        for (const auto dog : MAP_DOGS)
        {
            if (std::ranges::find(models, dog) == models.end())
                continue;
            Character character;
            character.id = "map/" + std::string(dog);
            character.name = "German shepherd";
            character.kind = Character::Kind::Dog;
            character.body = dog;
            character.bodies = {character.body};
            characters.push_back(std::move(character));
        }
        // its killstreaks, each of their looks a body (the friendly team's first)
        for (const auto& killstreak : KillstreakLogic::GetAll())
        {
            if (std::ranges::find(models, killstreak.models.front()) == models.end())
                continue;
            Character character;
            character.id = "map/" + std::string(killstreak.models.front());
            character.name = killstreak.name;
            character.kind = Character::Kind::Killstreak;
            character.body = killstreak.models.front();
            for (const auto model : killstreak.models)
                if (std::ranges::find(models, model) != models.end())
                    character.bodies.emplace_back(model);
            characters.push_back(std::move(character));
        }
        return characters;
    }

    Types::ActorKind GetActorKind(Character::Kind kind)
    {
        switch (kind)
        {
            case Character::Kind::Dog:
                return Types::ActorKind::Dog;
            case Character::Kind::Killstreak:
                return Types::ActorKind::Vehicle;
            default:
                return Types::ActorKind::Human;
        }
    }

    std::vector<std::string> GetModels(const Character& character, const Look& look, const Character* burnt)
    {
        if (character.kind == Character::Kind::Killstreak && !character.bodies.empty())
            return {character.bodies[std::min(look.body, character.bodies.size() - 1)]};
        if (look.variant == Variant::Burnt && burnt)
            return burnt->GetModels();
        if (look.variant != Variant::Wounded || character.woundedTorsos.empty())
            return character.GetModels();

        std::vector<std::string> models{
            character.woundedTorsos[std::min(look.torso, character.woundedTorsos.size() - 1)]};
        if (!character.woundedLegs.empty())
            models.push_back(character.woundedLegs[std::min(look.legs, character.woundedLegs.size() - 1)]);
        for (const auto& model : {character.head, character.hat, character.gear})
            if (!model.empty())
                models.push_back(model);
        return models;
    }

    Look GetLook(const Character& character, const std::vector<std::string>& models, const Character* burnt)
    {
        Look look;
        if (models.empty())
            return look;
        if (character.kind == Character::Kind::Killstreak)
        {
            const auto body = std::ranges::find(character.bodies, models.front());
            look.body = body != character.bodies.end() ? static_cast<std::size_t>(body - character.bodies.begin()) : 0;
            return look;
        }
        if (burnt && models.front() == burnt->body)
        {
            look.variant = Variant::Burnt;
            return look;
        }
        if (const auto torso = std::ranges::find(character.woundedTorsos, models.front());
            torso != character.woundedTorsos.end())
        {
            look.variant = Variant::Wounded;
            look.torso = static_cast<std::size_t>(torso - character.woundedTorsos.begin());
            for (std::size_t i = 1; i < models.size(); ++i)
                if (const auto legs = std::ranges::find(character.woundedLegs, models[i]);
                    legs != character.woundedLegs.end())
                    look.legs = static_cast<std::size_t>(legs - character.woundedLegs.begin());
        }
        return look;
    }

    const Character* Find(const std::vector<Character>& characters, std::string_view id)
    {
        const auto it = std::ranges::find(characters, id, &Character::id);
        return it != characters.end() ? &*it : nullptr;
    }

    const Character* FindByModels(const std::vector<Character>& characters, const std::vector<std::string>& models)
    {
        if (models.empty())
            return nullptr;
        const auto& body = models.front();
        const Character* best = nullptr;
        std::size_t bestScore = 0;
        for (const auto& character : characters)
        {
            const bool own = character.body == body;
            if (!own && !Contains(character.bodies, body) && !Contains(character.woundedTorsos, body))
                continue;
            // the body's own character first, then how many of the models it has
            std::size_t score = own ? 1000 : 0;
            for (const auto& model : models)
                if (Contains(character.bodies, model) || Contains(character.heads, model) ||
                    Contains(character.hats, model) || Contains(character.gears, model) ||
                    Contains(character.woundedTorsos, model) || Contains(character.woundedLegs, model))
                    ++score;
            if (!best || score > bestScore)
            {
                best = &character;
                bestScore = score;
            }
        }
        return best;
    }

    std::vector<std::string> GetZoneModels(const Character& character)
    {
        std::vector<std::string> models;
        const auto add = [&](const std::string& model) {
            if (!model.empty() && !Contains(models, model))
                models.push_back(model);
        };
        for (const auto& model : {character.body, character.head, character.hat, character.gear})
            add(model);
        for (const auto* list : {&character.bodies, &character.heads, &character.hats, &character.gears,
                                 &character.woundedTorsos, &character.woundedLegs})
            for (const auto& model : *list)
                add(model);
        return models;
    }

    ZoneIndex IndexZones(const std::vector<Character>& characters)
    {
        ZoneIndex index;
        for (const auto& character : characters)
        {
            if (character.zone.empty())
                continue;
            for (const auto& model : GetZoneModels(character))
                index[model].push_back(character.zone);
        }
        return index;
    }

    std::vector<std::string> GetZonesFor(const std::vector<std::string>& models, const Character* character,
                                         const Character* burnt, const ZoneIndex& index)
    {
        std::vector<std::string> zones;
        for (const auto& model : models)
        {
            const auto found = index.find(model);
            if (found == index.end())
                continue;
            const auto& candidates = found->second;
            auto zone = candidates.front();
            for (const auto* preferred : {burnt, character})
                if (preferred && Contains(candidates, preferred->zone))
                    zone = preferred->zone;
            if (!Contains(zones, zone))
                zones.push_back(zone);
        }
        return zones;
    }

    std::string Describe(const Character& character, const std::vector<Types::ActorCatalog::Pack>& levels)
    {
        const auto level = std::ranges::find(levels, character.level, &Types::ActorCatalog::Pack::name);
        return level != levels.end() ? std::format("{} ({})", character.name, level->displayName) : character.name;
    }

    bool IsDog(std::span<const std::string> models)
    {
        return !models.empty() && std::ranges::find(DOG_MODELS, models.front()) != DOG_MODELS.end();
    }
}  // namespace IWXMVM::Components::CharacterLogic
