#pragma once
#include "Types/Actor.hpp"
#include "Types/Character.hpp"

#include <unordered_map>

#include "nlohmann/json.hpp"

namespace IWXMVM::Components::CharacterLogic
{
    // What actors can be made of, by character (see Types::Character): reading the catalog of the campaign's and
    // Zombies' characters that comes with the mod, making characters of the map's own models, telling what a model is
    // by how characters use it, and the looks of a character. No game state; tested in tests/actors-tests.

    using Character = Types::Character;
    using ModelKind = Types::ActorCatalog::ModelKind;

    // The characters of the campaign levels and Zombies maps (resources/characters.json, written by
    // tools/t5-characters/catalog.py) and their levels
    struct CampaignCatalog
    {
        std::vector<Types::ActorCatalog::Pack> levels;  // in campaign order, then the Zombies maps
        std::vector<Character> characters;              // by level, in the order of the levels
    };
    // throws on a catalog it can't read
    CampaignCatalog ParseCatalog(const nlohmann::json& root);

    // What characters make of the models they use: the body slot a body (a full body, with its face, when its
    // character has no head), the head slot a head, and the hat and gear slots and the lower halves of wounded bodies
    // something worn on the body's bones. A model used in several slots takes the first of body, head, worn.
    std::unordered_map<std::string, ModelKind> GetModelKinds(const std::vector<Character>& characters);

    // multiplayer's dogs, which every map has loaded (the dogs killstreak's)
    inline constexpr std::array<std::string_view, 1> MAP_DOGS{"german_shepherd"};
    // whether an actor of these models is a dog: the map's, the campaign's or a Zombies hellhound. Where the game can be
    // asked, Actors::GetActorKind tells it from the bones; this one needs nothing loaded
    bool IsDog(std::span<const std::string> models);
    // The head the game gives a multiplayer body, made to sit under its cap or hood (the maps' mpbody and mphead
    // scripts, the same on every map and for every faction): armor head_1, camo head_2, flak head_3, standard head_4,
    // utility head_5 (c_usa_cia_mp_body_standard -> c_usa_cia_mp_head_4); empty for any other model
    std::string GetOwnHead(std::string_view body);
    // The map's own characters, from the multiplayer models it has (c_usa_cia_mp_body_armor, c_usa_cia_mp_head_1...):
    // one per body of a faction, with the body's own head (GetOwnHead, else the faction's first) and the faction's
    // other heads to choose from; then its dogs (MAP_DOGS), of kind Dog; then its killstreaks (KillstreakLogic: every
    // map has them all), of kind Killstreak, their looks (the friendly team's, the enemy's) as the bodies to choose from
    std::vector<Character> MakeMapCharacters(const std::vector<std::string>& models);
    // An actor saved while every map character wore its faction's head_1: one of a map character (character map/<body>)
    // made of exactly that body and head_1, on a body whose own head is another, gets its own head (the head came from
    // that, not from a choice; one with any other head keeps it). Returns whether it changed the models.
    bool GiveOwnHead(std::vector<std::string>& models, std::string_view character);
    // what an actor of a character's kind is: a dog's a dog, a killstreak's a vehicle, a person's and a zombie's a
    // person
    Types::ActorKind GetActorKind(Character::Kind kind);

    // How an actor looks as a character: as the game makes it, as its charred twin, or with parts shot off
    enum class Variant
    {
        Normal,
        Burnt,
        Wounded,
    };
    struct Look
    {
        Variant variant = Variant::Normal;
        std::size_t torso = 0;  // Wounded: which of the character's wounded upper bodies
        std::size_t legs = 0;   // and lower bodies
        // a killstreak's: which of its bodies (the friendly team's colours, the enemy's: see KillstreakLogic::GetLook)
        std::size_t body = 0;
    };
    // The models of a look of the character, the body first: its charred twin's (given as burnt) for Burnt, and for
    // Wounded a wounded upper body for the body with a lower body, then its head, hat and gear. A look the character
    // doesn't have gives its own models.
    std::vector<std::string> GetModels(const Character& character, const Look& look, const Character* burnt);
    // the look of the character an actor's models are (Normal when they are none of its others)
    Look GetLook(const Character& character, const std::vector<std::string>& models, const Character* burnt);

    const Character* Find(const std::vector<Character>& characters, std::string_view id);
    // The character an actor's models make up, for actors saved before they had one: of those whose models the
    // actor's body is among (as its body, a variant of it or a wounded upper body), the one with the most of the
    // actor's models among its own, the body's own character first; none when no character has the body
    const Character* FindByModels(const std::vector<Character>& characters, const std::vector<std::string>& models);

    // the name a character is listed with, with its level's when it has one: "Woods (S.O.G.)"
    std::string Describe(const Character& character, const std::vector<Types::ActorCatalog::Pack>& levels);

    // Every model of a character's zone (tools/t5-characters/zones.py puts them all in it): its parts, the arrays they
    // are picked from and its wounded bodies
    std::vector<std::string> GetZoneModels(const Character& character);
    // the character zones that have each model, in the order of the characters
    using ZoneIndex = std::unordered_map<std::string, std::vector<std::string>>;
    ZoneIndex IndexZones(const std::vector<Character>& characters);
    // The character zones an actor's models are in: for each model the zone of the actor's character if it has it,
    // else its charred twin's, else the first zone that has it; a model no zone has needs none (the map's own)
    std::vector<std::string> GetZonesFor(const std::vector<std::string>& models, const Character* character,
                                         const Character* burnt, const ZoneIndex& index);
}  // namespace IWXMVM::Components::CharacterLogic
