#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/PlayerLookLogic.hpp"
#include "Types/Actor.hpp"

using namespace IWXMVM;

// How a player of a demo looks: choices over the demo's models, the bones they take, the face
// paints

namespace
{
    namespace Look = Components::PlayerLookLogic;

    // as the test demo's clientInfo_t has them: the body, the head, and a stowed weapon on its tag
    const Look::Models MP_PLAYER = {
        {"c_usa_cia_mp_body_armor", ""}, {"c_usa_cia_mp_head_1", ""}, {"t5_weapon_ex41_world", "tag_stowed_back"}};
    constexpr std::size_t MOST = 7;
    constexpr std::uint32_t MAX_BONES = 160;

    Types::Character Marine()
    {
        Types::Character marine;
        marine.id = "khe_sanh/c_usa_jungmar_assault";
        marine.body = "c_usa_jungmar_body_drab";
        marine.head = "c_usa_jungmar_head1_nc";
        marine.gear = "c_usa_jungmar_gear1";
        return marine;
    }

    Types::Character FullBody()
    {
        Types::Character woods;
        woods.id = "khe_sanh/c_usa_jungmar_banres_khesanh";
        woods.body = "c_usa_jungmar_barnes_ks_fb";
        woods.hat = "c_usa_jungmar_barnes_ks_bdana";
        return woods;
    }

    std::vector<std::string> Names(const Look::Models& models)
    {
        std::vector<std::string> names;
        for (const auto& model : models)
            names.push_back(model.name);
        return names;
    }
}  // namespace

TEST("the game's face paints by the number it draws them by")
{
    CHECK((Look::FACE_PAINTS.size() == 25));
    CHECK((Look::GetFacePaintName(0) == "None"));
    CHECK((Look::GetFacePaintName(4) == "Sidewinder"));
    CHECK((Look::GetFacePaintName(11) == "Commando"));
    CHECK((Look::GetFacePaintName(24) == "Spider"));
    CHECK((Look::GetFacePaintName(25).empty()));
    CHECK((Look::GetFacePaintName(-1).empty()));
}

TEST("a player's choices go over those for all players")
{
    Types::Loadout all, player;
    all.body = "all_body";
    all.facePaint = 3;
    player.head = "player_head";
    const auto both = Look::Resolve(&player, nullptr, all);
    CHECK((both.body == "all_body"));
    CHECK((both.head == "player_head"));
    CHECK((both.facePaint == 3));
    CHECK((!both.worn));
    CHECK((both.HasModels()));

    player.facePaint = 0;
    CHECK((Look::Resolve(&player, nullptr, all).facePaint == 0));
    CHECK((Look::Resolve(nullptr, nullptr, all).facePaint == 3));

    CHECK((!Look::Resolve(nullptr, nullptr, Types::Loadout{}).HasModels()));
    Types::Loadout paintOnly;
    paintOnly.facePaint = 5;
    CHECK((!Look::Resolve(nullptr, nullptr, paintOnly).HasModels()));
}

TEST("a team's choices go between those of a player and those for all players")
{
    Types::Loadout all, team, player;
    all.body = "all_body";
    all.head = "all_head";
    all.facePaint = 1;
    team.body = "team_body";
    team.facePaint = 2;
    player.facePaint = 3;
    const auto resolved = Look::Resolve(&player, &team, all);
    CHECK((resolved.body == "team_body"));
    CHECK((resolved.head == "all_head"));
    CHECK((resolved.facePaint == 3));
    CHECK((Look::Resolve(nullptr, &team, all).facePaint == 2));
}

TEST("a team takes its name from the map's multiplayer bodies")
{
    const std::vector<std::string> nuketown{"c_rus_spet_mp_body_armor", "c_usa_cia_mp_body_armor"};
    CHECK((Look::GetTeamName(Look::TEAM_ALLIES, nuketown) == "CIA"));
    CHECK((Look::GetTeamName(Look::TEAM_AXIS, nuketown) == "Spetsnaz"));
    const std::vector<std::string> jungle{"c_usa_sog_mp_body_flak", "c_vtn_nva_mp_body_camo"};
    CHECK((Look::GetTeamName(Look::TEAM_ALLIES, jungle) == "SOG"));
    CHECK((Look::GetTeamName(Look::TEAM_AXIS, jungle) == "NVA"));
    const std::vector<std::string> cuba{"c_cub_rebels_mp_body_utility", "c_cub_tropas_mp_body_utility"};
    CHECK((Look::GetTeamName(Look::TEAM_ALLIES, cuba) == "Rebels"));
    CHECK((Look::GetTeamName(Look::TEAM_AXIS, cuba) == "Tropas"));
    CHECK((Look::GetTeamName(Look::TEAM_AXIS, {}) == "Team 1"));
}

TEST("a demo's choices for its players come back from its scene as they were")
{
    Types::PlayerChoices choices;
    Look::SetCharacter(choices.loadoutForAll, Marine());
    choices.loadoutForAll.facePaint = 19;
    Types::WeaponRule rule{std::string(Types::WeaponRule::ANY_PRIMARY)};
    rule.weapon = "ak47_mp";
    rule.attachments = std::vector<std::string>{"acog", "extclip"};
    rule.camo = 3;
    choices.teamLoadouts[Look::TEAM_ALLIES].weaponRules.push_back(rule);
    Look::SetCharacter(choices.playerLoadouts[7], FullBody());
    choices.deathForAll = "ai_death_flyback";
    choices.teamDeaths[Look::TEAM_AXIS] = "pb_death_faceplant";
    choices.playerDeaths[7] = "";
    CHECK((Look::ChoicesFromJson(nlohmann::json::parse(Look::ChoicesToJson(choices).dump())) == choices));

    // what isn't there in a scene from before is nothing chosen
    CHECK((Look::ChoicesFromJson(nlohmann::json::object()) == Types::PlayerChoices{}));
    CHECK((Look::ChoicesFromJson(nullptr) == Types::PlayerChoices{}));
}

TEST("a character sets the body, head and what is worn of a loadout")
{
    Types::Loadout loadout;
    Look::SetCharacter(loadout, Marine());
    CHECK((loadout.character == "khe_sanh/c_usa_jungmar_assault"));
    CHECK((loadout.body == "c_usa_jungmar_body_drab"));
    CHECK((loadout.head == "c_usa_jungmar_head1_nc"));
    CHECK((loadout.worn == std::vector<std::string>{"c_usa_jungmar_gear1"}));
    CHECK((Look::GetModels(loadout) ==
          std::vector<std::string>{"c_usa_jungmar_body_drab", "c_usa_jungmar_head1_nc", "c_usa_jungmar_gear1"}));

    // a full body has its face, and no head of the demo goes on it
    Look::SetCharacter(loadout, FullBody());
    CHECK((loadout.head == ""));
    CHECK((loadout.worn == std::vector<std::string>{"c_usa_jungmar_barnes_ks_bdana"}));
    CHECK((Look::GetModels(loadout) ==
          std::vector<std::string>{"c_usa_jungmar_barnes_ks_fb", "c_usa_jungmar_barnes_ks_bdana"}));
    CHECK((!loadout.IsEmpty()));
}

TEST("an actor's models make a loadout: the first head, the rest worn")
{
    Types::Loadout loadout;
    const auto isHead = [](const std::string& model) { return model.find("head") != std::string::npos; };
    Look::SetModels(loadout, {"body", "hat", "head_a", "head_b"}, "map/body", isHead);
    CHECK((loadout.body == "body"));
    CHECK((loadout.head == "head_a"));
    CHECK((loadout.worn == std::vector<std::string>{"hat", "head_b"}));
    CHECK((loadout.character == "map/body"));

    Look::SetModels(loadout, {"full_body"}, "", isHead);
    CHECK((loadout.head == ""));
    CHECK((loadout.worn->empty()));

    // no models leave it as it was
    Look::SetModels(loadout, {}, "other", isHead);
    CHECK((loadout.body == "full_body"));
}

TEST("models have a face to paint when one of them does, and may while they aren't loaded")
{
    Types::ActorCatalog catalog;
    catalog.models.push_back({"mason_fb", {}, "", 115, true});
    catalog.models.push_back({"hazmat_fb", {}, "", 90, false});
    CHECK((catalog.CanHaveFacePaint({"mason_fb"})));
    CHECK((!catalog.CanHaveFacePaint({"hazmat_fb"})));
    CHECK((catalog.CanHaveFacePaint({"hazmat_fb", "mason_fb"})));
    CHECK((catalog.CanHaveFacePaint({"not_loaded"})));
    // Keep: the demo's own models
    CHECK((catalog.CanHaveFacePaint({})));
}

TEST("what isn't chosen stays the demo's and the stowed weapon always does")
{
    CHECK((Look::GetWanted(MP_PLAYER, {}, MOST) == MP_PLAYER));

    Look::ResolvedCharacter marine{"body", "head", std::vector<std::string>{"gear"}, std::nullopt};
    CHECK((Look::GetWanted(MP_PLAYER, marine, MOST) ==
          Look::Models{{"body", ""}, {"head", ""}, {"gear", ""}, {"t5_weapon_ex41_world", "tag_stowed_back"}}));

    // the body alone keeps the demo's head
    Look::ResolvedCharacter body{"body", std::nullopt, std::nullopt, std::nullopt};
    CHECK((Names(Look::GetWanted(MP_PLAYER, body, MOST)) ==
          std::vector<std::string>{"body", "c_usa_cia_mp_head_1", "t5_weapon_ex41_world"}));

    // an empty head takes the demo's off
    Look::ResolvedCharacter noHead{"full_body", std::string(), std::vector<std::string>{}, std::nullopt};
    CHECK((Names(Look::GetWanted(MP_PLAYER, noHead, MOST)) ==
          std::vector<std::string>{"full_body", "t5_weapon_ex41_world"}));

    // a player with nothing stowed
    const Look::Models unstowed(MP_PLAYER.begin(), MP_PLAYER.begin() + 2);
    CHECK((Look::GetWanted(unstowed, marine, MOST) == Look::Models{{"body", ""}, {"head", ""}, {"gear", ""}}));

    // no more than the game lists
    Look::ResolvedCharacter many{"body", "head", std::vector<std::string>{"a", "b", "c", "d", "e", "f", "g"},
                                 std::nullopt};
    CHECK((Look::GetWanted(MP_PLAYER, many, MOST).size() == MOST));
}

TEST("a player's models fit a DObj: body, head and gun first, then what is worn, then the stowed weapon")
{
    const Look::Models models{{"body", ""}, {"head", ""}, {"gear", ""}, {"stowed", "tag_stowed_back"}};
    constexpr std::uint32_t AUG = 20;

    // an MP body fits whole, with what it wears and its guns
    CHECK((Look::Fit(models, std::array<std::uint32_t, 4>{84, 27, 5, 20}, AUG, MAX_BONES) == models));

    // the stowed weapon goes before what is worn
    CHECK((Names(Look::Fit(models, std::array<std::uint32_t, 4>{95, 27, 3, 18}, AUG, MAX_BONES)) ==
           std::vector<std::string>{"body", "head", "gear"}));

    // what is worn goes when it alone is too much, and the stowed weapon may then stay
    CHECK((Names(Look::Fit(models, std::array<std::uint32_t, 4>{95, 27, 20, 15}, AUG, MAX_BONES)) ==
           std::vector<std::string>{"body", "head", "stowed"}));

    // the full body of the test demo's crash of 2026-11-03 ("dobj ... has more than 160 bones"): Woods with his
    // bandana and an AUG, its spine rig and a stowed gun
    const Look::Models woods{{"c_usa_jungmar_barnes_ks_fb", ""},
                             {"c_usa_jungmar_barnes_ks_bdana", ""},
                             {"t5_weapon_m16a1_world", "tag_stowed_back"}};
    constexpr std::uint32_t RIG = 5;
    CHECK((Names(Look::Fit(woods, std::array<std::uint32_t, 3>{120, 5, 18}, AUG + RIG, MAX_BONES)) ==
           std::vector<std::string>{"c_usa_jungmar_barnes_ks_fb", "c_usa_jungmar_barnes_ks_bdana"}));
    // and with a pistol it keeps the stowed gun
    CHECK((Look::Fit(woods, std::array<std::uint32_t, 3>{120, 5, 18}, 8 + RIG, MAX_BONES) == woods));
}
