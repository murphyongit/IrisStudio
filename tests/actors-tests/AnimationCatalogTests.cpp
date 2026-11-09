#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/AnimationCatalogLogic.hpp"

#include <set>

using namespace IWXMVM;

// The catalog of every animation the game has (AnimationCatalogLogic): reading it,
// which animations fit which models, searching it, what each is, and what ActorLogic takes from it

namespace
{
    using Kind = Types::AnimationKind;
    using Family = Types::AnimationFamily;
    using Posture = Types::AnimationPosture;
    using Movement = Types::AnimationMovement;

    const std::filesystem::path DATA = std::filesystem::path(__FILE__).parent_path() / "data";
    // the catalog that comes with the mod (tools/t5-characters/animations.py over the game's zones)
    const std::filesystem::path SHIPPED_CATALOG =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "core" / "resources" /
        "animations.json";

    nlohmann::json ReadJson(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        return nlohmann::json::parse(file);
    }

    // A slice of that catalog (some animations of each kind, two copies of a name that differ) with the bones of some
    // models: an MP body and head (mp_nuked), the S.O.G. marine's body, head and Bowman's full body (khe_sanh), the dog
    // (common_mp)
    const nlohmann::json& SliceJson()
    {
        static const auto json = ReadJson(DATA / "animation-catalog-slice.json");
        return json;
    }

    const Types::AnimationCatalog& Slice()
    {
        static const auto catalog = AnimationCatalogLogic::Parse(SliceJson());
        return catalog;
    }

    const Types::AnimationCatalog& Shipped()
    {
        static const auto catalog = [] {
            try
            {
                return AnimationCatalogLogic::Parse(ReadJson(SHIPPED_CATALOG));
            }
            catch (const std::exception& e)
            {
                Test::Fail(__FILE__, __LINE__, std::string("the catalog doesn't read: ") + e.what());
                return Types::AnimationCatalog{};
            }
        }();
        return catalog;
    }

    std::vector<std::string> Bones(std::initializer_list<std::string_view> models)
    {
        std::vector<std::string> bones;
        for (const auto model : models)
        {
            const auto& list = SliceJson().at("models").at(std::string(model));
            for (const auto& bone : list)
                bones.push_back(bone.get<std::string>());
        }
        return bones;
    }

    AnimationCatalogLogic::ModelRig Rig(std::initializer_list<std::string_view> models)
    {
        const auto bones = Bones(models);
        return AnimationCatalogLogic::GetModelRig(bones);
    }

    const Types::AnimationEntry& Get(const Types::AnimationCatalog& catalog, std::string_view name)
    {
        static const Types::AnimationEntry none;
        const auto entry = catalog.Find(name);
        CHECK(entry != nullptr);
        return entry ? *entry : none;
    }

    bool Fits(std::string_view animation, const AnimationCatalogLogic::ModelRig& rig)
    {
        return AnimationCatalogLogic::Fits(Slice(), Get(Slice(), animation), rig);
    }
}  // namespace

TEST("animation catalog: reads an animation's numbers, notetracks, motion and states")
{
    const auto& slice = Slice();
    CHECK(slice.entries.size() == SliceJson().at("animations").size());
    CHECK(std::ranges::is_sorted(slice.entries, {}, &Types::AnimationEntry::name));

    const auto& run = Get(slice, "pb_combatrun_forward_loop");
    CHECK(run.frames == 50 && run.rate == 30.0f && run.loop && run.delta);
    CHECK_NEAR(run.Duration(), 1.6667f, 0.001f);
    CHECK(run.kind == Kind::FullBody && run.family == Family::MpPlayer);
    // 351.76 units a play over 1.67 s: the game's 211 a second
    CHECK_NEAR(run.motion.x / run.Duration(), 211.06f, 0.1f);
    CHECK((run.from == Types::AnimationState{Posture::Stand, Movement::Run}) && run.to == run.from);
    const auto source = slice.GetSource(run);
    CHECK(source && source->zone == "common_mp" && source->group == "Multiplayer");

    const auto& reload = Get(slice, "pt_reload_stand_auto");
    CHECK(reload.kind == Kind::Torso && !reload.loop && reload.notetracks.size() > 2);
    CHECK(std::ranges::is_sorted(reload.notetracks, {}, &Types::ActorNotetrack::time));
    CHECK(reload.notetracks.back().name == "end" && reload.notetracks.back().time == 1.0f);

    // a stop from a run with a turn: the name says what changes, the root turns 90 degrees left
    const auto& stop = Get(slice, "run_2_stand_90l");
    CHECK((stop.from == Types::AnimationState{Posture::Stand, Movement::Run}));
    CHECK((stop.to == Types::AnimationState{Posture::Stand, Movement::Stop}));
    CHECK_NEAR(stop.turn, 90.0f, 0.5f);
    CHECK((Get(slice, "pb_prone2crouch").from.posture == Posture::Prone && Get(slice, "pb_prone2crouch").to.posture ==
                                                                          Posture::Crouch));
    // MP's turns turn by the name (the game turns the player in code)
    CHECK_NEAR(Get(slice, "pb_turn_90left").turn, 90.0f, 0.01f);
    // a pose of one frame
    CHECK(Get(slice, "pf_death").kind == Kind::Face);

    // states as the harness shows them
    CHECK(AnimationCatalogLogic::DescribeState(stop.from) == "stand run");
    CHECK(AnimationCatalogLogic::DescribeState({}) == "-");
}

TEST("animation catalog: a name whose copies differ is listed once for each")
{
    const auto& slice = Slice();
    const auto& game = Get(slice, "pb_stand_alert_pistol");
    const auto& zombies = Get(slice, "pb_stand_alert_pistol (common_zombie)");
    // multiplayer's copy keeps the name, as it is the one the game has
    CHECK(slice.GetSource(game) && slice.GetSource(game)->zone == "common_mp");
    CHECK(slice.GetSource(zombies) && slice.GetSource(zombies)->zone == "common_zombie");
    CHECK(zombies.AssetName() == "pb_stand_alert_pistol" && game.AssetName() == "pb_stand_alert_pistol");
    CHECK(game.hash != zombies.hash && !game.hash.empty());
    // the zombie copy has more bones
    CHECK(slice.boneSets[zombies.boneSet].size() > slice.boneSets[game.boneSet].size());
}

TEST("animation catalog: refuses a catalog of another version or one that points nowhere")
{
    auto json = SliceJson();
    json["version"] = 2;
    bool threw = false;
    try
    {
        AnimationCatalogLogic::Parse(json);
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    CHECK(threw);

    json = SliceJson();
    json["animations"][0]["b"] = 9999;
    threw = false;
    try
    {
        AnimationCatalogLogic::Parse(json);
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    CHECK(threw);
}

TEST("animation catalog: deaths and alert animations are worked out with ActorLogic's rules")
{
    const auto& slice = Slice();
    CHECK(Get(slice, "ai_death_fallforward").death);
    CHECK(Get(slice, "ai_zombie_quad_death_tesla").death);
    CHECK(!Get(slice, "pb_stand_alert").death && !Get(slice, "run_2_stand_90l").death);
    // the players' are alert, as are aims; a casual walk isn't, cqb's is
    CHECK(Get(slice, "pb_stand_alert").alert && Get(slice, "pt_reload_stand_auto").alert);
    CHECK(Get(slice, "exposed_idle_alert_v1").alert && Get(slice, "walk_cqb_f").alert);
    CHECK(!Get(slice, "ai_death_fallforward").alert);
    // the same as the rules say of them
    for (const auto& entry : slice.entries)
    {
        const std::string name(entry.AssetName());
        CHECK(entry.death == ActorLogic::IsDeathAnimation(name, entry.loop, entry.notetracks));
        CHECK(entry.alert ==
              ActorLogic::IsAlertAnimation(name, ActorLogic::AnimationInfo{entry.Duration(), std::nullopt,
                                                                           entry.notetracks, entry.kind, entry.family}));
    }
}

TEST("animation catalog: models' rigs")
{
    const auto dog = Rig({"german_shepherd"});
    CHECK(dog.dog && !dog.human && !dog.face);
    const auto mp = Rig({"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"});
    CHECK(mp.human && !mp.dog && mp.face);
    CHECK(!Rig({"c_usa_cia_mp_body_standard"}).face);
    // a head alone is no body
    const auto head = Rig({"c_usa_cia_mp_head_1"});
    CHECK(!head.human && head.face);
    // the SP rig gets the spine rig's bones, the MP rig has them
    const auto sp = Rig({"c_usa_jungmar_body_drab", "c_usa_jungmar_head1_nc"});
    CHECK(sp.human && sp.bones.contains("torso_stabilizer") && sp.bones.contains("pelvis"));
    const auto spBones = Bones({"c_usa_jungmar_body_drab"});
    CHECK(std::ranges::find(spBones, "torso_stabilizer") == spBones.end());
}

TEST("animation catalog: what fits an MP body, an SP body and full body (6.2)")
{
    for (const auto rig : {Rig({"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"}),
                           Rig({"c_usa_jungmar_body_drab", "c_usa_jungmar_head1_nc"}),
                           Rig({"c_usa_jungmar_bowman_fb"})})
    {
        // MP's and SP's animations both ways (the SP body through the spine rig), the zombies', the faces', a
        // cutscene's (its face, clothes and camera bones don't count)
        for (const auto animation : {"pb_stand_alert", "pt_reload_stand_auto", "pb_combatrun_forward_loop",
                                     "ai_death_fallforward", "walk_cqb_f", "ai_zombie_quad_death_tesla",
                                     "ch_khe_e1b_chinookdropoff_guy03", "pf_death", "pf_casual_idle",
                                     "pb_stand_alert_pistol (common_zombie)"})
            CHECK(Fits(animation, rig));
        // not a dog's, a buffalo's, a prop's or one of no bones
        for (const auto animation :
             {"german_shepherd_run", "zombie_dog_run", "a_water_buffalo_idle_a", "fxanim_gp_tarp1_anim",
              "player_mantle_over_high"})
            CHECK(!Fits(animation, rig));
    }
    // a parachute's arms have finger bones no body has
    CHECK(!Fits("int_base_jump_chuteopen_loop", Rig({"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"})));
    // an MP body without a head has no face to move
    CHECK(!Fits("pf_death", Rig({"c_usa_cia_mp_body_standard"})) &&
          Fits("pb_stand_alert", Rig({"c_usa_cia_mp_body_standard"})));
    // a head alone: only faces
    CHECK(Fits("pf_death", Rig({"c_usa_cia_mp_head_1"})) && !Fits("pb_stand_alert", Rig({"c_usa_cia_mp_head_1"})));
}

TEST("animation catalog: what fits the dog")
{
    const auto dog = Rig({"german_shepherd"});
    CHECK(Fits("german_shepherd_run", dog) && Fits("zombie_dog_run", dog));
    // the buffalo's rig has the dog's bone names, but not all of them (27 of 30 of its bones are the dog's)
    CHECK(!Fits("a_water_buffalo_idle_a", dog));
    for (const auto animation : {"pb_stand_alert", "ai_death_fallforward", "pf_death", "fxanim_gp_tarp1_anim"})
        CHECK(!Fits(animation, dog));
}

TEST("animation catalog: GetFits is Fits of each entry")
{
    for (const auto rig : {Rig({"german_shepherd"}), Rig({"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"}),
                           Rig({"c_usa_jungmar_body_drab"}), Rig({"c_usa_cia_mp_head_1"})})
    {
        const auto fits = AnimationCatalogLogic::GetFits(Slice(), rig);
        CHECK(fits.size() == Slice().entries.size());
        for (std::size_t i = 0; i < fits.size() && i < Slice().entries.size(); ++i)
            CHECK(fits[i] == AnimationCatalogLogic::Fits(Slice(), Slice().entries[i], rig));
    }
}

TEST("animation catalog: searches names and zones by every word, any case")
{
    const auto names = [](std::string_view search) {
        std::set<std::string> found;
        for (const auto entry : AnimationCatalogLogic::Search(Slice(), search))
            found.insert(entry->name);
        return found;
    };
    CHECK(names("").size() == Slice().entries.size());
    CHECK(names("PB_STAND_alert") ==
          (std::set<std::string>{"pb_stand_alert", "pb_stand_alert_pistol", "pb_stand_alert_pistol (common_zombie)"}));
    CHECK(names("alert pistol").size() == 2);
    // by zone and by the zone's name
    CHECK(names("khe_sanh").contains("ch_khe_e1b_chinookdropoff_guy03"));
    CHECK(names("s.o.g. chinook").size() == 1);
    CHECK(names("zombie_moon tesla") == (std::set<std::string>{"ai_zombie_quad_death_tesla (zombie_moon)"}));
    CHECK(names("nothing_like_it").empty());
}

TEST("animation catalog: categories from the catalog, else the name")
{
    const auto category = [](std::string_view name) {
        const auto& entry = Get(Slice(), name);
        return AnimationCatalogLogic::GetCategory(entry.name, entry.kind, &entry);
    };
    CHECK(category("pf_death") == "Faces");
    CHECK(category("ai_death_fallforward") == "Deaths");
    CHECK(category("german_shepherd_run") == "Animals" && category("a_water_buffalo_idle_a") == "Animals");
    CHECK(category("fxanim_gp_tarp1_anim") == "Props and vehicles");
    // moving or changing stance as the catalog says
    CHECK(category("pb_combatrun_forward_loop") == "Movement" && category("pb_prone2crouch") == "Movement");
    CHECK(category("pt_reload_stand_auto") == "Combat");
    CHECK(category("pb_stand_alert") == "Idle and poses");
    // without the catalog: by kind and name
    CHECK(AnimationCatalogLogic::GetCategory("f_smile", Kind::Face, nullptr) == "Faces");
    CHECK(AnimationCatalogLogic::GetCategory("some_death_v2", Kind::FullBody, nullptr) == "Deaths");
    CHECK(AnimationCatalogLogic::GetCategory("walk_somewhere", Kind::FullBody, nullptr) == "Movement");
    for (const auto& entry : Slice().entries)
        CHECK(std::ranges::find(AnimationCatalogLogic::GetCategories(),
                                AnimationCatalogLogic::GetCategory(entry.name, entry.kind, &entry)) !=
              AnimationCatalogLogic::GetCategories().end());
}

TEST("animation catalog: the browser's list, with what the game has loaded")
{
    Types::ActorCatalog game;
    game.animations = {{"pb_stand_alert", 12.8f, true, {}, Kind::FullBody, Family::MpPlayer},
                       {"pf_death", 2.07f, false, {}, Kind::Face, Family::Human},
                       {"some_new_animation", 1.0f, false, {}, Kind::FullBody, Family::SpHuman}};
    std::ranges::sort(game.animations, {}, &Types::ActorCatalog::Animation::name);
    const auto list = AnimationCatalogLogic::List(Slice(), game);
    CHECK(list.animations.size() == Slice().entries.size() + 1);
    const auto find = [&](std::string_view name) {
        const auto it = std::ranges::find(list.animations, name, &AnimationCatalogLogic::Listed::name);
        return it != list.animations.end() ? &*it : nullptr;
    };
    const auto alert = find("pb_stand_alert");
    CHECK(alert && alert->loaded && alert->entry && alert->loaded->name == "pb_stand_alert");
    CHECK(alert && alert->index < Slice().entries.size() && &Slice().entries[alert->index] == alert->entry);
    // the zombies' copy isn't what the game has
    const auto zombies = find("pb_stand_alert_pistol (common_zombie)");
    CHECK(zombies && !zombies->loaded);
    CHECK(find("ch_khe_e1b_chinookdropoff_guy03") && !find("ch_khe_e1b_chinookdropoff_guy03")->loaded);
    // the game's first person arms' aren't listed, as the catalog doesn't list them
    game.animations.push_back({"viewmodel_ak47_reload", 2.0f, false, {}, Kind::Other, Family::Viewmodel});
    game.animations.push_back({"radio_handheld_idle", 2.0f, true, {}, Kind::Other, Family::Viewmodel});
    std::ranges::sort(game.animations, {}, &Types::ActorCatalog::Animation::name);
    CHECK(AnimationCatalogLogic::List(Slice(), game).animations.size() == list.animations.size());
    CHECK(!AnimationCatalogLogic::IsListed("viewmodel_ak47_reload", Family::Other) &&
          AnimationCatalogLogic::IsListed("pb_stand_alert", Family::MpPlayer));
    // one the catalog doesn't have is listed last, as loaded
    const auto extra = find("some_new_animation");
    CHECK(extra && extra->loaded && !extra->entry && extra->index == SIZE_MAX);
    CHECK(extra && list.headings[extra->heading] == "Loaded in the game" && &list.animations.back() == extra);
    // by where they come from: multiplayer's first, the levels' by name and group
    CHECK(list.headings.front() == "Multiplayer");
    CHECK(std::ranges::find(list.headings, "S.O.G. (Campaign)") != list.headings.end());
    CHECK(std::ranges::is_sorted(list.animations, {}, &AnimationCatalogLogic::Listed::heading));
    CHECK(alert && alert->lowerName == "pb_stand_alert" && AnimationCatalogLogic::GetCategories()[alert->category] ==
                                                               "Idle and poses");
}

TEST("animation catalog: ActorLogic takes the catalog's stance, death and alert")
{
    // the catalog's pose wins over the name: the last stand's upper body lies down
    const auto& laststand = Get(Slice(), "pt_laststand_fire");
    CHECK(laststand.from.posture == Posture::Prone);
    ActorLogic::AnimationInfo info{1.0f, std::nullopt, {}, Kind::Torso, Family::MpPlayer};
    CHECK(ActorLogic::GetStance("pt_laststand_fire", info) == Types::ActorStance::Stand);
    info.stance = AnimationCatalogLogic::GetStance(laststand.from.posture);
    CHECK(ActorLogic::GetStance("pt_laststand_fire", info) == Types::ActorStance::Prone);
    CHECK(ActorLogic::GetStance("pt_reload_crouch_rifle", std::nullopt) == Types::ActorStance::Crouch);
    CHECK(!AnimationCatalogLogic::GetStance(Posture::Unknown) &&
          AnimationCatalogLogic::GetStance(Posture::Back) == Types::ActorStance::Prone);
    // a flag the catalog gives is taken as it is
    ActorLogic::AnimationInfo flagged{1.0f, std::nullopt, {}, Kind::FullBody, Family::MpPlayer};
    flagged.alert = false;
    CHECK(!ActorLogic::IsAlertAnimation("pb_stand_alert", flagged));
    flagged.alert.reset();
    CHECK(ActorLogic::IsAlertAnimation("pb_stand_alert", flagged));
}

TEST("animation catalog: the shipped catalog (docs/t5-animation-plan.md 6.1)")
{
    const auto& shipped = Shipped();
    // every animation of the game but the first person arms', and the copies that differ
    CHECK(shipped.entries.size() > 7800 && shipped.entries.size() < 8100);
    CHECK(std::ranges::adjacent_find(shipped.entries, {}, &Types::AnimationEntry::name) == shipped.entries.end());
    CHECK(std::ranges::none_of(shipped.entries, [](const Types::AnimationEntry& entry) {
        return entry.family == Family::Viewmodel || entry.name.starts_with("viewmodel_");
    }));
    const auto twins = std::ranges::count_if(
        shipped.entries, [](const Types::AnimationEntry& entry) { return entry.AssetName() != entry.name; });
    CHECK(twins > 20 && twins < 60);
    // the six faces of the players, the legs' idles, the deaths actors start with
    for (const auto animation : {"pf_casual_idle", "pf_alert_idle", "pf_firing", "pf_melee", "pf_pain", "pf_death",
                                 "pb_stand_alert", "pb_crouch_alert", "pb_prone_aim", "ai_death_fallforward"})
        CHECK(shipped.Find(animation) != nullptr);

    // the dog: its own (german_shepherd_, german_shepard_, the Zombies' hellhounds) and nothing else
    const auto dog = Rig({"german_shepherd"});
    const auto fits = AnimationCatalogLogic::GetFits(shipped, dog);
    std::size_t fitting = 0;
    for (std::size_t i = 0; i < fits.size(); ++i)
    {
        if (!fits[i])
            continue;
        ++fitting;
        const auto& name = shipped.entries[i].name;
        CHECK(name.starts_with("german_shep") || name.starts_with("zombie_dog_"));
    }
    CHECK(fitting >= 120 && fitting <= 140);

    // an MP body and head: every human body's and face's animation but a few of extra bones, nothing else
    const auto mp = AnimationCatalogLogic::GetFits(shipped, Rig({"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"}));
    std::size_t humans = 0, humansFitting = 0;
    for (std::size_t i = 0; i < mp.size(); ++i)
    {
        const auto& entry = shipped.entries[i];
        const bool human = entry.family == Family::MpPlayer || entry.family == Family::SpHuman ||
                           entry.family == Family::Human;
        if (!human)
            CHECK(!mp[i]);
        humans += human ? 1 : 0;
        humansFitting += human && mp[i] ? 1 : 0;
    }
    CHECK(humansFitting + 10 >= humans && humansFitting > 6500);
}

TEST("animation catalog: the fit filter's lists for a dog, an MP body, an SP body and a head (F3, step 7)")
{
    const auto list = [](const AnimationCatalogLogic::ModelRig& rig) {
        std::set<std::string> names;
        const auto fits = AnimationCatalogLogic::GetFits(Slice(), rig);
        for (std::size_t i = 0; i < fits.size(); ++i)
            if (fits[i])
                names.insert(Slice().entries[i].name);
        return names;
    };
    // the dog: its own (multiplayer's and the hellhound's) and nothing a person's
    CHECK(list(Rig({"german_shepherd"})) == (std::set<std::string>{"german_shepherd_run", "zombie_dog_run"}));
    // a person's body and head: every person's and face's animation, none of the dog's, animals', props', of no bones
    const std::set<std::string> person{"pb_stand_alert",
                                       "pt_reload_stand_auto",
                                       "pb_combatrun_forward_loop",
                                       "pb_turn_90left",
                                       "pb_prone2crouch",
                                       "pt_laststand_fire",
                                       "ai_death_fallforward",
                                       "run_2_stand_90l",
                                       "walk_cqb_f",
                                       "exposed_idle_alert_v1",
                                       "pf_death",
                                       "pf_casual_idle",
                                       "pb_stand_alert_pistol",
                                       "pb_stand_alert_pistol (common_zombie)",
                                       "ch_khe_e1b_chinookdropoff_guy03",
                                       "ai_zombie_quad_death_tesla",
                                       "ai_zombie_quad_death_tesla (zombie_moon)"};
    CHECK(list(Rig({"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"})) == person);
    CHECK(list(Rig({"c_usa_jungmar_body_drab", "c_usa_jungmar_head1_nc"})) == person);
    // the parachute's fingers are what the S.O.G. bodies lack too
    CHECK(!list(Rig({"c_usa_jungmar_bowman_fb"})).contains("int_base_jump_chuteopen_loop"));
    // an MP body alone: no faces; a head alone: faces only
    auto bodyAlone = person;
    bodyAlone.erase("pf_death");
    bodyAlone.erase("pf_casual_idle");
    CHECK(list(Rig({"c_usa_cia_mp_body_standard"})) == bodyAlone);
    CHECK(list(Rig({"c_usa_cia_mp_head_1"})) == (std::set<std::string>{"pf_death", "pf_casual_idle"}));
}

TEST("animation catalog: why an animation doesn't fit, in words")
{
    const auto why = [](std::string_view animation, std::initializer_list<std::string_view> models) {
        return AnimationCatalogLogic::DescribeMisfit(Slice(), Get(Slice(), animation), Rig(models));
    };
    const auto has = [](const std::string& text, std::string_view part) {
        return text.find(part) != std::string::npos;
    };
    // a person's on a dog, a dog's on a person, a face on a dog, on a body without a head, a body's on a head
    CHECK(has(why("pb_stand_alert", {"german_shepherd"}), "a person's skeleton: the actor is a dog"));
    CHECK(has(why("ai_death_fallforward", {"german_shepherd"}), "the actor is a dog"));
    CHECK(has(why("german_shepherd_run", {"c_usa_cia_mp_body_standard"}), "a dog's skeleton: the actor is a person"));
    CHECK(has(why("pf_death", {"german_shepherd"}), "a person's face: the actor is a dog"));
    CHECK(has(why("pf_death", {"c_usa_cia_mp_body_standard"}), "a body without a head"));
    CHECK(has(why("pb_stand_alert", {"c_usa_cia_mp_head_1"}), "a head alone"));
    // the buffalo's on a dog: another animal's; a prop's; one that moves nothing; bones a body lacks, counted
    CHECK(has(why("a_water_buffalo_idle_a", {"german_shepherd"}), "another animal's skeleton"));
    CHECK(has(why("fxanim_gp_tarp1_anim", {"german_shepherd"}), "a prop or vehicle"));
    CHECK(why("player_mantle_over_high", {"german_shepherd"}) == "It moves no bones");
    CHECK(has(why("int_base_jump_chuteopen_loop", {"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"}),
              "bones the actor's models don't have"));
    // empty exactly when it fits
    for (const auto rig : {Rig({"german_shepherd"}), Rig({"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"}),
                           Rig({"c_usa_jungmar_body_drab"}), Rig({"c_usa_cia_mp_head_1"})})
        for (const auto& entry : Slice().entries)
            CHECK(AnimationCatalogLogic::DescribeMisfit(Slice(), entry, rig).empty() ==
                  AnimationCatalogLogic::Fits(Slice(), entry, rig));
}

TEST("animation catalog: how many of the shipped catalog's fit each model, as the browser lists them (step 5's)")
{
    const auto count = [](std::initializer_list<std::string_view> models) {
        const auto fits = AnimationCatalogLogic::GetFits(Shipped(), Rig(models));
        return std::ranges::count(fits, true);
    };
    CHECK(count({"german_shepherd"}) == 128);
    CHECK(count({"c_usa_cia_mp_body_standard", "c_usa_cia_mp_head_1"}) == 6773);
    CHECK(count({"c_usa_jungmar_body_drab", "c_usa_jungmar_head1_nc"}) == 6773);
    CHECK(count({"c_usa_cia_mp_body_standard"}) == 6723);
    CHECK(count({"c_usa_cia_mp_head_1"}) == 50);
}
