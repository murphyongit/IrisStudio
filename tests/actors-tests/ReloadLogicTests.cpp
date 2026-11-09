#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/PoseLogic.hpp"
#include "Components/ReloadLogic.hpp"

using namespace IWXMVM;
using Types::ReloadHand;
using Types::ReloadKind;
using Types::ReloadSource;

namespace
{
    // Recorded in the game (Nuketown, with the Vorkuta pack for the SP animations; harness `reloadnotetracks` and
    // `reloaddata`): every third-person reload animation's notetracks, and for some of them where the decoded wrists,
    // tag_inhand and tag_weapon_right are over the animation on c_usa_cia_mp_body_standard, with the gun's rig
    const std::filesystem::path DATA = std::filesystem::path(__FILE__).parent_path() / "data";
    // the curated table that comes with the mod
    const std::filesystem::path SHIPPED_TABLE =
        DATA.parent_path().parent_path().parent_path() / "core" / "resources" / "reload-profiles.json";

    nlohmann::json ReadJson(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        return nlohmann::json::parse(file, nullptr, true, true);
    }

    std::vector<Types::ActorNotetrack> ReadNotetracks(const nlohmann::json& json)
    {
        std::vector<Types::ActorNotetrack> notetracks;
        for (const auto& notetrack : json)
            notetracks.push_back({notetrack[0].get<std::string>(), notetrack[1].get<float>()});
        return notetracks;
    }

    // [x, y, z, qw, qx, qy, qz]
    glm::mat4 ReadTransform(const nlohmann::json& json)
    {
        const glm::quat rotation(json[3].get<float>(), json[4].get<float>(), json[5].get<float>(),
                                 json[6].get<float>());
        return glm::translate(glm::mat4(1.0f),
                              glm::vec3(json[0].get<float>(), json[1].get<float>(), json[2].get<float>())) *
               glm::mat4_cast(glm::normalize(rotation));
    }

    struct Recorded
    {
        std::string name;
        float duration = 0.0f;
        std::vector<Types::ActorNotetrack> notetracks;
        std::optional<Types::WeaponRig> rig;
        std::vector<float> times;
        std::vector<ReloadLogic::ReloadBones> bones;

        // between the recorded times, as the game would decode them there
        ReloadLogic::BonesAt Lookup() const
        {
            return [this](float time) -> std::optional<ReloadLogic::ReloadBones> {
                if (times.empty())
                    return std::nullopt;
                const auto next = std::ranges::upper_bound(times, time);
                const auto i = std::clamp<std::size_t>(next - times.begin(), 1, times.size() - 1);
                const auto t = std::clamp((time - times[i - 1]) / (times[i] - times[i - 1]), 0.0f, 1.0f);
                const auto blend = [&](const glm::mat4& a, const glm::mat4& b) {
                    const auto rotation = glm::slerp(glm::quat_cast(glm::mat3(a)), glm::quat_cast(glm::mat3(b)), t);
                    return glm::translate(glm::mat4(1.0f), glm::mix(glm::vec3(a[3]), glm::vec3(b[3]), t)) *
                           glm::mat4_cast(rotation);
                };
                const auto& a = bones[i - 1];
                const auto& b = bones[i];
                ReloadLogic::ReloadBones blended{.leftWrist = blend(a.leftWrist, b.leftWrist),
                                                 .rightWrist = blend(a.rightWrist, b.rightWrist),
                                                 .gun = blend(a.gun, b.gun)};
                if (a.handTag && b.handTag)
                    blended.handTag = blend(*a.handTag, *b.handTag);
                return blended;
            };
        }

        ReloadLogic::ProfileInput Input() const
        {
            return {.animation = name, .duration = duration, .notetracks = notetracks, .rig = rig, .bones = Lookup()};
        }
    };

    Recorded ReadRecorded(const std::string& name)
    {
        const auto json = ReadJson(DATA / "reload-poses" / (name + ".json"));
        Recorded recorded{.name = name,
                          .duration = json["duration"].get<float>(),
                          .notetracks = ReadNotetracks(json["notetracks"])};
        if (json.contains("rig"))
            recorded.rig = Types::WeaponRig{.magazineBone = json["rig"]["magazineBone"].get<std::string>(),
                                            .magazine = ReadTransform(json["rig"]["magazine"])};
        recorded.times = json["times"].get<std::vector<float>>();
        const auto& bones = json["bones"];
        for (std::size_t i = 0; i < recorded.times.size(); ++i)
        {
            ReloadLogic::ReloadBones at{.leftWrist = ReadTransform(bones["j_wrist_le"][i]),
                                        .rightWrist = ReadTransform(bones["j_wrist_ri"][i]),
                                        .gun = ReadTransform(bones["tag_weapon_right"][i])};
            if (bones.contains("tag_inhand"))
                at.handTag = ReadTransform(bones["tag_inhand"][i]);
            recorded.bones.push_back(at);
        }
        return recorded;
    }

    // the AK-47's world model's magazine, as recorded
    Types::WeaponRig AkRig()
    {
        return {.magazineBone = "tag_clip",
                .magazine = glm::translate(glm::mat4(1.0f), glm::vec3(-4.9599f, 0.015f, -1.0294f))};
    }

    float Distance(const glm::mat4& a, const glm::mat4& b)
    {
        return PoseLogic::GetTransformDistance(a, b, 10.0f);
    }

    // how far the magazine a hand holds with a grip is from where the gun has it at a time
    float GripError(const Recorded& recorded, const Types::ReloadProfile& profile, const Types::ReloadGrip& grip,
                    float time)
    {
        const auto bones = recorded.Lookup()(time);
        const auto hand = ReloadLogic::GetHand(*bones, profile.hand);
        return Distance(*hand * grip.magazine, ReloadLogic::GetMagazineOnGun(*bones, *recorded.rig));
    }

    bool InOrder(const Types::ReloadProfile& profile)
    {
        const auto before = [](const std::optional<Types::ReloadMoment>& a,
                               const std::optional<Types::ReloadMoment>& b) { return !a || !b || a->time < b->time; };
        return before(profile.out, profile.swap) && before(profile.swap, profile.in) && before(profile.out, profile.in);
    }
}  // namespace

TEST("every reload animation's notetracks give a profile of its kind, in order")
{
    const auto all = ReadJson(DATA / "reload-notetracks.json");
    CHECK(all.size() == 75);

    // by what the notetracks say: SP clip notetracks with a drop clip, or an attach clip without one, sounds of
    // magazines, LMG boxes, shells (MP sounds or SP load_shell) and rockets; the rest say nothing (with no poses to go
    // by here, neither do they)
    const std::map<ReloadKind, std::vector<std::string>> kinds{
        {ReloadKind::DroppedMagazine,
         {"ai_cqb_run_f_reload",
          "ai_cqb_walk_f_reload",
          "ai_pillar_crouch_reload_01",
          "corner_standl_reload_b_2_alert",
          "corner_standl_reload_v1",
          "corner_standr_reload_b_2_alert",
          "corner_standr_reload_v1",
          "cornercrr_reloada",
          "cornercrr_reloadb",
          "covercrouch_reload_hide",
          "coverstand_reloada",
          "cqb_stand_reload_knee",
          "cqb_stand_reload_steady",
          "exposed_crouch_reload",
          "exposed_reload",
          "exposed_reloadb",
          "heat_exposed_reload",
          "pistol_stand_reload_a",
          "prone_reload",
          "pt_laststand_reload",
          "pt_reload_stand_pistol",
          "run_lowready_reload"}},
        {ReloadKind::Magazine,
         {"ai_pillar_crouch_reload_02", "ai_pillar_crouch_reload_03", "cornercrl_reloada", "cornercrl_reloadb",
          "rpg_crouch_reload", "rpg_stand_reload", "pt_dw_botharms_reload", "pt_prone_dw_botharms_reload",
          "pt_reload_crouch_pistol", "pt_reload_crouch_rifle", "pt_reload_crouchwalk", "pt_reload_crouchwalk_pistol",
          "pt_reload_prone_pistol", "pt_reload_stand_auto", "pt_reload_stand_auto_mp40", "pt_reload_stand_rifle",
          "pt_rifle_crouch_reload_rearclip", "pt_rifle_prone_reload_rearclip", "pt_rifle_stand_reload_rearclip"}},
        {ReloadKind::Box, {"pt_lmg_crouch_reload", "pt_lmg_prone_reload", "pt_lmg_stand_reload"}},
        {ReloadKind::Shells,
         {"pt_shot_crouch_reload", "pt_shot_prone_reload", "pt_shot_stand_reload", "shotgun_crouch_reload",
          "shotgun_stand_reload_a", "shotgun_stand_reload_b", "shotgun_stand_reload_c"}},
        {ReloadKind::Rocket, {"pt_reload_prone_rpg", "pt_reload_stand_rpg"}},
    };
    std::size_t counted = 0;
    for (const auto& [name, entry] : all.items())
    {
        const auto notetracks = ReadNotetracks(entry["notetracks"]);
        const auto profile = ReloadLogic::MakeProfile(
            {.animation = name, .duration = entry["duration"].get<float>(), .notetracks = notetracks, .rig = AkRig()});
        auto expected = ReloadKind::None;
        for (const auto& [kind, names] : kinds)
            if (std::ranges::find(names, name) != names.end())
                expected = kind;
        counted += expected != ReloadKind::None ? 1 : 0;
        if (profile.kind != expected)
            Test::Fail(__FILE__, __LINE__,
                       std::format("{} is {}, expected {}", name, ReloadLogic::GetKindName(profile.kind),
                                   ReloadLogic::GetKindName(expected)));
        CHECK(InOrder(profile));
        if (profile.kind == ReloadKind::Shells)
            CHECK(profile.inserts.size() >= 3 && profile.inserts.size() <= 6);
        // the notetracks alone give no grips
        CHECK(!profile.oldGrip && !profile.newGrip && !ReloadLogic::GetMoments(profile));
    }
    CHECK(counted == 53);
}

TEST("notetracks give moments by class: SP clip notetracks, magazine sounds, the LMG's box")
{
    const auto all = ReadJson(DATA / "reload-notetracks.json");
    const auto clip = ReloadLogic::FindClipMoments(ReadNotetracks(all["pt_reload_stand_pistol"]["notetracks"]));
    CHECK(clip && clip->kind == ReloadKind::DroppedMagazine && clip->source == ReloadSource::ClipNotetracks);
    CHECK_NEAR(*clip->out, 0.02, 1e-5);
    CHECK_NEAR(*clip->swap, 0.26, 1e-5);
    CHECK_NEAR(*clip->in, 0.42, 1e-5);
    // a detach before anything is attached does nothing, as in SP scripts
    const auto corner = ReloadLogic::FindClipMoments(ReadNotetracks(all["cornercrl_reloada"]["notetracks"]));
    CHECK(corner && corner->kind == ReloadKind::Magazine && !corner->swap);
    CHECK_NEAR(*corner->out, 0.40678, 1e-4);
    CHECK_NEAR(*corner->in, 0.72881, 1e-4);

    const auto sounds = ReloadLogic::FindSoundMoments(ReadNotetracks(all["pt_reload_stand_auto"]["notetracks"]));
    CHECK(sounds && sounds->kind == ReloadKind::Magazine && sounds->source == ReloadSource::SoundNotetracks);
    CHECK_NEAR(*sounds->out, 0.014, 1e-3);
    CHECK_NEAR(*sounds->in, 0.42, 1e-3);
    CHECK(!ReloadLogic::FindClipMoments(ReadNotetracks(all["pt_reload_stand_auto"]["notetracks"])));

    // the stand reload opens the cover where the prone one sounds the box coming off
    const auto stand = ReloadLogic::FindSoundMoments(ReadNotetracks(all["pt_lmg_stand_reload"]["notetracks"]));
    const auto prone = ReloadLogic::FindSoundMoments(ReadNotetracks(all["pt_lmg_prone_reload"]["notetracks"]));
    CHECK(stand && stand->kind == ReloadKind::Box && prone && prone->kind == ReloadKind::Box);
    CHECK(stand->out && prone->out && *stand->out < *stand->in && *prone->out < *prone->in);
    CHECK_NEAR(*prone->out, 0.272, 1e-3);
    CHECK(!ReloadLogic::FindSoundMoments(ReadNotetracks(all["pt_g11_stand_reload"]["notetracks"])));
}

TEST("geometry alone finds a rifle reload where its sounds are")
{
    for (const auto& [name, out, in] :
         {std::tuple("pt_reload_stand_auto", 0.014f, 0.42f), std::tuple("pt_reload_crouch_rifle", 0.017f, 0.39f)})
    {
        auto recorded = ReadRecorded(name);
        recorded.notetracks.clear();
        const auto profile = ReloadLogic::MakeProfile(recorded.Input());
        CHECK(profile.kind == ReloadKind::Magazine && profile.kindSource == ReloadSource::Geometry);
        CHECK(profile.hand == ReloadHand::LeftWrist);
        CHECK(profile.out && profile.out->source == ReloadSource::Geometry);
        CHECK(profile.in && profile.in->source == ReloadSource::Geometry);
        CHECK(profile.swap && profile.swap->source == ReloadSource::Geometry);
        // where the hand is nearest the magazine around its trip, within 70 ms of the sounds
        CHECK_NEAR(profile.out->time, out, 0.03);
        CHECK_NEAR(profile.in->time, in, 0.03);
        CHECK(InOrder(profile));
    }
}

TEST(
    "a rifle reload takes its moments from its sounds, the swap from where the hand goes, and holds the magazine as it "
    "was on the gun")
{
    const auto recorded = ReadRecorded("pt_reload_stand_auto");
    const auto profile = ReloadLogic::MakeProfile(recorded.Input());
    CHECK(profile.kind == ReloadKind::Magazine && profile.kindSource == ReloadSource::SoundNotetracks);
    CHECK(profile.hand == ReloadHand::LeftWrist && profile.handSource == ReloadSource::Geometry);
    CHECK(profile.out && profile.out->source == ReloadSource::SoundNotetracks);
    CHECK(profile.in && profile.in->source == ReloadSource::SoundNotetracks);
    CHECK_NEAR(profile.out->time, 0.014, 1e-3);
    CHECK_NEAR(profile.in->time, 0.42, 1e-3);
    // the farthest of its two trips to the pouch (at 0.14 and 0.32)
    CHECK(profile.swap && profile.swap->source == ReloadSource::Geometry);
    CHECK_NEAR(profile.swap->time, 0.32, 0.01);
    CHECK(profile.magazineBone == "tag_clip");
    CHECK(profile.note.empty());

    // on the gun at out and in
    CHECK(profile.oldGrip && profile.oldGrip->kind == Types::ReloadGrip::Kind::Measured);
    CHECK(profile.newGrip && profile.newGrip->kind == Types::ReloadGrip::Kind::Measured);
    CHECK(GripError(recorded, profile, *profile.oldGrip, profile.out->time) < 1e-3f);
    CHECK(GripError(recorded, profile, *profile.newGrip, profile.in->time) < 1e-3f);
    // and off it in between, held as the hand pulls it away
    CHECK(GripError(recorded, profile, *profile.oldGrip, 0.1f) > 5.0f);
    CHECK(GripError(recorded, profile, *profile.newGrip, 0.38f) > 5.0f);

    // the old one falls from the hand at the swap, as fast as the hand moves
    CHECK(profile.fall && !profile.fall->fromGun);
    CHECK_NEAR(profile.fall->time, profile.swap->time, 1e-6);
    const auto atSwap = recorded.Lookup()(profile.swap->time);
    CHECK(Distance(profile.fall->magazine, atSwap->leftWrist * profile.oldGrip->magazine) < 1e-3f);
    CHECK(glm::length(profile.fall->velocity) < 200.0f && glm::length(profile.fall->push) == 0.0f);

    const auto moments = ReloadLogic::GetMoments(profile);
    CHECK(moments && !moments->dropped && moments->swap);
    CHECK_NEAR(moments->out, profile.out->time, 1e-6);
}

TEST("a reload without notetracks takes its moments from where the hand goes")
{
    const auto recorded = ReadRecorded("pt_reload_prone_auto");
    CHECK(recorded.notetracks.size() == 1);  // only "end"
    const auto profile = ReloadLogic::MakeProfile(recorded.Input());
    CHECK(profile.kind == ReloadKind::Magazine && profile.kindSource == ReloadSource::Geometry);
    CHECK(profile.out && profile.swap && profile.in);
    // the hand at the magazine before and after its trip to the pouch at 0.455
    CHECK_NEAR(profile.out->time, 0.256, 0.01);
    CHECK_NEAR(profile.swap->time, 0.455, 0.01);
    CHECK_NEAR(profile.in->time, 0.583, 0.01);
    CHECK(GripError(recorded, profile, *profile.oldGrip, profile.out->time) < 1e-3f);
    CHECK(GripError(recorded, profile, *profile.newGrip, profile.in->time) < 1e-3f);
    CHECK(ReloadLogic::GetMoments(profile));

    // the G11's reload says nothing either, and its world model has no magazine bone to move
    const auto g11 = ReloadLogic::MakeProfile(ReadRecorded("pt_g11_stand_reload").Input());
    CHECK(g11.kind == ReloadKind::None && g11.magazineBone.empty() && !g11.note.empty());
}

TEST("only a reload is worked out from where the hand goes: an animation named otherwise isn't one, a death never")
{
    // the prone reload says nothing in its notetracks: its name and the hand's trip make it one
    const auto recorded = ReadRecorded("pt_reload_prone_auto");
    CHECK(ReloadLogic::MakeProfile(recorded.Input()).kind == ReloadKind::Magazine);

    // the same hand's trip in an animation named otherwise is no reload (the guess every clip got)
    auto input = recorded.Input();
    input.animation = "pb_crouch_death_headshot_front";
    auto profile = ReloadLogic::MakeProfile(input);
    CHECK(profile.kind == ReloadKind::None && !ReloadLogic::SaysReload(profile) && !profile.note.empty());
    CHECK(!profile.out && !profile.in && !ReloadLogic::GetMoments(profile));

    // unless its notetracks or the user say so
    auto sounds = ReadRecorded("pt_reload_stand_auto");
    auto said = sounds.Input();
    said.animation = "pt_stand_shoot_auto";
    CHECK(ReloadLogic::MakeProfile(said).kind == ReloadKind::Magazine);
    const ReloadLogic::ProfileEntry magazine{.kind = ReloadKind::Magazine};
    input.override = &magazine;
    profile = ReloadLogic::MakeProfile(input);
    CHECK(profile.kind == ReloadKind::Magazine && profile.out && profile.in);

    // a death never, whatever its name, notetracks or the table say, unless the user says so
    const ReloadLogic::ProfileEntry curated{.kind = ReloadKind::Magazine, .out = 0.1f, .in = 0.5f};
    said.animation = "pt_reload_stand_auto";
    said.death = true;
    said.curated = &curated;
    profile = ReloadLogic::MakeProfile(said);
    CHECK(profile.kind == ReloadKind::None && !ReloadLogic::SaysReload(profile) && !ReloadLogic::GetMoments(profile));
    said.override = &magazine;
    CHECK(ReloadLogic::MakeProfile(said).kind == ReloadKind::Magazine);

    CHECK(ReloadLogic::IsReloadName("pt_reload_stand_auto") && ReloadLogic::IsReloadName("cornercrl_reloada"));
    CHECK(!ReloadLogic::IsReloadName("ai_viet_exposed_crouch_death_fetal"));
}

TEST("a user's Actor 3 dies without a reload: its left hand flails by the magazine, which the hand's path read as one")
{
    // recorded on intro_cinematic: the death of Actor 3's body holding an AK-47
    const auto recorded = ReadRecorded("ai_viet_exposed_crouch_death_fetal");
    auto input = recorded.Input();
    input.death = true;
    const auto profile = ReloadLogic::MakeProfile(input);
    CHECK(profile.kind == ReloadKind::None && !ReloadLogic::SaysReload(profile) && !ReloadLogic::GetMoments(profile));
    // as every clip was worked out before: a magazine pulled out by the left hand at 0.025 and put back at the end
    input.animation = "reload";
    input.death = false;
    const auto guessed = ReloadLogic::MakeProfile(input);
    CHECK(guessed.kind == ReloadKind::Magazine && guessed.kindSource == ReloadSource::Geometry);
    CHECK(guessed.hand == ReloadHand::LeftWrist && guessed.out && guessed.in);
    CHECK_NEAR(guessed.out->time, 0.025, 0.01);
    // its name alone, without the death, is no reload either
    input.animation = "ai_viet_exposed_crouch_death_fetal";
    CHECK(ReloadLogic::MakeProfile(input).kind == ReloadKind::None);
}

TEST(
    "an SP reload drops the magazine from the gun, with the wrist holding the new one where tag_inhand stays on the "
    "gun")
{
    const auto recorded = ReadRecorded("exposed_reload");
    const auto profile = ReloadLogic::MakeProfile(recorded.Input());
    CHECK(profile.kind == ReloadKind::DroppedMagazine && profile.kindSource == ReloadSource::ClipNotetracks);
    CHECK(profile.hand == ReloadHand::LeftWrist);
    CHECK(profile.out && profile.swap && profile.in);
    CHECK(profile.out->source == ReloadSource::ClipNotetracks && profile.swap->source == ReloadSource::ClipNotetracks);
    CHECK_NEAR(profile.out->time, 0.05, 1e-3);
    CHECK_NEAR(profile.swap->time, 0.225, 1e-3);
    CHECK_NEAR(profile.in->time, 0.625, 1e-3);
    CHECK(!profile.oldGrip && profile.newGrip);
    CHECK(GripError(recorded, profile, *profile.newGrip, profile.in->time) < 1e-3f);

    // it falls from where it is on the gun, pushed out of it
    CHECK(profile.fall && profile.fall->fromGun);
    CHECK_NEAR(profile.fall->time, profile.out->time, 1e-6);
    const auto atOut = recorded.Lookup()(profile.out->time);
    CHECK(Distance(profile.fall->magazine, ReloadLogic::GetMagazineOnGun(*atOut, *recorded.rig)) < 1e-3f);
    CHECK_NEAR(glm::length(profile.fall->push), ReloadLogic::MAGAZINE_PUSH, 1e-3);
    CHECK(ReloadLogic::GetMoments(profile)->dropped);

    const auto pistol = ReloadLogic::MakeProfile(ReadRecorded("pistol_stand_reload_a").Input());
    CHECK(pistol.kind == ReloadKind::DroppedMagazine && pistol.hand == ReloadHand::LeftWrist);
    const auto mp = ReloadLogic::MakeProfile(ReadRecorded("pt_reload_stand_pistol").Input());
    CHECK(mp.kind == ReloadKind::DroppedMagazine && mp.hand == ReloadHand::LeftWrist && mp.newGrip);
}

TEST("tag_inhand holds the magazine where it moves with the hand, and a magazine the hand isn't at falls")
{
    // the mirrored corner reload says right, moves tag_inhand with the left wrist, and "attaches" a clip while the
    // hand is away from the gun
    const auto recorded = ReadRecorded("cornercrl_reloada");
    const auto profile = ReloadLogic::MakeProfile(recorded.Input());
    CHECK(profile.hand == ReloadHand::HandTag && profile.handSource == ReloadSource::Geometry);
    CHECK(profile.kind == ReloadKind::DroppedMagazine && profile.kindSource == ReloadSource::Geometry);
    CHECK(profile.out && profile.out->source == ReloadSource::ClipNotetracks);
    CHECK(profile.in && profile.in->source == ReloadSource::ClipNotetracks);
    CHECK(profile.newGrip && GripError(recorded, profile, *profile.newGrip, profile.in->time) < 1e-3f);
    // not the SP clip model's own grip: the MP gun's magazine is elsewhere
    CHECK(profile.newGrip->kind == Types::ReloadGrip::Kind::Measured);

    std::vector<ReloadLogic::ReloadBones> carried;
    for (auto time = 0.45f; time < 0.7f; time += 0.02f)
        carried.push_back(*recorded.Lookup()(time));
    CHECK(ReloadLogic::MovesWith(carried, ReloadHand::LeftWrist));
    CHECK(!ReloadLogic::MovesWith(carried, ReloadHand::RightWrist));

    // MP animations keep tag_inhand on the gun, which moves with the right hand holding it: not in a hand
    const auto rifle = ReadRecorded("pt_reload_stand_auto");
    std::vector<ReloadLogic::ReloadBones> held;
    for (auto time = 0.02f; time < 0.42f; time += 0.02f)
        held.push_back(*rifle.Lookup()(time));
    CHECK(!ReloadLogic::MovesWith(held, ReloadHand::RightWrist) &&
          !ReloadLogic::MovesWith(held, ReloadHand::LeftWrist));

    // a magazine out sound while the hand is away: it falls from the gun
    auto away = ReadRecorded("pt_reload_stand_auto");
    away.notetracks = {{"sndnt#fly_assault_reload_npc_mag_out", 0.2f}, {"sndnt#fly_assault_reload_npc_mag_in", 0.42f}};
    const auto dropped = ReloadLogic::MakeProfile(away.Input());
    CHECK(dropped.kind == ReloadKind::DroppedMagazine && dropped.kindSource == ReloadSource::Geometry);
    CHECK(dropped.fall && dropped.fall->fromGun && !dropped.oldGrip);
}

TEST("an animation the game can't decode keeps what its notetracks say, without grips")
{
    // reload profiles work out no hands for reloads of 256 frames or more: the LMG reloads
    const auto lmg = ReadRecorded("pt_lmg_stand_reload");
    CHECK(lmg.times.empty());
    const auto box = ReloadLogic::MakeProfile(lmg.Input());
    CHECK(box.kind == ReloadKind::Box && box.out && box.in && !box.newGrip && !box.note.empty());
    CHECK(!ReloadLogic::GetMoments(box));

    auto rifle = ReadRecorded("pt_reload_stand_auto");
    auto input = rifle.Input();
    input.bones = [](float) -> std::optional<ReloadLogic::ReloadBones> { return std::nullopt; };
    const auto undecoded = ReloadLogic::MakeProfile(input);
    CHECK(undecoded.kind == ReloadKind::Magazine && undecoded.out && undecoded.in && !undecoded.swap);
    CHECK(!undecoded.oldGrip && !undecoded.newGrip && !undecoded.fall);
    CHECK(undecoded.note.find("256") != std::string::npos);
    CHECK(!ReloadLogic::GetMoments(undecoded));

    // nor with no gun, or one without a magazine of its own
    input = rifle.Input();
    input.rig.reset();
    CHECK(ReloadLogic::MakeProfile(input).kind == ReloadKind::None);
    input.rig = Types::WeaponRig{.model = "t5_weapon_m202_world"};
    const auto noMagazine = ReloadLogic::MakeProfile(input);
    CHECK(noMagazine.kind == ReloadKind::None && noMagazine.kindSource == ReloadSource::None &&
          !noMagazine.note.empty());
}

TEST("the user's override comes first, then the curated table, the notetracks and geometry")
{
    const auto recorded = ReadRecorded("pt_reload_stand_auto");
    const ReloadLogic::ProfileEntry override{.swap = 0.2f};
    const ReloadLogic::ProfileEntry curated{.out = 0.03f, .swap = 0.25f};
    auto input = recorded.Input();
    input.override = &override;
    input.curated = &curated;
    auto profile = ReloadLogic::MakeProfile(input);
    CHECK(profile.out && profile.out->source == ReloadSource::Curated);
    CHECK_NEAR(profile.out->time, 0.03, 1e-6);
    CHECK(profile.swap && profile.swap->source == ReloadSource::Override);
    CHECK_NEAR(profile.swap->time, 0.2, 1e-6);
    CHECK(profile.in && profile.in->source == ReloadSource::SoundNotetracks);
    CHECK(GripError(recorded, profile, *profile.oldGrip, profile.out->time) < 1e-3f);

    // the swap from geometry between where the table puts in and the sound out
    const ReloadLogic::ProfileEntry early{.in = 0.3f};
    input.override = nullptr;
    input.curated = &early;
    profile = ReloadLogic::MakeProfile(input);
    CHECK(profile.out->source == ReloadSource::SoundNotetracks && profile.in->source == ReloadSource::Curated);
    CHECK(profile.swap && profile.swap->source == ReloadSource::Geometry);
    CHECK_NEAR(profile.swap->time, 0.14, 0.02);

    // a moment that doesn't come in order with those before is left out
    const ReloadLogic::ProfileEntry late{.out = 0.5f};
    input.curated = &late;
    profile = ReloadLogic::MakeProfile(input);
    CHECK(profile.out->source == ReloadSource::Curated && (!profile.in || profile.in->time > 0.5f));

    // turned off, or held in the other hand
    const ReloadLogic::ProfileEntry off{.kind = ReloadKind::None};
    input.curated = nullptr;
    input.override = &off;
    profile = ReloadLogic::MakeProfile(input);
    CHECK(profile.kind == ReloadKind::None && profile.kindSource == ReloadSource::Override);
    CHECK(!ReloadLogic::GetMoments(profile));
    const ReloadLogic::ProfileEntry right{.hand = ReloadHand::RightWrist};
    input.override = &right;
    profile = ReloadLogic::MakeProfile(input);
    CHECK(profile.hand == ReloadHand::RightWrist && profile.handSource == ReloadSource::Override);
    CHECK(GripError(recorded, profile, *profile.newGrip, profile.in->time) < 1e-3f);
}

TEST("profile entries read and write as json, and the curated table that comes with the mod reads")
{
    const ReloadLogic::ProfileEntry entry{
        .kind = ReloadKind::DroppedMagazine, .hand = ReloadHand::HandTag, .out = 0.1f, .in = 0.6f};
    const auto json = ReloadLogic::EntryToJson(entry);
    CHECK(json["kind"] == "dropped" && json["hand"] == "tag" && !json.contains("swap"));
    CHECK(ReloadLogic::EntryFromJson(json) == entry);
    // unknown names are left out, moments kept from 0 to 1
    const auto odd = ReloadLogic::EntryFromJson({{"kind", "belt"}, {"hand", 3}, {"swap", 1.5}});
    CHECK(!odd.kind && !odd.hand && odd.swap == 1.0f);
    CHECK(ReloadLogic::EntryFromJson(nlohmann::json::array()) == ReloadLogic::ProfileEntry{});

    const auto table = ReloadLogic::TableFromJson(
        {{"profiles", {{"pt_reload_stand_auto", {{"swap", 0.3}}}, {"exposed_reload", {{"kind", "none"}}}}}});
    CHECK(table.size() == 2 && table.at("pt_reload_stand_auto").swap == 0.3f);
    CHECK(table.at("exposed_reload").kind == ReloadKind::None);
    CHECK(ReloadLogic::TableFromJson(nlohmann::json::array()).empty());

    const auto shipped = ReadJson(SHIPPED_TABLE);
    CHECK(shipped.contains("profiles") && shipped["profiles"].is_object());
    for (const auto& [name, value] : ReloadLogic::TableFromJson(shipped))
        CHECK(ReloadLogic::EntryToJson(value).size() == shipped["profiles"][name].size());
}

TEST("the curated table names recorded reload animations, and their profiles say what it says, in order")
{
    // (filled by the harness sweep)
    const auto all = ReadJson(DATA / "reload-notetracks.json");
    const auto table = ReloadLogic::TableFromJson(ReadJson(SHIPPED_TABLE));
    CHECK(!table.empty());
    for (const auto& [name, entry] : table)
    {
        if (!all.contains(name))
        {
            Test::Fail(__FILE__, __LINE__, name + " isn't a recorded reload animation");
            continue;
        }
        const auto notetracks = ReadNotetracks(all[name]["notetracks"]);
        const auto profile = ReloadLogic::MakeProfile({.animation = name,
                                                       .duration = all[name]["duration"].get<float>(),
                                                       .notetracks = notetracks,
                                                       .curated = &entry,
                                                       .rig = AkRig()});
        if (entry.kind && (profile.kind != *entry.kind || profile.kindSource != ReloadSource::Curated))
            Test::Fail(
                __FILE__, __LINE__,
                std::format("{} is {} ({}), the table says {}", name, ReloadLogic::GetKindName(profile.kind),
                            ReloadLogic::GetSourceName(profile.kindSource), ReloadLogic::GetKindName(*entry.kind)));
        if (entry.hand)
            CHECK(profile.hand == *entry.hand && profile.handSource == ReloadSource::Curated);
        for (const auto& [said, moment] :
             {std::pair(entry.out, profile.out), {entry.swap, profile.swap}, {entry.in, profile.in}})
            if (said)
                CHECK(moment && moment->source == ReloadSource::Curated && moment->time == *said);
        CHECK(InOrder(profile));
    }
    // (the SP launcher reloads' clip notetracks are a rocket's)
    CHECK(table.at("rpg_stand_reload").kind == ReloadKind::Rocket);
}

TEST("the curated dual wield reload holds the new magazine with the hand on the gun, which takes it at the hip")
{
    const auto recorded = ReadRecorded("pt_dw_botharms_reload");
    // On its own: the magazine falls as the reload starts (the left hand isn't at it), and the left hand, which holds
    // the other gun and never comes to this one's magazine, holds the new one over 20 units from it until it goes in
    const auto found = ReloadLogic::MakeProfile(recorded.Input());
    CHECK(found.kind == ReloadKind::DroppedMagazine && found.kindSource == ReloadSource::Geometry);
    CHECK(found.hand == ReloadHand::LeftWrist && found.newGrip);
    CHECK(glm::length(glm::vec3(found.newGrip->magazine[3])) > 20.0f);

    // As the table has it: held with the right hand, which holds the gun, from where the gun reaches the hip
    const auto table = ReloadLogic::TableFromJson(ReadJson(SHIPPED_TABLE));
    auto input = recorded.Input();
    input.curated = &table.at("pt_dw_botharms_reload");
    const auto profile = ReloadLogic::MakeProfile(input);
    CHECK(profile.kind == ReloadKind::DroppedMagazine && profile.kindSource == ReloadSource::Curated);
    CHECK(profile.hand == ReloadHand::RightWrist && profile.handSource == ReloadSource::Curated);
    CHECK(profile.out && profile.out->source == ReloadSource::SoundNotetracks);
    CHECK(profile.swap && profile.swap->source == ReloadSource::Curated);
    CHECK(profile.in && profile.in->source == ReloadSource::SoundNotetracks);
    CHECK_NEAR(profile.out->time, 0.016, 1e-3);
    CHECK_NEAR(profile.in->time, 0.689, 1e-3);
    CHECK(profile.newGrip && glm::length(glm::vec3(profile.newGrip->magazine[3])) < 10.0f);
    // so from the swap on the new one is where the gun has it
    for (auto time = profile.swap->time; time <= profile.in->time; time += 0.02f)
        CHECK(GripError(recorded, profile, *profile.newGrip, time) < 0.5f);
    // and the old one falls from the gun as the reload starts
    CHECK(profile.fall && profile.fall->fromGun);
    CHECK_NEAR(profile.fall->time, profile.out->time, 1e-6);
    const auto moments = ReloadLogic::GetMoments(profile);
    CHECK(moments && moments->dropped && moments->swap);
}

TEST("a profile's fall is placed where the actor stands, as fast as its clip plays")
{
    Types::ReloadProfile profile;
    profile.fall = Types::ReloadFall{.time = 0.1f,
                                     .magazine = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 50.0f)),
                                     .velocity = {20.0f, 0.0f, 0.0f},
                                     .push = {0.0f, 0.0f, -40.0f},
                                     .spin = 300.0f,
                                     .fromGun = true};
    const auto rig = AkRig();
    // facing left (+y), at (100, 0, 0)
    const auto placement = glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 0.0f, 0.0f)) *
                           glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1));
    std::vector<glm::vec3> asked;
    const auto fall = ReloadLogic::PlaceFall(profile, rig, placement, 2.0f, [&](glm::vec3 point) {
        asked.push_back(point);
        return -10.0f;
    });
    CHECK(fall.has_value());
    // the magazine bone on the gun model's: the model lies where the bone is less its place in the gun
    CHECK(Distance(fall->start, placement * profile.fall->magazine * glm::inverse(rig.magazine)) < 1e-4f);
    CHECK_NEAR(fall->velocity.x, 0.0, 1e-3);
    CHECK_NEAR(fall->velocity.y, 40.0, 1e-3);
    CHECK_NEAR(fall->velocity.z, -40.0, 1e-3);
    CHECK(fall->ground == -10.0f && asked.size() == 2);
    // the hand's motion it falls with is kept to a speed, the push isn't
    const auto fast = ReloadLogic::PlaceFall(profile, rig, glm::mat4(1.0f), 100.0f, [](glm::vec3) { return 0.0f; });
    CHECK_NEAR(fast->velocity.x, ReloadLogic::MAX_MAGAZINE_SPEED, 1e-3);
    CHECK_NEAR(fast->velocity.z, -40.0, 1e-3);
    // Where the game draws the magazine bone as it starts to fall (the legs under an upper body clip lower the gun 6
    // units and turn it a quarter left): it starts there, its motion turned the same
    const auto turned = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1));
    const auto drawn =
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -6.0f)) * placement * turned * profile.fall->magazine;
    const auto onDrawn = ReloadLogic::PlaceFall(profile, rig, placement, 2.0f, [](glm::vec3) { return -10.0f; }, drawn);
    CHECK(onDrawn.has_value());
    CHECK(Distance(onDrawn->start, drawn * glm::inverse(rig.magazine)) < 1e-4f);
    // facing back (-x) now: the hand's motion (forward) goes back, the push still down
    CHECK_NEAR(onDrawn->velocity.x, -40.0, 1e-3);
    CHECK_NEAR(onDrawn->velocity.y, 0.0, 1e-3);
    CHECK_NEAR(onDrawn->velocity.z, -40.0, 1e-3);
    profile.fall.reset();
    CHECK(!ReloadLogic::PlaceFall(profile, rig, placement, 1.0f, [](glm::vec3) { return 0.0f; }));
}

TEST("a trip of the hand is found on a curve, and none where the hand doesn't go far or doesn't come back")
{
    ReloadLogic::Curve curve;
    const std::vector<float> distances{4, 4, 6, 12, 20, 28, 20, 12, 7, 5, 5, 6, 5};
    for (std::size_t i = 0; i < distances.size(); ++i)
    {
        curve.times.push_back(static_cast<float>(i) / 12.0f);
        curve.distances.push_back(distances[i]);
    }
    const auto trip = ReloadLogic::FindTrip(curve);
    CHECK(trip.has_value());
    CHECK_NEAR(trip->swap, 5.0 / 12.0, 1e-6);
    // the last nearest point before (of the two at 4), the first after (not the later 5)
    CHECK_NEAR(trip->out, 1.0 / 12.0, 1e-6);
    CHECK_NEAR(trip->in, 9.0 / 12.0, 1e-6);
    CHECK_NEAR(*ReloadLogic::FindFarthest(curve, 0.0f, 4.5f / 12.0f), 4.0 / 12.0, 1e-6);
    CHECK(!ReloadLogic::FindFarthest(curve, 0.1f, 0.12f));

    auto low = curve;
    for (auto& distance : low.distances)
        distance = std::min(distance, 14.0f);
    CHECK(!ReloadLogic::FindTrip(low));
    auto gone = curve;
    for (std::size_t i = 6; i < gone.distances.size(); ++i)
        gone.distances[i] = 20.0f;
    CHECK(!ReloadLogic::FindTrip(gone));
}

TEST("a clip maps a profile's moments to its ticks")
{
    Types::Actor actor;
    Types::ActorClip clip{.id = 1, .animation = "reload", .start = 1000, .length = 4000, .loop = false};
    actor.clips.push_back(clip);
    const ActorLogic::AnimationLookup animations =
        [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
        if (name != "reload")
            return std::nullopt;
        return ActorLogic::AnimationInfo{.duration = 2.0f};
    };
    const ActorLogic::ReloadMomentsLookup moments = [](const Types::ActorClip&) {
        return std::optional(ActorLogic::ReloadMoments{.out = 0.1f, .in = 0.6f, .swap = 0.3f, .dropped = true});
    };
    auto reloads = ActorLogic::GetReloads(actor, 10000, animations, moments);
    CHECK(reloads.size() == 1);
    CHECK(reloads[0].out == 1200 && reloads[0].swap == 1600u && reloads[0].in == 2200 && reloads[0].dropped);
    actor.clips[0].speed = 2.0f;
    reloads = ActorLogic::GetReloads(actor, 10000, animations, moments);
    CHECK(reloads[0].out == 1100 && reloads[0].swap == 1300u && reloads[0].in == 1600);
}

namespace
{
    // An actor playing a recorded reload in a loop from a tick, for four plays, with its profile
    struct Reloading
    {
        Recorded recorded;
        std::shared_ptr<const Types::ReloadProfile> profile;
        Types::Actor actor;
        std::vector<ActorLogic::Reload> reloads;

        explicit Reloading(const std::string& name)
            : recorded(ReadRecorded(name)),
              profile(std::make_shared<const Types::ReloadProfile>(ReloadLogic::MakeProfile(recorded.Input())))
        {
            const auto play = static_cast<std::uint32_t>(std::round(recorded.duration * 1000.0f));
            actor.clips.push_back({.id = 1, .animation = name, .start = 1000, .length = 4 * play, .loop = true});
        }

        // what its magazines do at a tick, with up to three that fell
        ReloadLogic::Magazines At(std::uint32_t tick, bool profiled = true)
        {
            const ActorLogic::AnimationLookup animations = [&](const std::string&) {
                return std::optional(ActorLogic::AnimationInfo{.duration = recorded.duration});
            };
            reloads = ActorLogic::GetReloads(
                actor, tick, animations, [&](const Types::ActorClip&) { return ReloadLogic::GetMoments(*profile); });
            return ReloadLogic::GetMagazines(
                reloads,
                [&](const Types::ActorClip&) {
                    return profiled ? profile : std::shared_ptr<const Types::ReloadProfile>();
                },
                *recorded.rig, tick, 3);
        }

        // how far the magazine a hand holds as the magazines say is from where the gun has it at a time
        float HeldError(const ReloadLogic::Magazines& magazines, float time) const
        {
            const auto bones = recorded.Lookup()(time);
            const auto hand = ReloadLogic::GetHand(*bones, magazines.hand);
            return Distance(*hand * magazines.held * recorded.rig->magazine,
                            ReloadLogic::GetMagazineOnGun(*bones, *recorded.rig));
        }
    };
}  // namespace

TEST("a pulled magazine is drawn in the hand with the profile's grips, and the last few lie where they fell")
{
    using Held = ActorLogic::HeldMagazine;
    Reloading rifle("pt_reload_stand_auto");
    const auto& profile = *rifle.profile;
    rifle.At(UINT32_MAX);
    const auto first = rifle.reloads.at(0);
    CHECK(first.swap && first.out < *first.swap && *first.swap < first.in);

    auto magazines = rifle.At(first.out - 1);
    CHECK(!magazines.state.offGun && magazines.fallen.empty());
    // the old one in the hand from out, where it was on the gun
    magazines = rifle.At(first.out);
    CHECK(magazines.state.offGun && magazines.state.held == Held::Old);
    CHECK(magazines.hand == ReloadHand::LeftWrist && ReloadLogic::GetHandBone(magazines.hand) == "j_wrist_le");
    CHECK(rifle.HeldError(magazines, profile.out->time) < 1e-3f);
    CHECK(magazines.fallen.empty());
    // the new one from the swap, which reaches the gun where it has it at in; the old one falls from there
    magazines = rifle.At((*first.swap + first.in) / 2);
    CHECK(magazines.state.offGun && magazines.state.held == Held::New);
    CHECK(rifle.HeldError(magazines, profile.in->time) < 1e-3f);
    CHECK(magazines.fallen.size() == 1 && magazines.fallen[0].start == *first.swap);
    CHECK(magazines.fallen[0].reload == &rifle.reloads.back() && magazines.fallen[0].profile == rifle.profile);
    magazines = rifle.At(first.in);
    CHECK(!magazines.state.offGun && magazines.state.held == Held::None && magazines.fallen.size() == 1);

    // long after its four plays, the last three that fell, the latest first
    magazines = rifle.At(100000);
    CHECK(!magazines.state.offGun && magazines.fallen.size() == 3);
    CHECK(magazines.fallen.size() == 3 && magazines.fallen[0].start > magazines.fallen[1].start &&
          magazines.fallen[1].start > magazines.fallen[2].start);
    // until the profile is there, the magazine stays on the gun
    magazines = rifle.At(first.out, false);
    CHECK(!magazines.state.offGun && magazines.state.held == Held::None && magazines.fallen.empty());
}

TEST("a dropped magazine falls from the gun at out, and the hand has the new one from the swap")
{
    using Held = ActorLogic::HeldMagazine;
    Reloading sp("exposed_reload");
    sp.At(UINT32_MAX);
    const auto first = sp.reloads.at(0);
    CHECK(first.dropped && first.swap);

    auto magazines = sp.At(first.out);
    CHECK(magazines.state.offGun && magazines.state.held == Held::None);
    CHECK(magazines.fallen.size() == 1 && magazines.fallen[0].start == first.out);
    magazines = sp.At(*first.swap);
    CHECK(magazines.state.held == Held::New && sp.HeldError(magazines, sp.profile->in->time) < 1e-3f);

    // and with tag_inhand where that moves with the hand
    Reloading corner("cornercrl_reloada");
    corner.At(UINT32_MAX);
    const auto cornered = corner.reloads.at(0);
    magazines = corner.At(cornered.in - 1);
    CHECK(magazines.state.held == Held::New && ReloadLogic::GetHandBone(magazines.hand) == "tag_inhand");
    CHECK(corner.HeldError(magazines, corner.profile->in->time) < 1e-3f);
}

TEST("a clip's plays map ticks to points of a play and back, however it loops, starts and plays")
{
    // a 2 s animation from 0.5 s into it at 2x, looping, from tick 1000: a play every second, the first a quarter in
    Types::ActorClip clip{.id = 1, .animation = "reload", .start = 1000, .length = 4000, .speed = 2.0f, .offset = 0.5f};
    CHECK(ActorLogic::GetPlayAt(clip, 2.0f, 1000) == 0);
    CHECK(ActorLogic::GetPlayAt(clip, 2.0f, 1749) == 0);
    CHECK(ActorLogic::GetPlayAt(clip, 2.0f, 1750) == 1);
    CHECK(ActorLogic::GetPlayAt(clip, 2.0f, 4999) == 4);
    // outside the clip, its first
    CHECK(ActorLogic::GetPlayAt(clip, 2.0f, 999) == 0 && ActorLogic::GetPlayAt(clip, 2.0f, 6000) == 0);
    CHECK_NEAR(ActorLogic::GetTimeInPlay(clip, 2.0f, 0, 1000), 0.25, 1e-6);
    CHECK_NEAR(ActorLogic::GetTimeInPlay(clip, 2.0f, 1, 2000), 0.25, 1e-6);
    CHECK_NEAR(ActorLogic::GetTimeInPlay(clip, 2.0f, 1, 1000), -0.75, 1e-6);
    CHECK(ActorLogic::GetTickInPlay(clip, 2.0f, 1, 0.25f) == 2000u);
    CHECK(ActorLogic::GetTickInPlay(clip, 2.0f, 2, 0.5f) == 3250u);
    // before the clip starts there is no tick for it
    CHECK(!ActorLogic::GetTickInPlay(clip, 2.0f, 0, 0.1f));
    // a tick to a point and back is the same tick
    bool same = true;
    for (std::uint32_t tick = 1000; tick < 5000; tick += 7)
    {
        const auto play = ActorLogic::GetPlayAt(clip, 2.0f, tick);
        same = same && ActorLogic::GetTickInPlay(clip, 2.0f, play, ActorLogic::GetTimeInPlay(clip, 2.0f, play, tick)) ==
                           std::optional(tick);
    }
    CHECK(same);

    // a clip that doesn't loop plays it once, then holds its last frame (past 1)
    clip.loop = false;
    CHECK(ActorLogic::GetPlayAt(clip, 2.0f, 4000) == 0);
    CHECK_NEAR(ActorLogic::GetTimeInPlay(clip, 2.0f, 0, 4000), 3.25, 1e-6);
    // and one that doesn't play maps nothing to a tick
    clip.speed = 0.0f;
    CHECK(!ActorLogic::GetTickInPlay(clip, 2.0f, 0, 0.5f));
    CHECK_NEAR(ActorLogic::GetTimeInPlay(clip, 2.0f, 0, 4000), 0.25, 1e-6);
}

TEST("a moment moved stays in order with the others, and a magazine chosen where nothing told when gets moments")
{
    const auto recorded = ReadRecorded("pt_reload_stand_auto");
    const auto profile = ReloadLogic::MakeProfile(recorded.Input());
    using ReloadLogic::Moment;
    const auto gap = ReloadLogic::MIN_MOMENT_GAP;
    // the swap between out and in, out before the swap, in after it, all within a play
    CHECK_NEAR(ReloadLogic::PlaceMoment(profile, Moment::Swap, 0.25f), 0.25, 1e-6);
    CHECK_NEAR(ReloadLogic::PlaceMoment(profile, Moment::Swap, 0.9f), profile.in->time - gap, 1e-6);
    CHECK_NEAR(ReloadLogic::PlaceMoment(profile, Moment::Swap, 0.0f), profile.out->time + gap, 1e-6);
    CHECK_NEAR(ReloadLogic::PlaceMoment(profile, Moment::Out, 0.5f), profile.swap->time - gap, 1e-6);
    CHECK_NEAR(ReloadLogic::PlaceMoment(profile, Moment::Out, -1.0f), 0.0, 1e-6);
    CHECK_NEAR(ReloadLogic::PlaceMoment(profile, Moment::In, 2.0f), 1.0, 1e-6);
    CHECK_NEAR(ReloadLogic::PlaceMoment(profile, Moment::In, 0.1f), profile.swap->time + gap, 1e-6);
    // without a swap, out and in keep apart
    auto unswapped = profile;
    unswapped.swap.reset();
    CHECK_NEAR(ReloadLogic::PlaceMoment(unswapped, Moment::Out, 0.9f), profile.in->time - gap, 1e-6);
    CHECK(ReloadLogic::GetMomentName(Moment::Swap) == "swap");

    // an override with the swap moved there: the profile's swap is the user's, the rest as before
    ReloadLogic::ProfileEntry entry;
    ReloadLogic::GetMoment(entry, Moment::Swap) = ReloadLogic::PlaceMoment(profile, Moment::Swap, 0.25f);
    auto input = recorded.Input();
    input.override = &entry;
    const auto moved = ReloadLogic::MakeProfile(input);
    CHECK(moved.swap && moved.swap->source == ReloadSource::Override);
    CHECK_NEAR(moved.swap->time, 0.25, 1e-6);
    CHECK(moved.out == profile.out && moved.in == profile.in && moved.kind == profile.kind);
    CHECK(ReloadLogic::GetMoment(moved, Moment::Swap) == moved.swap);

    // turned off, back to automatic
    auto off = ReloadLogic::WithKind(entry, ReloadKind::None, profile);
    CHECK(off.kind == ReloadKind::None && off.swap == entry.swap && !off.out);
    CHECK(!ReloadLogic::WithKind(off, std::nullopt, profile).kind);
    // a magazine where nothing tells when: something to move from
    Types::ReloadProfile nothing;
    CHECK(!ReloadLogic::SaysReload(nothing) && ReloadLogic::SaysReload(profile));
    const auto started = ReloadLogic::WithKind({}, ReloadKind::DroppedMagazine, nothing);
    CHECK(started.kind == ReloadKind::DroppedMagazine && started.out == ReloadLogic::DEFAULT_MOMENTS[0] &&
          started.swap == ReloadLogic::DEFAULT_MOMENTS[1] && started.in == ReloadLogic::DEFAULT_MOMENTS[2]);
    // where the profile has them, they stay its own
    CHECK(!ReloadLogic::WithKind({}, ReloadKind::Magazine, profile).out);
}

TEST("a swap the user moves along a clip changes when the hand has the new magazine and when the old one falls")
{
    using Held = ActorLogic::HeldMagazine;
    Reloading rifle("pt_reload_stand_auto");
    rifle.At(UINT32_MAX);
    const auto before = rifle.reloads.at(0);
    const auto& clip = rifle.actor.clips[0];
    // the swap dragged to a tick of the clip's second play, halfway between out and the swap
    const auto play = ActorLogic::GetPlayAt(clip, rifle.recorded.duration, before.out + 3000);
    const auto first = rifle.reloads.at(static_cast<std::size_t>(play));
    const auto tick = (first.out + *first.swap) / 2;
    ReloadLogic::ProfileEntry entry;
    entry.swap = ReloadLogic::PlaceMoment(*rifle.profile, ReloadLogic::Moment::Swap,
                                          ActorLogic::GetTimeInPlay(clip, rifle.recorded.duration, play, tick));
    auto input = rifle.recorded.Input();
    input.override = &entry;
    const auto old = rifle.profile;
    rifle.profile = std::make_shared<const Types::ReloadProfile>(ReloadLogic::MakeProfile(input));
    CHECK(rifle.profile->swap->source == ReloadSource::Override);

    auto magazines = rifle.At(tick);
    CHECK(magazines.state.held == Held::New && rifle.reloads.back().swap == tick);
    CHECK(rifle.HeldError(magazines, rifle.profile->in->time) < 1e-3f);
    CHECK(!magazines.fallen.empty() && magazines.fallen[0].start == tick);
    // a tick before, the old one is still in the hand; as it was there without the override
    magazines = rifle.At(tick - 1);
    CHECK(magazines.state.held == Held::Old);
    rifle.profile = old;
    magazines = rifle.At(tick);
    CHECK(magazines.state.held == Held::Old);
}
