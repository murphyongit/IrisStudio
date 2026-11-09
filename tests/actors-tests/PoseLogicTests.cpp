#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/PoseLogic.hpp"

using namespace IWXMVM;

namespace
{
    glm::quat Turn(float degrees, glm::vec3 axis)
    {
        return glm::angleAxis(glm::radians(degrees), glm::normalize(axis));
    }

    void CheckNear(glm::vec3 actual, glm::vec3 expected, float tolerance)
    {
        CHECK_NEAR(actual.x, expected.x, tolerance);
        CHECK_NEAR(actual.y, expected.y, tolerance);
        CHECK_NEAR(actual.z, expected.z, tolerance);
    }

    // the angle between two rotations, in degrees (from the rotation between them, as acos is too coarse near 0)
    float AngleBetween(glm::quat a, glm::quat b)
    {
        const auto between = glm::inverse(a) * b;
        return glm::degrees(2.0f *
                            std::atan2(glm::length(glm::vec3(between.x, between.y, between.z)), std::abs(between.w)));
    }
}  // namespace

TEST("bones are placed down their chain from the root")
{
    // an arm along x from the root, turned 90 degrees about z at the shoulder: the elbow and the hand go along y
    const std::vector<PoseLogic::LocalBone> bones = {
        {.rotation = glm::quat(1, 0, 0, 0), .translation = {0, 0, 50}, .parent = -1},
        {.rotation = Turn(90, {0, 0, 1}), .translation = {10, 0, 0}, .parent = 0},
        {.rotation = glm::quat(1, 0, 0, 0), .translation = {12, 0, 0}, .parent = 1},
        {.rotation = Turn(-90, {0, 0, 1}), .translation = {10, 0, 0}, .parent = 2},
    };
    Types::AnimationPose pose;
    PoseLogic::Compose(bones, pose);
    CheckNear(pose.positions[0], {0, 0, 50}, 0.001f);
    CheckNear(pose.positions[1], {10, 0, 50}, 0.001f);
    CheckNear(pose.positions[2], {10, 12, 50}, 0.001f);
    CheckNear(pose.positions[3], {10, 22, 50}, 0.001f);
    // the hand is turned back straight
    CHECK_NEAR(AngleBetween(pose.rotations[3], glm::quat(1, 0, 0, 0)), 0, 0.01);
    CHECK_NEAR(AngleBetween(pose.rotations[2], Turn(90, {0, 0, 1})), 0, 0.01);
    CHECK(pose.parents == (std::vector<std::int32_t>{-1, 0, 1, 2}));
}

TEST("a bone whose parent isn't before it is placed from the root")
{
    const std::vector<PoseLogic::LocalBone> bones = {
        {.translation = {5, 0, 0}, .parent = -1},
        {.translation = {0, 3, 0}, .parent = 1},
        {.translation = {0, 0, 2}, .parent = 7},
    };
    Types::AnimationPose pose;
    PoseLogic::Compose(bones, pose);
    CheckNear(pose.positions[1], {0, 3, 0}, 0.001f);
    CheckNear(pose.positions[2], {0, 0, 2}, 0.001f);
    CHECK(pose.parents == (std::vector<std::int32_t>{-1, -1, -1}));
}

TEST("a bone's transform has its axes and position, and puts what is on it where it is")
{
    const std::vector<PoseLogic::LocalBone> bones = {
        {.rotation = Turn(90, {0, 0, 1}), .translation = {1, 2, 3}, .parent = -1},
        {.rotation = Turn(30, {1, 0, 0}), .translation = {4, 0, 0}, .parent = 0},
    };
    Types::AnimationPose pose;
    pose.names = {"j_wrist_le", "tag_inhand"};
    PoseLogic::Compose(bones, pose);
    const auto transform = PoseLogic::GetTransform(pose, 1);
    CheckNear(glm::vec3(transform[3]), pose.positions[1], 0.001f);
    CheckNear(glm::vec3(transform[3]), {1, 6, 3}, 0.001f);
    // its x axis is the root's y; a point on it is where the chain puts it
    CheckNear(glm::vec3(transform[0]), {0, 1, 0}, 0.001f);
    CheckNear(glm::vec3(transform * glm::vec4(2, 0, 0, 1)), {1, 8, 3}, 0.001f);
    CHECK(pose.FindBone("tag_inhand") == std::optional<std::size_t>(1));
    CHECK(!pose.FindBone("tag_clip"));
}

TEST("transforms are as far apart as their origins, or as points along their axes when they are turned")
{
    const auto a = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));
    CHECK_NEAR(PoseLogic::GetTransformDistance(a, a, 10.0f), 0, 0.0001);
    CHECK_NEAR(PoseLogic::GetTransformDistance(a, glm::translate(a, glm::vec3(0, 0.3f, 0.4f)), 10.0f), 0.5, 0.0001);
    // turned by 90 degrees: a point 10 along x is 10 * sqrt(2) away
    const auto turned = a * glm::mat4_cast(Turn(90, {0, 0, 1}));
    CHECK_NEAR(PoseLogic::GetTransformDistance(a, turned, 10.0f), 10 * std::sqrt(2.0f), 0.001);
    // by a little: about the arc
    const auto little = a * glm::mat4_cast(Turn(0.5f, {0, 1, 0}));
    CHECK_NEAR(PoseLogic::GetTransformDistance(a, little, 10.0f), glm::radians(0.5f) * 10.0f, 0.001);
}

TEST("animations made for SP bodies line up the spine; those for MP bodies and upper bodies don't")
{
    using Kind = PoseLogic::SpineKind;
    const std::vector<std::string_view> mp = {"j_mainroot", "pelvis", "torso_stabilizer", "j_spinelower", "j_head"};
    const std::vector<std::string_view> sp = {"j_mainroot", "j_spinelower", "j_spineupper", "j_wrist_le"};
    const std::vector<std::string_view> upper = {"j_spinelower", "j_spineupper", "j_wrist_le", "j_wrist_ri"};
    const std::vector<std::string_view> none = {};
    CHECK(PoseLogic::GetSpineKind(mp) == Kind::Moves);
    CHECK(PoseLogic::GetSpineKind(sp) == Kind::Lined);
    CHECK(PoseLogic::GetSpineKind(upper) == Kind::AtRest);
    CHECK(PoseLogic::GetSpineKind(none) == Kind::AtRest);
}

TEST("the lined up stabilizer turns as pelvis's parent does, and the spine starts where it rests")
{
    // rigs like an MP body's: j_mainroot, pelvis on it, torso_stabilizer on pelvis, j_spinelower on that
    struct Rig
    {
        glm::quat mainroot, pelvis, stabilizer;
        glm::vec3 pelvisAt, stabilizerAt, spineAt;
    };
    const std::vector<Rig> rigs = {
        {glm::quat(1, 0, 0, 0), glm::quat(1, 0, 0, 0), Turn(-90, {0, 1, 0}), {0, 0, 0}, {0.07f, 0, 5.2f}, {5, 0, 0}},
        {Turn(90, {1, 0, 0}), Turn(20, {0, 0, 1}), Turn(-85, {0.1f, 1, 0}), {0, 0, 40}, {0.5f, 0.1f, 5}, {4, 1, 0.5f}},
        {Turn(-30, {1, 1, 1}), Turn(-100, {0, 1, 0.3f}), Turn(45, {1, 0, 0}), {1, 2, 3}, {0, 0, 0}, {0, 3, 0}},
    };
    for (const auto& rig : rigs)
    {
        const auto lineUp = PoseLogic::LineUpSpine(rig.pelvis, rig.stabilizer, rig.spineAt);
        const auto pose = [&](glm::quat stabilizer, glm::vec3 offset) {
            const std::vector<PoseLogic::LocalBone> bones = {
                {.rotation = rig.mainroot, .translation = {0, 0, 0}, .parent = -1},
                {.rotation = rig.pelvis, .translation = rig.pelvisAt, .parent = 0},
                {.rotation = stabilizer, .translation = rig.stabilizerAt + offset, .parent = 1},
                {.rotation = glm::quat(1, 0, 0, 0), .translation = rig.spineAt, .parent = 2},
            };
            Types::AnimationPose result;
            PoseLogic::Compose(bones, result);
            return result;
        };
        const auto atRest = pose(rig.stabilizer, {});
        const auto lined = pose(lineUp.rotation, lineUp.offset);
        CHECK_NEAR(AngleBetween(lined.rotations[2], rig.mainroot), 0, 0.01);
        CheckNear(lined.positions[3], atRest.positions[3], 0.001f);

        // aimed ahead for the players' upper body over SP legs (AimSpine): the model's axes, however the legs turn
        // j_mainroot, the spine starting where it rests (the user's crouching actor aimed 37 degrees to its right)
        const auto aim = PoseLogic::AimSpine(rig.pelvis, rig.stabilizer, rig.spineAt, rig.mainroot);
        const auto aimed = pose(aim.rotation, aim.offset);
        CHECK_NEAR(AngleBetween(aimed.rotations[2], glm::quat(1, 0, 0, 0)), 0, 0.01);
        CheckNear(aimed.positions[3], atRest.positions[3], 0.001f);
    }

    // an idle's pelvis swaying 6 degrees either way about a turn of 37: the middle of it, however its quaternions'
    // signs come
    const auto middle = Turn(-37, {0, 0, 1});
    const std::vector<glm::quat> sway = {middle * Turn(6, {0, 0, 1}), -(middle * Turn(-6, {0, 0, 1})),
                                         middle * Turn(3, {0, 1, 0}), -(middle * Turn(-3, {0, 1, 0}))};
    CHECK_NEAR(AngleBetween(PoseLogic::AverageRotation(sway), middle), 0, 0.01);
    CHECK_NEAR(AngleBetween(PoseLogic::AverageRotation({}), glm::quat(1, 0, 0, 0)), 0, 0.01);
}

namespace
{
    // the rest values, as the zones have them (x, y, z, w)
    glm::quat Rest(float x, float y, float z, float w)
    {
        return glm::normalize(glm::quat(w, x, y, z));
    }

    const glm::quat MAINROOT_REST = Rest(-0.494f, -0.506f, -0.506f, 0.494f);
    const glm::vec3 MAINROOT_AT = {-3.44f, 0.0f, 37.23f};
    const glm::quat STABILIZER_REST = Rest(0.494f, 0.506f, 0.506f, 0.494f);
    const glm::quat MP_SPINE_REST = Rest(-0.492f, -0.507f, -0.507f, 0.493f);
    const glm::vec3 MP_SPINE_AT = {0.07f, 0.0f, 5.2f};
    const glm::quat SP_SPINE_REST = Rest(0.0f, 0.0f, -0.003f, 1.0f);
    const glm::vec3 SP_SPINE_AT = {5.2f, 0.2f, 0.0f};
    // j_spineupper on j_spinelower, and a point well up the back on that, alike on both rigs
    const glm::quat SPINEUPPER_REST = Rest(0.0f, 0.0f, 0.025f, 1.0f);
    const glm::vec3 SPINEUPPER_AT = {5.22f, 0.17f, 0.0f};
    const glm::vec3 HEAD_AT = {20.0f, 1.0f, 0.0f};

    PoseLogic::SpineChain MpChain()
    {
        return {.pelvis = {},
                .stabilizer = {.rotation = STABILIZER_REST},
                .spine = {.rotation = MP_SPINE_REST, .translation = MP_SPINE_AT}};
    }

    // An animation's keys of the bones down the back, as the players' full body animations have them
    struct SpineKeys
    {
        glm::quat mainroot;
        glm::vec3 mainrootOffset;
        glm::quat stabilizer;
        glm::quat spine;
        glm::vec3 spineOffset;
    };

    const std::vector<SpineKeys> MP_KEYS = {
        // at rest
        {MAINROOT_REST, {}, STABILIZER_REST, MP_SPINE_REST, {}},
        // leaning, turned and bent, the spine moved a little
        {MAINROOT_REST * Turn(10, {0, 0, 1}),
         {1, -2, 0.5f},
         STABILIZER_REST* Turn(25, {1, 0.2f, 0}),
         MP_SPINE_REST* Turn(-15, {0, 1, 0.3f}),
         {0.1f, 0.2f, -0.3f}},
        {Turn(-80, {1, 1, 0}), {0, 0, -10}, Turn(60, {0.3f, 1, 0.5f}), Turn(110, {1, 0, 0.2f}), {}},
    };

    // the MP body: j_mainroot, pelvis, the stabilizer, j_spinelower, j_spineupper and a point up the back
    Types::AnimationPose PoseMpBody(const SpineKeys& keys)
    {
        const std::vector<PoseLogic::LocalBone> bones = {
            {.rotation = keys.mainroot, .translation = MAINROOT_AT + keys.mainrootOffset, .parent = -1},
            {.parent = 0},
            {.rotation = keys.stabilizer, .parent = 1},
            {.rotation = keys.spine, .translation = MP_SPINE_AT + keys.spineOffset, .parent = 2},
            {.rotation = SPINEUPPER_REST, .translation = SPINEUPPER_AT, .parent = 3},
            {.translation = HEAD_AT, .parent = 4},
        };
        Types::AnimationPose pose;
        PoseLogic::Compose(bones, pose);
        return pose;
    }

    // an SP body (j_mainroot, j_spinelower, j_spineupper and the point up the back) with j_spinelower given
    Types::AnimationPose PoseSpBody(const SpineKeys& keys, const PoseLogic::LocalBone& spine)
    {
        const std::vector<PoseLogic::LocalBone> bones = {
            {.rotation = keys.mainroot, .translation = MAINROOT_AT + keys.mainrootOffset, .parent = -1},
            {.rotation = spine.rotation, .translation = spine.translation, .parent = 0},
            {.rotation = SPINEUPPER_REST, .translation = SPINEUPPER_AT, .parent = 1},
            {.translation = HEAD_AT, .parent = 2},
        };
        Types::AnimationPose pose;
        PoseLogic::Compose(bones, pose);
        return pose;
    }
}  // namespace

TEST("an MP animation's spine composed through the spine rig is where the MP body has it")
{
    for (const auto& keys : MP_KEYS)
    {
        const auto mp = PoseMpBody(keys);
        const auto spine = PoseLogic::ComposeSpine(MpChain(), {}, {.rotation = keys.stabilizer},
                                                   {.rotation = keys.spine, .offset = keys.spineOffset});
        const auto sp = PoseSpBody(keys, spine);
        // j_spinelower, j_spineupper and the point up the back
        for (const auto [spBone, mpBone] : {std::pair(1, 3), std::pair(2, 4), std::pair(3, 5)})
        {
            CheckNear(sp.positions[spBone], mp.positions[mpBone], 0.001f);
            CHECK_NEAR(AngleBetween(sp.rotations[spBone], mp.rotations[mpBone]), 0, 0.01);
        }
    }
}

TEST("an MP animation's spine keys on an SP body without the rig turn its torso")
{
    // what the game did before the rig: the stabilizer's keys are lost and j_spinelower's are taken as on j_mainroot
    for (const auto& keys : MP_KEYS)
    {
        const auto mp = PoseMpBody(keys);
        const auto sp = PoseSpBody(keys, {.rotation = keys.spine, .translation = SP_SPINE_AT + keys.spineOffset});
        CHECK(glm::distance(sp.positions[3], mp.positions[5]) > 10.0f);
        CHECK(AngleBetween(sp.rotations[1], mp.rotations[3]) > 45.0f);
    }
}

TEST("the spine rig fitted to an SP body rests its spine where the body has it")
{
    const auto chain = MpChain();
    const PoseLogic::LocalBone spRest{.rotation = SP_SPINE_REST, .translation = SP_SPINE_AT};
    const auto fitted = PoseLogic::FitSpineChain(chain, spRest);
    const auto rested = PoseLogic::ComposeSpine(fitted, {}, {}, {});
    CheckNear(rested.translation, SP_SPINE_AT, 0.0001f);
    CHECK_NEAR(AngleBetween(rested.rotation, SP_SPINE_REST), 0, 0.01);
    // the MP rest is a little off from it (by 0.006 units and 0.5 degrees with these rests), and the fit only moves
    // j_spinelower
    const auto mpRested = PoseLogic::ComposeSpine(chain, {}, {}, {});
    const auto apart = glm::distance(mpRested.translation, SP_SPINE_AT);
    CHECK(apart > 0.001f && apart < 0.05f);
    const auto turned = AngleBetween(mpRested.rotation, SP_SPINE_REST);
    CHECK(turned > 0.05f && turned < 1.0f);
    CHECK_NEAR(AngleBetween(fitted.stabilizer.rotation, chain.stabilizer.rotation), 0, 0.001);
    CheckNear(fitted.spine.translation, MP_SPINE_AT, 0.05f);

    // and an MP animation's keys put the spine where they did, as far off as the rests are
    for (const auto& keys : MP_KEYS)
    {
        const auto onMp = PoseLogic::ComposeSpine(chain, {}, {.rotation = keys.stabilizer}, {.rotation = keys.spine});
        const auto onFitted =
            PoseLogic::ComposeSpine(fitted, {}, {.rotation = keys.stabilizer}, {.rotation = keys.spine});
        CHECK_NEAR(AngleBetween(onMp.rotation, onFitted.rotation), 0, 0.01);
        CHECK(glm::distance(onMp.translation, onFitted.translation) < 0.05f);
    }
}

TEST("an SP animation on an SP body with the spine rig is as without it, the stabilizer lined up")
{
    // the rig's stabilizer lined up for SP animations (LineUpSpine), as on an actor with an SP and an MP clip
    const auto fitted = PoseLogic::FitSpineChain(MpChain(), {.rotation = SP_SPINE_REST, .translation = SP_SPINE_AT});
    const auto lineUp =
        PoseLogic::LineUpSpine(fitted.pelvis.rotation, fitted.stabilizer.rotation, fitted.spine.translation);
    const std::vector<std::pair<glm::quat, glm::vec3>> spKeys = {
        {SP_SPINE_REST, {}}, {Turn(30, {0, 0, 1}), {0.2f, -0.1f, 0.4f}}, {Turn(-70, {1, 0.5f, 0}), {1, 0, 0}}};
    for (const auto& [rotation, offset] : spKeys)
    {
        const auto rigged = PoseLogic::ComposeSpine(fitted, {}, {.rotation = lineUp.rotation, .offset = lineUp.offset},
                                                    {.rotation = rotation, .offset = offset});
        CheckNear(rigged.translation, SP_SPINE_AT + offset, 0.001f);
        CHECK_NEAR(AngleBetween(rigged.rotation, rotation), 0, 0.01);
    }
}

TEST("a player's body gives up a tag for the stabilizer: tag_sync, or another tag nothing hangs from, between the bones")
{
    using Names = std::vector<std::string_view>;
    using Parents = std::vector<std::int32_t>;
    // as the campaign's soldiers have them, and the bodies with only tag_sync
    const Names soldier = {"tag_origin", "j_mainroot", "tag_aim", "tag_sync", "j_hip_le", "j_hip_ri", "j_spinelower"};
    const Parents soldierParents = {-1, 0, 0, 0, 1, 1, 1};
    CHECK(PoseLogic::FindSpareTag(soldier, soldierParents, 1, 6) == std::optional<std::size_t>(3));
    const Names worker = {"tag_origin", "j_mainroot", "tag_sync", "j_hip_le", "j_hip_ri", "j_spinelower"};
    const Parents workerParents = {-1, 0, 0, 1, 1, 1};
    CHECK(PoseLogic::FindSpareTag(worker, workerParents, 1, 5) == std::optional<std::size_t>(2));
    // any tag will do, one with a bone on it won't, nor one past the spine or before j_mainroot
    const Names aimOnly = {"tag_origin", "j_mainroot", "tag_aim", "j_spinelower", "tag_sync"};
    CHECK(PoseLogic::FindSpareTag(aimOnly, Parents{-1, 0, 0, 1, 0}, 1, 3) == std::optional<std::size_t>(2));
    CHECK(!PoseLogic::FindSpareTag(aimOnly, Parents{-1, 0, 0, 2, 0}, 1, 3).has_value());
    const Names none = {"tag_origin", "j_mainroot", "j_hip_le", "j_spinelower", "tag_sync"};
    CHECK(!PoseLogic::FindSpareTag(none, Parents{-1, 0, 1, 1, 0}, 1, 3).has_value());
}

TEST("animations are told apart by the bones they move: MP players', SP ones, upper bodies, faces and others")
{
    using Kind = Types::AnimationKind;
    using Family = Types::AnimationFamily;
    const auto classify = [](std::vector<std::string_view> bones) { return PoseLogic::ClassifyAnimation(bones); };
    const auto is = [](PoseLogic::AnimationRig rig, Kind kind, Family family) {
        return rig.kind == kind && rig.family == family;
    };
    // pb_stand_alert, pt_reload_stand_auto
    CHECK(is(classify({"j_mainroot", "j_hip_le", "torso_stabilizer", "j_spinelower", "j_knee_le", "j_head"}),
             Kind::FullBody, Family::MpPlayer));
    CHECK(is(classify({"j_spinelower", "j_spineupper", "j_spine4", "j_wrist_le", "tag_inhand"}), Kind::Torso,
             Family::MpPlayer));
    // an AI's (casual_stand_idle), and a body's without the spine
    CHECK(is(classify({"j_mainroot", "j_hip_le", "j_spinelower", "j_spineupper", "j_head", "tag_sync"}), Kind::FullBody,
             Family::SpHuman));
    CHECK(is(classify({"j_mainroot", "j_hip_ri", "j_knee_ri"}), Kind::FullBody, Family::Human));
    // pf_firing, f_death_v1; generic_lookupleft (the head alone)
    CHECK(is(classify({"j_jaw", "j_lip_top_le", "j_brow_le", "j_eyeball_le", "tag_eye", "j_cheekpuff_ri"}), Kind::Face,
             Family::Human));
    CHECK(is(classify({"j_forehead_le", "j_bridge", "j_nose_ri", "j_tongue", "j_chin_skinroll"}), Kind::Face,
             Family::Human));
    CHECK(is(classify({"j_neck", "j_head"}), Kind::Torso, Family::Human));
    // german_shepherd_run, viewmodel_ak74u_fire, fxanim_gp_windsock_anim
    CHECK(is(classify({"j_mainroot", "j_spine1", "j_hip_base_le", "j_tail1", "j_neck"}), Kind::Other, Family::Dog));
    CHECK(is(classify({"tag_torso", "j_shoulder_le", "j_elbow_le", "j_wrist_le", "j_gun", "tag_camera"}), Kind::Other,
             Family::Viewmodel));
    CHECK(is(classify({"windsock_jnt", "tag_origin"}), Kind::Other, Family::Other));
    CHECK(is(classify({}), Kind::Other, Family::Other));
    // the stabilizer without the body's root or legs is an upper body's
    CHECK(is(classify({"torso_stabilizer", "j_spinelower"}), Kind::Torso, Family::MpPlayer));
}
