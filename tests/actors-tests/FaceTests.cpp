#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"

using namespace IWXMVM;

// Faces (ActorLogic::GetFace, GetFaceLibrary, WeighLayers' faces)

namespace
{
    using Kind = Types::AnimationKind;
    using Family = Types::AnimationFamily;

    Types::ActorClip Clip(std::string animation, std::uint32_t start, std::uint32_t length, std::uint32_t blendIn = 0,
                          bool loop = true)
    {
        return {.animation = std::move(animation), .start = start, .length = length, .loop = loop, .blendIn = blendIn};
    }

    Types::Actor ActorWith(std::vector<Types::ActorClip> clips, Types::ActorFace face = Types::ActorFace::Auto)
    {
        Types::Actor actor;
        actor.clips = std::move(clips);
        actor.face = face;
        ActorLogic::Normalize(actor);
        return actor;
    }

    // The game's faces (as long as the game's: firing 4.67 s, melee 2.8, pain 1.1, death 2.07), a casual SP idle, an
    // MP player's idle, an SP aim, a shooting loop that fires halfway, a knife swing with a melee notetrack and an SP
    // death with a ragdoll
    ActorLogic::AnimationLookup Animations()
    {
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            static const std::vector<Types::ActorNotetrack> shooting = {{"fire", 0.5f}, {"end", 1.0f}};
            static const std::vector<Types::ActorNotetrack> swinging = {{"melee", 0.25f}, {"end", 1.0f}};
            static const std::vector<Types::ActorNotetrack> dying = {{"start_ragdoll", 0.3f}, {"end", 1.0f}};
            const std::map<std::string, float> faces = {
                {"pf_casual_idle", 36.2f}, {"pf_alert_idle", 33.6f}, {"pf_firing", 4.667f}, {"pf_melee", 2.8f},
                {"pf_pain", 1.1f},         {"pf_death", 2.067f},     {"f_smile", 3.0f}};
            if (const auto face = faces.find(name); face != faces.end())
                return ActorLogic::AnimationInfo{face->second, glm::vec2(0), {}, Kind::Face, Family::Human};
            if (name == "casual_stand_idle")
                return ActorLogic::AnimationInfo{3.9f, glm::vec2(0), {}, Kind::FullBody, Family::SpHuman};
            if (name == "pb_stand_alert")
                return ActorLogic::AnimationInfo{12.8f, glm::vec2(0), {}, Kind::FullBody, Family::MpPlayer};
            if (name == "exposed_aim_5")
                return ActorLogic::AnimationInfo{2.0f, glm::vec2(0), {}, Kind::FullBody, Family::SpHuman};
            if (name == "shoot_loop")
                return ActorLogic::AnimationInfo{1.0f, glm::vec2(0), shooting, Kind::FullBody, Family::SpHuman};
            if (name == "knife_swing")
                return ActorLogic::AnimationInfo{2.0f, glm::vec2(0), swinging, Kind::FullBody, Family::SpHuman};
            if (name == "ai_death_fallforward")
                return ActorLogic::AnimationInfo{1.333f, glm::vec2(0), dying, Kind::FullBody, Family::SpHuman};
            return std::nullopt;
        };
    }

    // a game without the players' faces
    ActorLogic::AnimationLookup WithoutFaces()
    {
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            if (name.starts_with("pf_"))
                return std::nullopt;
            return Animations()(name);
        };
    }

    // how much the face plays an animation at a tick
    float WeightOf(const std::vector<ActorLogic::FaceSample>& face, std::string_view animation)
    {
        float weight = 0.0f;
        for (const auto& sample : face)
            if (sample.animation == animation)
                weight += sample.weight;
        return weight;
    }

    const ActorLogic::FaceSample* Find(const std::vector<ActorLogic::FaceSample>& face, std::string_view animation)
    {
        const auto found = std::ranges::find(face, animation, &ActorLogic::FaceSample::animation);
        return found != face.end() ? &*found : nullptr;
    }

    float Total(const std::vector<ActorLogic::FaceSample>& face)
    {
        float total = 0.0f;
        for (const auto& sample : face)
            total += sample.weight;
        return total;
    }
}  // namespace

TEST("the face idles, alert while a clip aims or shoots")
{
    const auto animations = Animations();
    // casual, then aiming (with a blend of 400 ms that the face follows), then a player's idle
    const auto actor = ActorWith({Clip("casual_stand_idle", 1000, 4000), Clip("exposed_aim_5", 5000, 4000, 400),
                                  Clip("pb_stand_alert", 9000, 4000, 0)});
    auto face = ActorLogic::GetFace(actor, 3000, animations);
    CHECK(face.size() == 1 && face[0].animation == "pf_casual_idle" && face[0].loop && !face[0].clip);
    CHECK_NEAR(face[0].weight, 1.0, 1e-5);
    CHECK_NEAR(face[0].time, 2.0, 1e-5);
    face = ActorLogic::GetFace(actor, 5100, animations);
    CHECK_NEAR(WeightOf(face, "pf_alert_idle"), 0.25, 1e-4);
    CHECK_NEAR(WeightOf(face, "pf_casual_idle"), 0.75, 1e-4);
    // (both from the first clip's start)
    CHECK_NEAR(Find(face, "pf_alert_idle")->time, 4.1, 1e-4);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 7000, animations), "pf_alert_idle"), 1.0, 1e-5);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 10000, animations), "pf_alert_idle"), 1.0, 1e-5);
    // before the first clip, as it is at its start
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 0, animations), "pf_casual_idle"), 1.0, 1e-5);

    CHECK(ActorLogic::IsAlertAnimation("pt_stand_shoot_auto", std::nullopt));
    CHECK(ActorLogic::IsAlertAnimation("walk_cqb_f", std::nullopt));
    CHECK(ActorLogic::IsAlertAnimation("shoot_loop", animations("shoot_loop")));
    CHECK(!ActorLogic::IsAlertAnimation("casual_stand_idle", animations("casual_stand_idle")));
    CHECK(!ActorLogic::IsAlertAnimation("run_lowready_f", std::nullopt));

    // Off, or without clips, or a game without them: no face
    CHECK(ActorLogic::GetFace(ActorWith({Clip("casual_stand_idle", 0, 4000)}, Types::ActorFace::Off), 1000, animations)
              .empty());
    CHECK(ActorLogic::GetFace(ActorWith({}), 1000, animations).empty());
    CHECK(ActorLogic::GetFace(actor, 3000, WithoutFaces()).empty());
}

TEST("the face fires for half a second from each shot")
{
    const auto animations = Animations();
    auto actor = ActorWith({Clip("casual_stand_idle", 0, 20000)});
    // a burst of three, 100 ms apart, and a shot alone later
    actor.events.push_back({.type = Types::ActorEventType::Fire, .tick = 2000, .shots = 3, .interval = 100});
    actor.events.push_back({.type = Types::ActorEventType::Fire, .tick = 6000});
    ActorLogic::Normalize(actor);

    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 1999, animations), "pf_firing"), 0.0, 1e-6);
    // blending in over 100 ms from the first shot, the animation from there
    auto face = ActorLogic::GetFace(actor, 2050, animations);
    CHECK_NEAR(WeightOf(face, "pf_firing"), 0.5, 1e-4);
    CHECK_NEAR(WeightOf(face, "pf_casual_idle"), 0.5, 1e-4);
    CHECK(!Find(face, "pf_firing")->loop);
    CHECK_NEAR(Find(face, "pf_firing")->time, 0.05, 1e-4);
    face = ActorLogic::GetFace(actor, 2600, animations);
    CHECK_NEAR(WeightOf(face, "pf_firing"), 1.0, 1e-5);
    CHECK_NEAR(Find(face, "pf_firing")->time, 0.6, 1e-4);
    // until 500 ms after the last shot (2200), then back to the idle over 100 ms
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 2699, animations), "pf_firing"), 1.0, 1e-5);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 2750, animations), "pf_firing"), 0.5, 1e-4);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 2800, animations), "pf_firing"), 0.0, 1e-6);
    // the shot alone: its window of its own, the animation from its start
    face = ActorLogic::GetFace(actor, 6200, animations);
    CHECK_NEAR(WeightOf(face, "pf_firing"), 1.0, 1e-5);
    CHECK_NEAR(Find(face, "pf_firing")->time, 0.2, 1e-4);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 6700, animations), "pf_firing"), 0.0, 1e-6);

    // a clip's fire notetracks too, once it has a gun and shoots them
    auto shooter = ActorWith({Clip("shoot_loop", 1000, 3000)});
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(shooter, 1700, animations), "pf_firing"), 0.0, 1e-6);
    shooter.weapon = "ak47_mp";
    // shots at 1500, 2500 and 3500
    face = ActorLogic::GetFace(shooter, 1700, animations);
    CHECK_NEAR(WeightOf(face, "pf_firing"), 1.0, 1e-5);
    CHECK_NEAR(Find(face, "pf_firing")->time, 0.2, 1e-4);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(shooter, 2200, animations), "pf_firing"), 0.0, 1e-6);
    CHECK_NEAR(Find(ActorLogic::GetFace(shooter, 2600, animations), "pf_firing")->time, 0.1, 1e-4);
    shooter.clips[0].notetrackShots = false;
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(shooter, 1700, animations), "pf_firing"), 0.0, 1e-6);
}

TEST("the face winces when hit and dies with a death")
{
    const auto animations = Animations();
    auto actor = ActorWith({Clip("exposed_aim_5", 0, 20000)});
    actor.events.push_back({.type = Types::ActorEventType::Hit, .tick = 3000});
    // a burst under the pain: the pain goes over it
    actor.events.push_back({.type = Types::ActorEventType::Fire, .tick = 2900, .shots = 5, .interval = 100});
    ActorLogic::Normalize(actor);
    auto face = ActorLogic::GetFace(actor, 3500, animations);
    CHECK_NEAR(WeightOf(face, "pf_pain"), 1.0, 1e-5);
    CHECK_NEAR(Find(face, "pf_pain")->time, 0.5, 1e-4);
    CHECK(!Find(face, "pf_pain")->loop);
    // once through (1.1 s), then the idle: the firing it went over ended meanwhile, 500 ms after its last shot (3300)
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 4099, animations), "pf_pain"), 1.0, 1e-5);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 4300, animations), "pf_pain"), 0.0, 1e-6);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 4300, animations), "pf_alert_idle"), 1.0, 1e-5);

    // a melee notetrack: the melee's face once through
    const auto swinging = ActorWith({Clip("knife_swing", 1000, 2000, 0, false)});
    face = ActorLogic::GetFace(swinging, 1700, animations);
    CHECK_NEAR(WeightOf(face, "pf_melee"), 1.0, 1e-5);
    CHECK_NEAR(Find(face, "pf_melee")->time, 0.2, 1e-4);

    // a death (as Hit "Dies" makes it): its face from the clip's start, held after it, over the hit's pain
    auto dying = ActorWith({Clip("casual_stand_idle", 0, 5000)});
    ActorLogic::AddDeath(dying, 5000, "ai_death_fallforward", 1.333f);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(dying, 5050, animations), "pf_death"), 0.5, 1e-4);
    for (const auto tick : {5100u, 6000u, 9000u, 60000u})
    {
        face = ActorLogic::GetFace(dying, tick, animations);
        CHECK_NEAR(WeightOf(face, "pf_death"), 1.0, 1e-5);
        CHECK(!Find(face, "pf_death")->loop);
        CHECK_NEAR(Find(face, "pf_death")->time, (tick - 5000) / 1000.0, 1e-4);
    }
    // played from the view (PlayAt) too, and alive again once what played resumes after it
    auto played = ActorWith({Clip("casual_stand_idle", 0, 10000)});
    const auto death = ActorLogic::PlayAt(played, 4000, "ai_death_fallforward", 1.333f, false,
                                          animations("ai_death_fallforward")->notetracks);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(played, 8000, animations), "pf_death"), 1.0, 1e-5);
    ActorLogic::SetClipAfter(played, death, ActorLogic::ClipAfter::Resume, 20000);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(played, 5200, animations), "pf_death"), 1.0, 1e-5);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(played, 5500, animations), "pf_casual_idle"), 1.0, 1e-5);
}

TEST("the Face row's clips go over the automatic face while they show")
{
    const auto animations = Animations();
    auto actor = ActorWith({Clip("casual_stand_idle", 0, 20000)});
    actor.events.push_back({.type = Types::ActorEventType::Fire, .tick = 3000, .shots = 20, .interval = 100});
    const auto smile = ActorLogic::AddFaceClip(actor, Clip("f_smile", 2000, 3000, 200, false));
    CHECK(actor.faceClips.size() == 1 && actor.faceClips[0].id == smile);

    // blending in over its blend in, over the idle
    auto face = ActorLogic::GetFace(actor, 2100, animations);
    CHECK_NEAR(WeightOf(face, "f_smile"), 0.5, 1e-4);
    CHECK_NEAR(WeightOf(face, "pf_casual_idle"), 0.5, 1e-4);
    CHECK(Find(face, "f_smile")->clip && Find(face, "f_smile")->clip->id == smile);
    CHECK_NEAR(Find(face, "f_smile")->time, 0.1, 1e-4);
    // all of the face while it shows, though the actor shoots
    face = ActorLogic::GetFace(actor, 4000, animations);
    CHECK(face.size() == 1 && face[0].animation == "f_smile");
    CHECK_NEAR(face[0].weight, 1.0, 1e-6);
    // out over FACE_BLEND as it ends, to what the face does then (firing)
    face = ActorLogic::GetFace(actor, 5050, animations);
    CHECK_NEAR(WeightOf(face, "f_smile"), 0.5, 1e-4);
    CHECK_NEAR(WeightOf(face, "pf_firing"), 0.5, 1e-4);
    // (going on as it blends out, as a clip under the next one's blend in does)
    CHECK_NEAR(Find(face, "f_smile")->time, 3.05, 1e-4);
    CHECK_NEAR(WeightOf(ActorLogic::GetFace(actor, 5200, animations), "f_smile"), 0.0, 1e-6);

    // one after another: the second blends in over the first
    ActorLogic::AddFaceClip(actor, Clip("pf_pain", 3000, 1000, 400, false));
    CHECK(actor.faceClips.size() == 2 && actor.faceClips[0].End() == 3000);
    face = ActorLogic::GetFace(actor, 3100, animations);
    CHECK_NEAR(WeightOf(face, "pf_pain"), 0.25, 1e-4);
    CHECK_NEAR(WeightOf(face, "f_smile"), 0.75, 1e-4);
    CHECK_NEAR(Total(face), 1.0, 1e-5);

    // with Face Off they still play, over nothing
    actor.face = Types::ActorFace::Off;
    face = ActorLogic::GetFace(actor, 2100, animations);
    CHECK(face.size() == 1 && face[0].animation == "f_smile");
    CHECK_NEAR(face[0].weight, 0.5, 1e-4);
    CHECK(ActorLogic::GetFace(actor, 8000, animations).empty());
}

TEST("the face is a function of the tick, scrubbed or played")
{
    const auto animations = Animations();
    auto actor = ActorWith({Clip("casual_stand_idle", 0, 4000), Clip("shoot_loop", 4000, 6000, 250),
                            Clip("ai_death_fallforward", 10000, 1333, 100, false)});
    actor.weapon = "ak47_mp";
    actor.events.push_back({.type = Types::ActorEventType::Hit, .tick = 7000});
    actor.events.push_back({.type = Types::ActorEventType::Fire, .tick = 2000, .shots = 10, .interval = 90});
    ActorLogic::AddFaceClip(actor, Clip("f_smile", 1000, 500, 100));
    ActorLogic::Normalize(actor);
    // the same at a tick whatever came before (as GetFace keeps nothing), and every face adding up to 1
    std::vector<std::vector<ActorLogic::FaceSample>> forwards;
    for (std::uint32_t tick = 0; tick < 13000; tick += 37)
        forwards.push_back(ActorLogic::GetFace(actor, tick, animations));
    std::size_t i = forwards.size();
    for (std::uint32_t tick = (static_cast<std::uint32_t>(forwards.size()) - 1) * 37;; tick -= 37)
    {
        const auto face = ActorLogic::GetFace(actor, tick, animations);
        const auto& before = forwards[--i];
        CHECK(face.size() == before.size());
        for (std::size_t k = 0; k < face.size() && k < before.size(); ++k)
            CHECK(face[k].animation == before[k].animation && face[k].weight == before[k].weight &&
                  face[k].time == before[k].time);
        CHECK_NEAR(Total(face), 1.0, 1e-4);
        if (tick < 37)
            break;
    }
}

TEST("the face's animations are weighed as much as the clips, and are the tree's entries")
{
    const auto animations = Animations();
    auto actor = ActorWith({Clip("casual_stand_idle", 0, 4000), Clip("exposed_aim_5", 4000, 4000, 400)});
    actor.events.push_back({.type = Types::ActorEventType::Fire, .tick = 4100});
    actor.events.push_back({.type = Types::ActorEventType::Fire, .tick = 5800});
    ActorLogic::AddFaceClip(actor, Clip("pf_firing", 6000, 1000, 200));
    const auto none = [](const std::string&) { return PoseLogic::SpineKind::AtRest; };

    const auto layers = ActorLogic::GetLayers(actor, 4150, animations, Types::GunClass::Rifle);
    const auto weights = ActorLogic::WeighLayers(layers, none);
    float face = 0.0f, firing = 0.0f;
    for (const auto& entry : weights.faces)
    {
        face += entry.weight;
        if (entry.animation == "pf_firing")
            firing = entry.weight;
    }
    CHECK_NEAR(face, 1.0, 1e-5);
    CHECK_NEAR(firing, 0.5, 1e-4);
    // (the clips' entries as they were)
    CHECK(weights.animations.size() == 2);

    // a face clip of an animation the automatic face plays too is the same entry, as the heavier (or the clip) has it
    const auto over =
        ActorLogic::WeighLayers(ActorLogic::GetLayers(actor, 6100, animations, Types::GunClass::Rifle), none);
    CHECK(over.faces.size() == 1 && over.faces[0].animation == "pf_firing");
    CHECK_NEAR(over.faces[0].weight, 1.0, 1e-5);
    CHECK_NEAR(over.faces[0].time, 0.1, 1e-4);

    // the library: the game's six with Face Auto, then the face clips'
    const auto library = ActorLogic::GetFaceLibrary(actor, animations);
    CHECK(library.size() == 6 && library[0] == "pf_casual_idle" && library[5] == "pf_death");
    actor.face = Types::ActorFace::Off;
    CHECK(ActorLogic::GetFaceLibrary(actor, animations) == std::vector<std::string>{"pf_firing"});
    actor.face = Types::ActorFace::Auto;
    ActorLogic::AddFaceClip(actor, Clip("f_smile", 8000, 1000));
    CHECK(ActorLogic::GetFaceLibrary(actor, animations).size() == 7);
    const std::vector<std::string> clipsOnly{"pf_firing", "f_smile"};
    CHECK(ActorLogic::GetFaceLibrary(actor, WithoutFaces()) == clipsOnly);
    // without faces, nothing of them
    actor.face = Types::ActorFace::Off;
    actor.faceClips.clear();
    CHECK(ActorLogic::GetFaceLibrary(actor, animations).empty());
    CHECK(ActorLogic::WeighLayers(ActorLogic::GetLayers(actor, 4150, animations, Types::GunClass::Rifle), none)
              .faces.empty());
}

TEST("scenes keep the face and the Face row's clips, and older scenes have none")
{
    ActorLogic::Scene scene;
    Types::Actor actor;
    actor.models = {"body"};
    actor.clips = {Clip("casual_stand_idle", 0, 5000)};
    actor.face = Types::ActorFace::Auto;
    ActorLogic::AddFaceClip(actor, Clip("pf_pain", 1000, 1100, 100, false));
    ActorLogic::AddFaceClip(actor, Clip("f_smile", 3000, 2000, 0, true));
    scene.actors.push_back(actor);
    const auto read = ActorLogic::SceneFromJson(ActorLogic::SceneToJson(scene, "demo"), 10000);
    CHECK(read.actors.size() == 1);
    CHECK(read.actors[0].face == Types::ActorFace::Auto);
    CHECK(read.actors[0].faceClips.size() == 2);
    CHECK(read.actors[0].faceClips[0].animation == "pf_pain" && read.actors[0].faceClips[0].start == 1000 &&
          read.actors[0].faceClips[0].length == 1100 && !read.actors[0].faceClips[0].loop &&
          read.actors[0].faceClips[0].blendIn == 100);
    CHECK(read.actors[0].faceClips[1].animation == "f_smile" && read.actors[0].faceClips[1].loop);
    CHECK(read.actors[0].faceClips[0].id != 0 && read.actors[0].faceClips[0].id != read.actors[0].faceClips[1].id);

    auto older = ActorLogic::SceneToJson(scene, "demo");
    older["actors"][0].erase("face");
    older["actors"][0].erase("faceClips");
    const auto old = ActorLogic::SceneFromJson(older, 10000);
    CHECK(old.actors[0].face == Types::ActorFace::Off && old.actors[0].faceClips.empty());

    // starting points get it when the game has the players' faces; a formation's copies keep theirs, later
    ActorLogic::StartAnimations starts{.idle = "casual_stand_idle"};
    CHECK(ActorLogic::MakeStart(ActorLogic::Start::Standing, starts, 0, 10000, "").face == Types::ActorFace::Off);
    starts.face = true;
    CHECK(ActorLogic::MakeStart(ActorLogic::Start::Standing, starts, 0, 10000, "").face == Types::ActorFace::Auto);
    const auto copies = ActorLogic::MakeFormation(actor, {.count = 2, .stagger = 300}, 0.0f);
    CHECK(copies.size() == 1 && copies[0].face == Types::ActorFace::Auto && copies[0].faceClips.size() == 2);
    CHECK(copies[0].faceClips[0].start == 1300);
}
