#pragma once
#include <unordered_map>

namespace IWXMVM::Types
{
    // A character of the game an actor can be: its models by the slots the game's own scripts put them in (see
    // tools/t5-characters/catalog.py), which say more about what a model is than its name does. The slots are the
    // game's: a head slot may hold a blouse and a hat slot a skirt.
    struct Character
    {
        // what it is (characters.json's kind): a person, a dog, or a zombie (a person's skeleton too, see
        // CharacterLogic::GetActorKind); or one of the map's killstreaks (see CharacterLogic::MAP_KILLSTREAKS)
        enum class Kind
        {
            Human,
            Dog,
            Zombie,
            Killstreak,
        };

        std::string id;     // "khe_sanh/c_usa_jungmar_banres_khesanh"; "map/<body>" for the map's own characters
        std::string name;   // "Woods"
        std::string level;  // the zone it comes from ("khe_sanh"), empty for the map's own
        Kind kind = Kind::Human;
        std::string team;                   // allies, axis, ...
        std::string body, head, hat, gear;  // empty for none
        // what the parts can be instead, as the game picks them for crowds (the parts above among them)
        std::vector<std::string> bodies, heads, hats, gears;
        // bodies with parts shot off: upper bodies, and lower bodies attached to them
        std::vector<std::string> woundedTorsos, woundedLegs;
        std::string burnt;                // the id of its charred twin, if it has one
        std::string burntOf;              // for a charred twin, the id of the character it is of
        std::string zone;                 // the character zone its models are in (tools/t5-characters/zones.py)
        std::vector<std::string> alsoIn;  // other levels it is in
        std::unordered_map<std::string, std::uint32_t> bones;  // of its models, as far as known
        // made of a model only the level's own scripts use, which no character script names (the field agent of
        // Reznov's reveal, the Zombies crew; a shrunk zombie of Shangri-La)
        bool loose = false;

        // the models it is made of as the game makes it: the body first, then the head, hat and gear it has
        std::vector<std::string> GetModels() const
        {
            std::vector<std::string> models{body};
            for (const auto& model : {head, hat, gear})
                if (!model.empty())
                    models.push_back(model);
            return models;
        }
    };
}  // namespace IWXMVM::Types
