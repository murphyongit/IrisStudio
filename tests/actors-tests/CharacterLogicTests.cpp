#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/CharacterLogic.hpp"

#include <set>

using namespace IWXMVM;
using Components::CharacterLogic::Look;
using Components::CharacterLogic::Variant;
using ModelKind = Types::ActorCatalog::ModelKind;

namespace
{
    // the catalog that comes with the mod (tools/t5-characters/catalog.py over the game's zones)
    const std::filesystem::path SHIPPED_CATALOG =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" / "resources" /
        "characters.json";

    const Components::CharacterLogic::CampaignCatalog& Shipped()
    {
        static const auto catalog = [] {
            try
            {
                std::ifstream file(SHIPPED_CATALOG);
                return Components::CharacterLogic::ParseCatalog(nlohmann::json::parse(file));
            }
            catch (const std::exception& e)
            {
                Test::Fail(__FILE__, __LINE__, std::string("the catalog doesn't read: ") + e.what());
                return Components::CharacterLogic::CampaignCatalog{};
            }
        }();
        return catalog;
    }

    const Types::Character& Get(std::string_view id)
    {
        const auto character = Components::CharacterLogic::Find(Shipped().characters, id);
        static const Types::Character none;
        CHECK(character != nullptr);
        return character ? *character : none;
    }

    // the name heuristic the Actors tab sorted models with before characters (t5/src/Actors.cpp Classify)
    ModelKind ClassifyByName(std::string_view name)
    {
        constexpr std::array GEAR{"headwear", "hat",    "helmet", "ushanka", "pilotka", "beret", "cap",  "scarf",
                                  "gasmask",  "goggle", "hood",   "bandana", "glasses", "gear",  "mask", "beanie"};
        if (std::ranges::any_of(GEAR, [&](const char* part) { return name.find(part) != std::string_view::npos; }))
            return ModelKind::Gear;
        if (name.find("head") != std::string_view::npos)
            return ModelKind::Head;
        if (name.ends_with("_fb") || name.find("_fb_") != std::string_view::npos ||
            name.find("fullbody") != std::string_view::npos)
            return ModelKind::FullBody;
        return ModelKind::Body;
    }
}  // namespace

TEST("the shipped character catalog reads, with every level's characters and a zone name each")
{
    const auto& catalog = Shipped();
    CHECK(catalog.levels.size() >= 25);
    CHECK(catalog.characters.size() >= 250);
    std::set<std::string> ids;
    for (const auto& character : catalog.characters)
    {
        CHECK(ids.insert(character.id).second);
        CHECK(!character.body.empty());
        CHECK(character.zone.starts_with("t5c_") && character.zone.size() <= 60);
        CHECK(std::ranges::find(catalog.levels, character.level, &Types::ActorCatalog::Pack::name) !=
              catalog.levels.end());
        CHECK(std::ranges::find(character.bodies, character.body) != character.bodies.end());
    }
    CHECK(catalog.levels.front().name == "frontend");
    CHECK(catalog.levels.front().displayName == "Interrogation");
}

TEST("Barnes of S.O.G. is Woods: his full body and the bandana on its own bone")
{
    const auto& woods = Get("khe_sanh/c_usa_jungmar_banres_khesanh");
    CHECK(woods.name == "Woods");
    CHECK(woods.body == "c_usa_jungmar_barnes_ks_fb");
    CHECK(woods.head.empty());
    CHECK(woods.hat == "c_usa_jungmar_barnes_ks_bdana");
    CHECK(woods.zone == "t5c_khe_sanh_usa_jungmar_banres_khesanh");
    CHECK(woods.bones.at("c_usa_jungmar_barnes_ks_fb") == 120);
    CHECK(
        (woods.GetModels() == std::vector<std::string>{"c_usa_jungmar_barnes_ks_fb", "c_usa_jungmar_barnes_ks_bdana"}));
    CHECK(Components::CharacterLogic::Describe(woods, Shipped().levels) == "Woods (S.O.G.)");
}

TEST("the characters' slots file the models the name heuristic misfiled (B4)")
{
    const auto kinds = Components::CharacterLogic::GetModelKinds(Shipped().characters);
    auto kind = [&](const char* model) {
        const auto it = kinds.find(model);
        CHECK(it != kinds.end());
        return it != kinds.end() ? it->second : ModelKind::Body;
    };
    // the wounded marines' bandages are what they wear, not bodies: the "body that is just a bandage"
    CHECK(kind("c_usa_jungmar_bandage_knee") == ModelKind::Gear);
    CHECK(kind("c_usa_jungmar_bandage_torso") == ModelKind::Gear);
    CHECK(ClassifyByName("c_usa_jungmar_bandage_knee") == ModelKind::Body);
    // Project Nova's bodies aren't heads although "fullahead" has "head" in it
    CHECK(kind("c_rus_fullahead_soldier_body1") == ModelKind::Body);
    CHECK(ClassifyByName("c_rus_fullahead_soldier_body1") == ModelKind::Head);
    CHECK(kind("c_ger_infantry_body_nogear") == ModelKind::Body);
    CHECK(kind("c_usa_jungmar_headblown_fb") == ModelKind::FullBody);
    CHECK(kind("c_usa_jungmar_barnes_ks_fb") == ModelKind::FullBody);
    CHECK(kind("c_usa_jungmar_barnes_ks_bdana") == ModelKind::Gear);
    CHECK(kind("c_usa_jungmar_head1_nc") == ModelKind::Head);
    CHECK(kind("c_usa_jungmar_body_drab") == ModelKind::Body);
    // the wounded bodies: an upper body for the body, a lower body worn on its bones
    CHECK(kind("c_vtn_nva1_body_g_upclean") == ModelKind::Body);
    CHECK(kind("c_vtn_nva2_body_g_legsoff") == ModelKind::Gear);

    // about a hundred of the models were misfiled
    std::size_t misfiled = 0;
    for (const auto& [model, modelKind] : kinds)
    {
        const auto byName = ClassifyByName(model);
        const bool bodies = (byName == ModelKind::Body || byName == ModelKind::FullBody) &&
                            (modelKind == ModelKind::Body || modelKind == ModelKind::FullBody);
        if (byName != modelKind && !bodies)
            ++misfiled;
    }
    CHECK(misfiled >= 80);
}

TEST("a character's burnt and wounded looks, and back from an actor's models")
{
    const auto& nva = Get("khe_sanh/c_vtn_nva1");
    CHECK(nva.burnt == "khe_sanh/c_vtn_nva1_char");
    const auto& burnt = Get(nva.burnt);
    CHECK(burnt.burntOf == nva.id);
    CHECK(nva.woundedTorsos.size() == 4 && nva.woundedLegs.size() == 4);

    using Components::CharacterLogic::GetLook;
    using Components::CharacterLogic::GetModels;
    CHECK((GetModels(nva, {}, &burnt) ==
           std::vector<std::string>{"c_vtn_nva1_body", "c_vtn_nva1_head", "c_vtn_nva1_gear"}));
    const auto burntModels = GetModels(nva, {.variant = Variant::Burnt}, &burnt);
    CHECK((burntModels ==
           std::vector<std::string>{"c_vtn_nva1_body_char", "c_vtn_nva1_head_char", "c_vtn_nva1_gear_char"}));
    CHECK(GetLook(nva, burntModels, &burnt).variant == Variant::Burnt);

    const auto wounded = GetModels(nva, {.variant = Variant::Wounded, .torso = 1, .legs = 3}, &burnt);
    CHECK((wounded == std::vector<std::string>{"c_vtn_nva1_body_g_rarmoff", "c_vtn_nva2_body_g_legsoff",
                                               "c_vtn_nva1_head", "c_vtn_nva1_gear"}));
    const auto look = GetLook(nva, wounded, &burnt);
    CHECK(look.variant == Variant::Wounded && look.torso == 1 && look.legs == 3);
    CHECK(GetLook(nva, nva.GetModels(), &burnt).variant == Variant::Normal);

    // a character without them keeps its own models
    const auto& woods = Get("khe_sanh/c_usa_jungmar_banres_khesanh");
    CHECK(GetModels(woods, {.variant = Variant::Burnt}, nullptr) == woods.GetModels());
    CHECK(GetModels(woods, {.variant = Variant::Wounded}, nullptr) == woods.GetModels());
}

TEST("an actor saved with models only gets the character they make up")
{
    using Components::CharacterLogic::FindByModels;
    const auto& characters = Shipped().characters;
    const auto knee =
        FindByModels(characters, {"c_usa_jungmar_body_drab", "c_usa_jungmar_head1_nc", "c_usa_jungmar_bandage_knee"});
    CHECK(knee && knee->id == "khe_sanh/c_usa_jungmar_wounded_knee");
    const auto assault =
        FindByModels(characters, {"c_usa_jungmar_body_drab", "c_usa_jungmar_head1_nc", "c_usa_jungmar_gear1"});
    CHECK(assault && assault->id == "khe_sanh/c_usa_jungmar_assault");
    const auto woods = FindByModels(characters, {"c_usa_jungmar_barnes_ks_fb"});
    CHECK(woods && woods->id == "khe_sanh/c_usa_jungmar_banres_khesanh");
    // a wounded upper body is the character's
    const auto wounded = FindByModels(characters, {"c_vtn_nva1_body_g_torso", "c_vtn_nva2_body_g_lowclean"});
    CHECK(wounded && wounded->id == "khe_sanh/c_vtn_nva1");
    CHECK(FindByModels(characters, {"c_usa_cia_mp_body_armor"}) == nullptr);
    CHECK(FindByModels(characters, {}) == nullptr);
}

TEST("an actor's models need the zone of its character, else of its burnt twin, else the first that has them")
{
    using Components::CharacterLogic::GetZonesFor;
    const auto& characters = Shipped().characters;
    const auto index = Components::CharacterLogic::IndexZones(characters);
    // the marine body and head are in many marines' zones: the actor's own character's is taken
    const auto& knee = Get("khe_sanh/c_usa_jungmar_wounded_knee");
    CHECK((GetZonesFor(knee.GetModels(), &knee, nullptr, index) ==
           std::vector<std::string>{"t5c_khe_sanh_usa_jungmar_wounded_knee"}));
    // burnt, the twin's zone has the charred models
    const auto& nva = Get("khe_sanh/c_vtn_nva1");
    const auto& burnt = Get(nva.burnt);
    const auto burntModels =
        Components::CharacterLogic::GetModels(nva, {.variant = Components::CharacterLogic::Variant::Burnt}, &burnt);
    CHECK((GetZonesFor(burntModels, &nva, &burnt, index) == std::vector<std::string>{"t5c_khe_sanh_vtn_nva1_char"}));
    // wounded, its own zone has the wounded bodies
    const auto wounded =
        Components::CharacterLogic::GetModels(nva, {.variant = Components::CharacterLogic::Variant::Wounded}, &burnt);
    CHECK((GetZonesFor(wounded, &nva, &burnt, index) == std::vector<std::string>{"t5c_khe_sanh_vtn_nva1"}));
    // a head swapped in from another character needs that character's zone too
    auto mixed = knee.GetModels();
    mixed[1] = "c_vtn_nva1_head";
    const auto zones = GetZonesFor(mixed, &knee, nullptr, index);
    CHECK(zones.size() == 2 && zones[0] == "t5c_khe_sanh_usa_jungmar_wounded_knee");
    // the map's own models need none, nor does an actor without a character
    CHECK(GetZonesFor({"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"}, nullptr, nullptr, index).empty());
    CHECK((GetZonesFor({"c_usa_jungmar_barnes_ks_fb"}, nullptr, nullptr, index) ==
           std::vector<std::string>{"t5c_khe_sanh_usa_jungmar_banres_khesanh"}));
}

TEST("a loose full body is a character of its level without a head: the field agent of The Defector")
{
    // c_usa_jungmar_fieldagent_fb, which only hue_city's own script spawns (Reznov's reveal), no character script names
    const auto& agent = Get("hue_city/c_usa_jungmar_fieldagent_fb");
    CHECK(agent.loose && agent.level == "hue_city" && agent.kind == Types::Character::Kind::Human);
    CHECK(agent.body == "c_usa_jungmar_fieldagent_fb" && agent.head.empty() && agent.hat.empty());
    CHECK(agent.zone == "t5c_hue_city_usa_jungmar_fieldagent_fb");
    CHECK(agent.bones.at("c_usa_jungmar_fieldagent_fb") == 115);
    CHECK(Components::CharacterLogic::Describe(agent, Shipped().levels) == "Marine field agent (The Defector)");
    // the picker's search finds it by "field" in its name and by "fieldagent" in its id
    CHECK(agent.name.find("field") != std::string::npos && agent.id.find("fieldagent") != std::string::npos);
    CHECK(Components::CharacterLogic::GetModelKinds(Shipped().characters).at("c_usa_jungmar_fieldagent_fb") ==
          ModelKind::FullBody);

    // a loose body with the parts of its name: the SR-71's ground crew, four heads
    const auto& crew = Get("wmd_sr71/c_usa_sr71_groundcrew_body");
    CHECK(crew.loose && crew.head == "c_usa_sr71_groundcrew_head1" && crew.heads.size() == 4);
    // the Zombies crew of the old maps: one character, in Shangri-La and the four maps of World at War
    const auto& richtofen = Get("zombie_temple/c_ger_richtofen_body");
    CHECK(richtofen.head == "c_ger_richtofen_head" && richtofen.hat == "c_ger_richtofen_offcap");
    CHECK(richtofen.alsoIn.size() == 4);
    // a loose head goes into the variants of the character it goes with: Brooks's on the Bowman disguise
    const auto& bowman = Get("flashpoint/c_usa_blackops_bowman_disguise");
    CHECK(std::ranges::find(bowman.heads, "c_usa_jungmar_head3_nohat") != bowman.heads.end());
    CHECK(bowman.head == "c_usa_blackops_bowman_disguise_headb");
    // the characters of the scripts aren't loose; 30 are (the 30 bodies of the levels no character names)
    CHECK(!Get("khe_sanh/c_usa_jungmar_banres_khesanh").loose);
    CHECK(std::ranges::count_if(Shipped().characters, [](const Types::Character& each) { return each.loose; }) == 30);
}

TEST("a shrunk twin goes with its zombie: Shangri-La's shrink ray's models")
{
    const auto& zombie = Get("zombie_temple/c_viet_zombie_nva1");
    CHECK(!zombie.loose);
    const auto& shrunk = Get("zombie_temple/c_viet_zombie_nva1_shrunk");
    CHECK(shrunk.loose && shrunk.kind == Types::Character::Kind::Zombie && shrunk.level == "zombie_temple");
    CHECK(shrunk.name == "Zombie NVA 1 (shrunk)");
    // its parts swapped for their shrunk models; the hat without one (c_viet_zombie_nohat) off, the gas mask's kept
    CHECK((shrunk.GetModels() == std::vector<std::string>{"c_viet_zombie_nva1_body_m", "c_viet_zombie_nva1_head_m"}));
    CHECK((shrunk.hats == std::vector<std::string>{"c_viet_zombie_nva1_gasmask_m"}));
    CHECK(Components::CharacterLogic::GetZoneModels(shrunk).size() == 3);
}

TEST("the map's multiplayer models make a character per body of each faction")
{
    const std::vector<std::string> nuketown{
        "c_rus_spet_mp_body_armor",    "c_rus_spet_mp_body_camo",    "c_rus_spet_mp_body_flak",
        "c_rus_spet_mp_body_standard", "c_rus_spet_mp_body_utility", "c_rus_spet_mp_head_1",
        "c_rus_spet_mp_head_2",        "c_rus_spet_mp_head_3",       "c_rus_spet_mp_head_4",
        "c_rus_spet_mp_head_5",        "c_usa_cia_mp_body_armor",    "c_usa_cia_mp_body_camo",
        "c_usa_cia_mp_body_flak",      "c_usa_cia_mp_body_standard", "c_usa_cia_mp_body_utility",
        "c_usa_cia_mp_head_1",         "c_usa_cia_mp_head_2",        "c_usa_cia_mp_head_3",
        "c_usa_cia_mp_head_4",         "c_usa_cia_mp_head_5",        "c_usa_jungmar_barnes_ks_fb"};
    const auto characters = Components::CharacterLogic::MakeMapCharacters(nuketown);
    CHECK(characters.size() == 10);
    const auto cia = Components::CharacterLogic::Find(characters, "map/c_usa_cia_mp_body_armor");
    CHECK(cia != nullptr);
    if (cia)
    {
        CHECK(cia->name == "CIA (armor)");
        CHECK(cia->head == "c_usa_cia_mp_head_1");
        CHECK(cia->heads.size() == 5);
        CHECK(cia->level.empty() && cia->zone.empty());
    }
    const auto spetsnaz = Components::CharacterLogic::Find(characters, "map/c_rus_spet_mp_body_utility");
    CHECK(spetsnaz && spetsnaz->name == "Spetsnaz (utility)" && spetsnaz->head == "c_rus_spet_mp_head_5");
    const auto kinds = Components::CharacterLogic::GetModelKinds(characters);
    CHECK(kinds.at("c_usa_cia_mp_head_3") == ModelKind::Head);
    CHECK(kinds.at("c_rus_spet_mp_body_flak") == ModelKind::Body);
}

TEST("the map's characters wear their body's own head, made for its cap or hood (no hair through the cap)")
{
    using Components::CharacterLogic::GetOwnHead;
    CHECK(GetOwnHead("c_usa_cia_mp_body_armor") == "c_usa_cia_mp_head_1");
    CHECK(GetOwnHead("c_rus_spet_mp_body_camo") == "c_rus_spet_mp_head_2");
    CHECK(GetOwnHead("c_vtn_nva_mp_body_flak") == "c_vtn_nva_mp_head_3");
    CHECK(GetOwnHead("c_usa_cia_mp_body_standard") == "c_usa_cia_mp_head_4");
    CHECK(GetOwnHead("c_cub_tropas_mp_body_utility") == "c_cub_tropas_mp_head_5");
    CHECK(GetOwnHead("c_usa_cia_mp_head_1").empty() && GetOwnHead("c_usa_jungmar_body_drab").empty() &&
          GetOwnHead("c_usa_cia_mp_body_other").empty());

    // Nuketown's twenty models: every body of both factions with its own head, all five to choose from
    std::vector<std::string> nuketown;
    for (const auto faction : {"c_rus_spet_mp", "c_usa_cia_mp"})
    {
        for (const auto kind : {"armor", "camo", "flak", "standard", "utility"})
            nuketown.push_back(std::format("{}_body_{}", faction, kind));
        for (int head = 1; head <= 5; ++head)
            nuketown.push_back(std::format("{}_head_{}", faction, head));
    }
    const auto characters = Components::CharacterLogic::MakeMapCharacters(nuketown);
    CHECK(characters.size() == 10);
    const std::vector<std::pair<std::string, std::string>> worn{
        {"c_usa_cia_mp_body_armor", "c_usa_cia_mp_head_1"},    {"c_usa_cia_mp_body_camo", "c_usa_cia_mp_head_2"},
        {"c_usa_cia_mp_body_flak", "c_usa_cia_mp_head_3"},     {"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_4"},
        {"c_usa_cia_mp_body_utility", "c_usa_cia_mp_head_5"},  {"c_rus_spet_mp_body_armor", "c_rus_spet_mp_head_1"},
        {"c_rus_spet_mp_body_camo", "c_rus_spet_mp_head_2"},   {"c_rus_spet_mp_body_flak", "c_rus_spet_mp_head_3"},
        {"c_rus_spet_mp_body_standard", "c_rus_spet_mp_head_4"}, {"c_rus_spet_mp_body_utility", "c_rus_spet_mp_head_5"}};
    for (const auto& [body, head] : worn)
    {
        const auto character = Components::CharacterLogic::Find(characters, "map/" + body);
        CHECK(character && character->head == head && character->heads.size() == 5);
        CHECK(character && (character->GetModels() == std::vector<std::string>{body, head}));
        // an actor of the body with another of its faction's heads is still that character
        CHECK(Components::CharacterLogic::FindByModels(characters, {body, body.substr(0, body.rfind("_body_")) +
                                                                              "_head_2"}) == character);
    }
    // a faction without the body's own head loaded wears its first
    const auto partial = Components::CharacterLogic::MakeMapCharacters(
        {"c_usa_cia_mp_body_flak", "c_usa_cia_mp_head_1", "c_usa_cia_mp_head_2"});
    CHECK(partial.size() == 1 && partial.front().head == "c_usa_cia_mp_head_1");
}

TEST("an actor saved with the old head_1 gets its body's own, one with another head keeps it")
{
    using Components::CharacterLogic::GiveOwnHead;
    std::vector<std::string> standard{"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"};
    CHECK(GiveOwnHead(standard, "map/c_usa_cia_mp_body_standard"));
    CHECK((standard == std::vector<std::string>{"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_4"}));
    CHECK(!GiveOwnHead(standard, "map/c_usa_cia_mp_body_standard"));
    // another head was picked, so it stays
    std::vector<std::string> picked{"c_rus_spet_mp_body_camo", "c_rus_spet_mp_head_3"};
    CHECK(!GiveOwnHead(picked, "map/c_rus_spet_mp_body_camo") && picked[1] == "c_rus_spet_mp_head_3");
    // armor's own head is head_1
    std::vector<std::string> armor{"c_usa_cia_mp_body_armor", "c_usa_cia_mp_head_1"};
    CHECK(!GiveOwnHead(armor, "map/c_usa_cia_mp_body_armor") && armor[1] == "c_usa_cia_mp_head_1");
    // not the map character (models from a scene or preset of their own, a campaign character), or more models
    std::vector<std::string> loose{"c_usa_cia_mp_body_flak", "c_usa_cia_mp_head_1"};
    CHECK(!GiveOwnHead(loose, "") && !GiveOwnHead(loose, "map/c_usa_cia_mp_body_camo"));
    std::vector<std::string> more{"c_usa_cia_mp_body_flak", "c_usa_cia_mp_head_1", "c_usa_jungmar_gear1"};
    CHECK(!GiveOwnHead(more, "map/c_usa_cia_mp_body_flak"));
    // as a scene loads
    Types::Actor actor{.models = {"c_rus_spet_mp_body_utility", "c_rus_spet_mp_head_1"},
                       .character = "map/c_rus_spet_mp_body_utility"};
    Types::Actor kept{.models = {"c_rus_spet_mp_body_utility", "c_rus_spet_mp_head_2"},
                      .character = "map/c_rus_spet_mp_body_utility"};
    const auto read = ActorLogic::SceneFromJson(
        ActorLogic::SceneToJson({.actors = {actor, kept}}, "demo"), 10000);
    CHECK(read.actors.size() == 2);
    CHECK(read.actors.size() == 2 && read.actors[0].models[1] == "c_rus_spet_mp_head_5" &&
          read.actors[1].models[1] == "c_rus_spet_mp_head_2");
}

TEST("characters have a kind: the campaign's and Zombies' dogs, the map's german shepherd (step 7)")
{
    using Kind = Types::Character::Kind;
    using Components::CharacterLogic::GetActorKind;
    // the catalog's dogs: WMD's german shepherd and Kino der Toten's hellhound (zombie_wolf), each a body alone
    for (const auto id : {"wmd/character_sp_german_sheperd_dog", "zombie_theater/character_sp_zombie_dog"})
    {
        const auto& dog = Get(id);
        CHECK(dog.kind == Kind::Dog && dog.head.empty() && !dog.zone.empty());
        CHECK(GetActorKind(dog.kind) == Types::ActorKind::Dog);
    }
    CHECK(Get("zombie_theater/character_sp_zombie_dog").body == "zombie_wolf");
    const auto dogs = std::ranges::count_if(Shipped().characters,
                                            [](const Types::Character& each) { return each.kind == Kind::Dog; });
    CHECK(dogs == 2);
    // a person's and a zombie's are a person's skeleton
    CHECK(Get("khe_sanh/c_usa_jungmar_banres_khesanh").kind == Kind::Human);
    CHECK(GetActorKind(Kind::Human) == Types::ActorKind::Human &&
          GetActorKind(Kind::Zombie) == Types::ActorKind::Human);
    // the dogs' bodies are full bodies (they have no head)
    const auto kinds = Components::CharacterLogic::GetModelKinds(Shipped().characters);
    CHECK(kinds.at("german_sheperd_dog") == ModelKind::FullBody && kinds.at("zombie_wolf") == ModelKind::FullBody);

    // multiplayer's dog is the map's, when the map has it loaded (every map does)
    const std::vector<std::string> models{"c_usa_cia_mp_body_armor", "c_usa_cia_mp_head_1", "german_shepherd"};
    const auto map = Components::CharacterLogic::MakeMapCharacters(models);
    const auto shepherd = Components::CharacterLogic::Find(map, "map/german_shepherd");
    CHECK(shepherd != nullptr);
    if (shepherd)
    {
        CHECK(shepherd->kind == Kind::Dog && shepherd->body == "german_shepherd" && shepherd->head.empty());
        CHECK(shepherd->name == "German shepherd" && shepherd->level.empty() && shepherd->zone.empty());
        CHECK(shepherd->GetModels() == std::vector<std::string>{"german_shepherd"});
    }
    CHECK(Components::CharacterLogic::GetModelKinds(map).at("german_shepherd") == ModelKind::FullBody);
    // the factions' first, the dog last; none without the model
    CHECK(map.size() == 2 && map.back().id == "map/german_shepherd");
    CHECK(Components::CharacterLogic::MakeMapCharacters({"c_usa_cia_mp_body_armor"}).size() == 1);
    // an actor of the dog's model alone is the dog
    CHECK(Components::CharacterLogic::FindByModels(map, {"german_shepherd"}) == shepherd);
}
