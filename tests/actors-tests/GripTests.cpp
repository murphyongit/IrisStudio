#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/GripLogic.hpp"

using namespace IWXMVM;
using namespace IWXMVM::GripLogic;

// The left hand on a gun. The numbers are a real player's, from the game: the left
// arm on tag_weapon_right with the game's IK off, and with it on (ak47_mp, famas_mp, ak74u_mp with a grip)

namespace
{
    // the arm of an animation that holds a rifle, on tag_weapon_right; the gun's own bone is the model here
    const glm::vec3 SHOULDER(-17.42f, 12.23f, -0.15f);
    const glm::vec3 ELBOW(-9.41f, 10.0f, -8.45f);
    const glm::vec3 WRIST(-4.41f, 2.24f, -4.16f);
    const glm::vec3 WRIST_X(0.171f, -0.928f, 0.33f);
    const glm::vec3 WRIST_Y(-0.864f, 0.02f, 0.504f);

    bool Near(const glm::vec3& a, const glm::vec3& b, float tolerance = 0.02f)
    {
        return glm::distance(a, b) <= tolerance;
    }

    glm::quat WristTurn()
    {
        const auto x = glm::normalize(WRIST_X);
        const auto y = glm::normalize(WRIST_Y - x * glm::dot(WRIST_Y, x));
        return glm::quat_cast(glm::mat3(x, y, glm::cross(x, y)));
    }

    std::optional<ArmMove> SolveArm(const GunGrip& grip, const BonePose& gun = {},
                                    const glm::vec3& wrist = WRIST, GripKind kind = GripKind::Gun)
    {
        const auto at = [&](const glm::vec3& position) {
            return BonePose{gun.position + gun.rotation * position, gun.rotation * glm::quat(1, 0, 0, 0)};
        };
        return Solve(at(SHOULDER), at(ELBOW), {gun.position + gun.rotation * wrist, gun.rotation * WristTurn()}, gun,
                     grip, kind);
    }
}  // namespace

TEST("grip: the wrist goes the gun's offset from where the animation has it")
{
    // the real AK: wrist (-4.41 2.24 -4.16) to (-0.41 2.24 -2.16)
    const auto move = SolveArm({{4.0f, 0.0f, 2.0f}, {50.0f, 0.0f, 0.0f}});
    CHECK(move.has_value());
    if (!move)
        return;
    CHECK(move->weight == 1.0f);
    CHECK(Near(move->hand.to, glm::vec3(-0.41f, 2.24f, -2.16f)));
}

TEST("grip: the hand turns as the game turns it")
{
    // the real AK's wrist turns to x (0.363 -0.928 0.082), y (-0.169 0.020 0.985)
    const auto move = SolveArm({{4.0f, 0.0f, 2.0f}, {50.0f, 0.0f, 0.0f}});
    CHECK(move.has_value());
    if (!move)
        return;
    const auto turned = glm::normalize(move->hand.turn * WristTurn());
    CHECK(Near(turned * glm::vec3(1, 0, 0), glm::vec3(0.363f, -0.928f, 0.082f), 0.01f));
    CHECK(Near(turned * glm::vec3(0, 1, 0), glm::vec3(-0.169f, 0.02f, 0.985f), 0.01f));
}

TEST("grip: yaw and roll turn it too")
{
    // ak74u_grip_mp: pitch -30, yaw 60, roll 20 turns x to (0.931 -0.356 0.083), y to (-0.356 -0.934 -0.018)
    const auto start = glm::normalize(glm::vec3(0.177f, -0.927f, 0.331f));
    const auto second = glm::vec3(-0.864f, 0.015f, 0.503f);
    const auto y = glm::normalize(second - start * glm::dot(second, start));
    const auto wrist = glm::quat_cast(glm::mat3(start, y, glm::cross(start, y)));
    const auto move = Solve({SHOULDER}, {ELBOW}, {WRIST, wrist}, {}, {{3.5f, 0.0f, -1.0f}, {-30.0f, 60.0f, 20.0f}},
                            GripKind::Gun);
    CHECK(move.has_value());
    if (!move)
        return;
    const auto turned = glm::normalize(move->hand.turn * wrist);
    CHECK(Near(turned * glm::vec3(1, 0, 0), glm::vec3(0.931f, -0.356f, 0.083f), 0.01f));
    CHECK(Near(turned * glm::vec3(0, 1, 0), glm::vec3(-0.356f, -0.934f, -0.018f), 0.01f));
}

TEST("grip: the arm keeps its lengths and its shoulder, and the elbow is about where the game has it")
{
    const auto move = SolveArm({{4.0f, 0.0f, 2.0f}, {50.0f, 0.0f, 0.0f}});
    CHECK(move.has_value());
    if (!move)
        return;
    CHECK(Near(move->upper.to, SHOULDER));
    const auto elbow = move->forearm.to;
    CHECK(std::abs(glm::distance(elbow, SHOULDER) - glm::distance(ELBOW, SHOULDER)) < 0.01f);
    CHECK(std::abs(glm::distance(elbow, move->hand.to) - glm::distance(ELBOW, WRIST)) < 0.01f);
}

TEST("grip: the elbow goes where the game's goes, bent about the shoulder's z axis")
{
    // a real player with ak74u_grip_mp, with the game's IK off, then on
    const auto x = glm::normalize(glm::vec3(0.24f, -0.021f, -0.971f));
    const auto z = glm::normalize(glm::vec3(0.746f, 0.644f, 0.17f));
    const auto shoulder = glm::quat_cast(glm::mat3(x, glm::cross(z, x), z));
    const auto move = Solve({glm::vec3(-12.32f, 9.51f, 1.99f), shoulder}, {glm::vec3(-9.63f, 9.68f, -9.44f)},
                            {glm::vec3(-5.51f, 1.95f, -4.25f), WristTurn()}, {},
                            {{3.5f, 0.0f, -1.0f}, {-30.0f, 60.0f, 20.0f}}, GripKind::Gun);
    CHECK(move.has_value());
    if (!move)
        return;
    CHECK(Near(move->hand.to, glm::vec3(-2.01f, 1.95f, -5.25f)));
    CHECK(Near(move->forearm.to, glm::vec3(-8.22f, 9.09f, -9.0f), 0.1f));
}

TEST("grip: each part of the arm moves as one, and they meet")
{
    const auto move = SolveArm({{4.0f, 0.0f, 2.0f}, {50.0f, 0.0f, 0.0f}});
    CHECK(move.has_value());
    if (!move)
        return;
    // the elbow is where the upper arm's move puts it, the wrist where the forearm's does
    CHECK(Near(Move(move->upper, ELBOW), move->forearm.to));
    CHECK(Near(Move(move->forearm, WRIST), move->hand.to));
    CHECK(Near(Move(move->hand, WRIST), move->hand.to));
}

TEST("grip: it works wherever the gun is and however it is turned")
{
    const BonePose gun{{12.0f, -30.0f, 41.0f}, glm::angleAxis(glm::radians(70.0f), glm::normalize(glm::vec3(1, 2, 3)))};
    const auto move = SolveArm({{4.0f, 0.0f, 2.0f}, {50.0f, 0.0f, 0.0f}}, gun);
    CHECK(move.has_value());
    if (!move)
        return;
    CHECK(Near(glm::inverse(gun.rotation) * (move->hand.to - gun.position), glm::vec3(-0.41f, 2.24f, -2.16f), 0.05f));
}

TEST("grip: a gun with no grip leaves the hand where it is")
{
    CHECK(!SolveArm({}).has_value());
}

TEST("grip: as it fades out the whole arm comes back to the animation's")
{
    // a hand all but out of reach of the grip
    const auto spot = GetGripSpot(GripKind::Gun);
    const auto x = WristTurn() * glm::vec3(1, 0, 0);
    const auto wrist = spot - HAND_POINT * x + glm::normalize(glm::vec3(0, 1, 0)) * (GRIP_LETS_GO - 0.1f);
    const auto move = Solve({SHOULDER}, {ELBOW}, {wrist, WristTurn()}, {}, {{4.0f, 0.0f, 2.0f}, {}}, GripKind::Gun);
    CHECK(move.has_value());
    if (!move)
        return;
    CHECK(move->weight < 0.03f);
    CHECK(Near(move->hand.to, wrist, 0.1f));
    CHECK(Near(move->forearm.to, ELBOW, 0.2f));
}

TEST("grip: it lets go as the hand leaves the spot")
{
    CHECK(GetGripWeight(0.0f) == 1.0f);
    CHECK(GetGripWeight(GRIP_HOLDS) == 1.0f);
    CHECK(std::abs(GetGripWeight(5.5f) - 0.5f) < 0.001f);
    CHECK(GetGripWeight(GRIP_LETS_GO) == 0.0f);
    CHECK(GetGripWeight(100.0f) == 0.0f);
}

TEST("grip: a hand away from the gun has none")
{
    // as a reload's hand at a pouch
    CHECK(!SolveArm({{4.0f, 0.0f, 2.0f}, {50.0f, 0.0f, 0.0f}}, {}, glm::vec3(-20.0f, 14.0f, -15.0f)).has_value());
}

TEST("grip: half of the grip is half of the offset")
{
    // the hand some way off the spot
    const auto spotted = WRIST + glm::vec3(5.0f, 0.0f, 0.0f);
    const auto distance = glm::distance(spotted + HAND_POINT * (WristTurn() * glm::vec3(1, 0, 0)), GetGripSpot(GripKind::Gun));
    const auto move = SolveArm({{4.0f, 0.0f, 2.0f}, {}}, {}, spotted);
    CHECK(move.has_value());
    if (!move)
        return;
    CHECK(move->weight > 0.1f && move->weight < 0.9f);
    CHECK(std::abs(move->weight - GetGripWeight(distance)) < 0.001f);
    CHECK(Near(move->hand.to, spotted + move->weight * glm::vec3(4.0f, 0.0f, 2.0f), 0.05f));
}

TEST("grip: no farther than the arm reaches")
{
    const auto move = Solve({SHOULDER}, {ELBOW}, {WRIST, WristTurn()}, {}, {{40.0f, 0.0f, 0.0f}, {}}, GripKind::Gun);
    CHECK(move.has_value());
    if (!move)
        return;
    const auto reach = glm::distance(ELBOW, SHOULDER) + glm::distance(ELBOW, WRIST);
    CHECK(glm::distance(move->hand.to, SHOULDER) <= reach);
    CHECK(glm::distance(move->hand.to, SHOULDER) > reach - 0.1f);
}

TEST("grip: the forearm twists by the pitch")
{
    // real players: pitch 40 turns j_wristtwist_le -36 degrees, pitch -30 turns it 27
    CHECK(std::abs(SolveArm({{}, {40.0f, 0.0f, 0.0f}})->twist + 36.0f) < 0.01f);
    CHECK(std::abs(SolveArm({{}, {-30.0f, 60.0f, 20.0f}})->twist - 27.0f) < 0.01f);
}

TEST("grip: prone only where the weapon file has one")
{
    const GunGrip standing{{2.0f, 0.0f, 2.0f}, {40.0f, 0.0f, 0.0f}}, prone{{2.0f, 0.0f, 2.5f}, {40.0f, 0.0f, 0.0f}};
    CHECK(&PickGrip(standing, prone, false) == &standing);
    CHECK(&PickGrip(standing, prone, true) == &prone);
    // (a machine gun's are all 0)
    const GunGrip none;
    CHECK(&PickGrip(standing, none, true) == &standing);
}

TEST("grip: the spot goes by the kind of gun")
{
    CHECK(GetGripKind(0, 0, false) == GripKind::Gun);    // a rifle
    CHECK(GetGripKind(0, 3, false) == GripKind::Gun);    // a shotgun
    CHECK(GetGripKind(0, 4, false) == GripKind::Pistol);
    CHECK(GetGripKind(2, 6, false) == GripKind::Launcher);
    CHECK(GetGripKind(2, 0, false) == GripKind::Launcher);  // the crossbow
    CHECK(GetGripKind(2, 4, false) == GripKind::Pistol);    // the ballistic knife, a pistol to the game
    CHECK(GetGripKind(2, 5, false) == GripKind::Gun);    // the grenade launcher attachments
    CHECK(GetGripKind(1, 5, true) == GripKind::Held);
    CHECK(GetGripKind(1, 5, false) == GripKind::Other);  // a claymore in the hand
    CHECK(GetGripKind(0, 11, false) == GripKind::Other);  // the knife
    CHECK(Near(GetGripSpot(GripKind::Gun), glm::vec3(-4.0f, 0.1f, -3.4f)));
    CHECK(Near(GetGripSpot(GripKind::Pistol), GetGripSpot(GripKind::Gun)));
}

TEST("grip: a soldier's arms need no shift")
{
    CHECK(IsSoldierArm(SOLDIER_LEFT_ARM, SOLDIER_LEFT_ARM));
    const auto parents = ArmParents{};
    CHECK(Near(GetWristShift(SOLDIER_LEFT_ARM, parents, SOLDIER_RIGHT_ARM, parents), glm::vec3(0.0f)));
}

TEST("grip: a woman's hand is shifted to where a soldier's would be")
{
    // the Pentagon's women's left arm, on bones lined up with the model: only the lengths count
    const ArmRest woman{{6.151f, -0.166f, 1.357f}, {5.168f, 0.0f, 0.0f}, {10.457f, 0.0f, 0.0f}, {10.179f, -0.051f, -0.179f}};
    CHECK(!IsSoldierArm(woman, SOLDIER_LEFT_ARM));
    const auto shift = GetWristShift(woman, {}, SOLDIER_RIGHT_ARM, {});
    const auto expected = (SOLDIER_LEFT_ARM.clavicle - woman.clavicle) + (SOLDIER_LEFT_ARM.shoulder - woman.shoulder) +
                          (SOLDIER_LEFT_ARM.elbow - woman.elbow) + (SOLDIER_LEFT_ARM.wrist - woman.wrist);
    CHECK(Near(shift, expected));
    // the right arm counts the other way, as the gun hangs from it
    ArmRest right = SOLDIER_RIGHT_ARM;
    right.shoulder.y -= 1.0f;
    CHECK(Near(GetWristShift(SOLDIER_LEFT_ARM, {}, right, {}), glm::vec3(0.0f, -1.0f, 0.0f)));
}

TEST("grip: the shift turns with the bones")
{
    ArmRest shorter = SOLDIER_LEFT_ARM;
    shorter.elbow.x -= 1.0f;
    ArmParents parents;
    parents.shoulder = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    CHECK(Near(GetWristShift(shorter, parents, SOLDIER_RIGHT_ARM, {}), glm::vec3(0.0f, 1.0f, 0.0f)));
}

TEST("grip: the solve goes by the shifted hand")
{
    // the wrist 3 units short of a soldier's: the grip and its fade go by a soldier's, the hand ends where his would
    const auto wrist = WRIST - glm::vec3(3.0f, 0.0f, 0.0f);
    const auto move = Solve({SHOULDER}, {ELBOW}, {wrist, WristTurn()}, {}, {{4.0f, 0.0f, 2.0f}, {}}, GripKind::Gun,
                            glm::vec3(3.0f, 0.0f, 0.0f));
    CHECK(move.has_value());
    if (!move)
        return;
    CHECK(move->weight == 1.0f);
    CHECK(Near(move->hand.to, WRIST + glm::vec3(4.0f, 0.0f, 2.0f), 0.05f));
}

TEST("grip: a soldier's gun tag")
{
    CHECK(IsSoldierGunTag(SOLDIER_GUN_TAG));
    CHECK(!IsSoldierGunTag(glm::vec3(-10.858f, 2.997f, -8.502f)));  // the Pentagon's women's
}

TEST("grip: a reload clip stands the grip aside as it blends in")
{
    ActorLogic::TreeWeights weights;
    CHECK(GetGripShare(weights) == 1.0f);
    weights.animations.push_back({.animation = "pb_stand_alert", .weight = 1.0f});
    CHECK(GetGripShare(weights) == 1.0f);
    Types::ActorClip reload{.animation = "pt_reload_stand_auto"};
    weights.animations.push_back({.clip = &reload, .weight = 0.25f});
    CHECK(std::abs(GetGripShare(weights) - 0.75f) < 0.001f);
    weights.animations[1].weight = 1.0f;
    CHECK(GetGripShare(weights) == 0.0f);
}

TEST("grip: a clip with its Left hand off stands the grip aside, the actor's other clips keep it")
{
    Types::ActorClip own{.animation = "crouch_alert_a_twitch", .grip = false};
    Types::ActorClip held{.animation = "casual_crouch_twitch"};
    ActorLogic::TreeWeights weights;
    weights.animations.push_back({.clip = &held, .weight = 1.0f});
    CHECK(GetGripShare(weights) == 1.0f);
    // as it blends in from the clip before, and once it is all that shows
    weights.animations[0].weight = 0.75f;
    weights.animations.push_back({.clip = &own, .weight = 0.25f});
    CHECK(std::abs(GetGripShare(weights) - 0.75f) < 0.001f);
    weights.animations = {{.clip = &own, .weight = 1.0f}};
    CHECK(GetGripShare(weights) == 0.0f);
    // its hand is the animation's: no grip, no place
    const GunGrip ak{{4.0f, 0.0f, 2.0f}, {50.0f, 0.0f, 0.0f}};
    CHECK(!Solve({SHOULDER}, {ELBOW}, {WRIST, WristTurn()}, {}, ak, GripKind::Gun, glm::vec3(0.0f),
                 GetGripShare(weights), 1.0f));
    // the legs' idle under it, a hundredth of it, changes nothing
    weights.animations.push_back({.animation = "casual_crouch_idle", .weight = 0.01f});
    CHECK(GetGripShare(weights) == 0.0f);
    // a reload with it off is aside once, not twice
    Types::ActorClip reload{.animation = "pt_reload_stand_auto", .grip = false};
    weights.animations = {{.clip = &reload, .weight = 0.5f}, {.clip = &held, .weight = 0.5f}};
    CHECK(std::abs(GetGripShare(weights) - 0.5f) < 0.001f);
}

TEST("grip: its share scales the grip")
{
    const auto half = Solve({SHOULDER}, {ELBOW}, {WRIST, WristTurn()}, {}, {{4.0f, 0.0f, 2.0f}, {}}, GripKind::Gun,
                            glm::vec3(0.0f), 0.5f);
    CHECK(half.has_value());
    if (half)
    {
        CHECK(half->weight == 0.5f);
        CHECK(Near(half->hand.to, WRIST + glm::vec3(2.0f, 0.0f, 1.0f), 0.05f));
    }
    CHECK(!Solve({SHOULDER}, {ELBOW}, {WRIST, WristTurn()}, {}, {{4.0f, 0.0f, 2.0f}, {}}, GripKind::Gun, glm::vec3(0.0f),
                 0.0f)
               .has_value());
}

TEST("grip: other arms stay on a soldier's spot through a reload")
{
    const auto wrist = WRIST - glm::vec3(3.0f, 0.0f, 0.0f);
    const auto move = Solve({SHOULDER}, {ELBOW}, {wrist, WristTurn()}, {}, {{4.0f, 0.0f, 2.0f}, {50.0f, 0.0f, 0.0f}},
                            GripKind::Gun, glm::vec3(3.0f, 0.0f, 0.0f), 0.0f);
    CHECK(move.has_value());
    if (!move)
        return;
    CHECK(Near(move->hand.to, WRIST, 0.05f));
    CHECK(std::abs(move->twist) < 0.001f);

    // however far from the gun the reload takes it
    const auto away = WRIST + glm::vec3(-3.0f, 9.0f, -6.0f);
    const auto reached = away - glm::vec3(3.0f, 0.0f, 0.0f);
    const auto far = Solve({SHOULDER}, {reached + glm::vec3(0.0f, 0.0f, 9.0f)}, {reached, WristTurn()}, {}, {},
                           GripKind::Gun, glm::vec3(3.0f, 0.0f, 0.0f), 0.0f);
    CHECK(far.has_value());
    if (far)
        CHECK(Near(far->hand.to, away, 0.05f));
}

// casual_crouch_idle, as the survey of the AI's clips has it on the gun
namespace
{
    const glm::vec3 AI_WRIST(-3.77f, 2.71f, -0.97f);

    glm::quat AiWristTurn()
    {
        const auto x = glm::normalize(glm::vec3(0.61f, -0.74f, -0.27f));
        const auto side = glm::vec3(-0.32f, -0.54f, 0.78f);
        const auto y = glm::normalize(side - x * glm::dot(side, x));
        return glm::quat_cast(glm::mat3(x, y, glm::cross(x, y)));
    }

    std::optional<ArmMove> SolveAi(const GunGrip& grip, float campaign, float share = 1.0f,
                                   const glm::vec3& wrist = AI_WRIST, GripKind kind = GripKind::Gun)
    {
        return Solve({SHOULDER}, {ELBOW}, {wrist, AiWristTurn()}, {}, grip, kind, glm::vec3(0.0f), share, campaign);
    }

    const GunGrip AK{{4.0f, 0.0f, 2.0f}, {50.0f, 0.0f, 0.0f}};
}  // namespace

TEST("grip: a clip made for the AI goes to the place a player's hand goes, not the offset on top of its own")
{
    const auto move = SolveAi(AK, 1.0f);
    CHECK(move.has_value());
    if (!move)
        return;
    CHECK(Near(move->hand.to, PLAYER_WRIST + AK.offset));
    // the hand a real player has on an AK
    const auto turned = glm::normalize(move->hand.turn * AiWristTurn());
    CHECK(Near(turned * glm::vec3(1, 0, 0), glm::vec3(0.363f, -0.928f, 0.082f), 0.01f));
    CHECK(Near(turned * glm::vec3(0, 1, 0), glm::vec3(-0.169f, 0.02f, 0.985f), 0.01f));
}

TEST("grip: the old rule on that clip is the palm up the owner saw, and each gun goes to its own place")
{
    const auto over = SolveAi(AK, 0.0f);
    CHECK(over.has_value());
    if (over)
    {
        // 50 degrees on top of a hand already turned for a rifle: its palm faces up, 90 degrees or more from a player's
        const auto turned = glm::normalize(over->hand.turn * AiWristTurn());
        const auto player = SolveAi(AK, 1.0f);
        const auto toPlayer = glm::normalize(turned * glm::inverse(player->hand.turn * AiWristTurn()));
        const auto angle = glm::degrees(glm::angle(toPlayer));
        CHECK(angle > 40.0f);
    }
    const GunGrip short_gun{{1.2f, 0.0f, 0.0f}, {-42.0f, 58.0f, 0.0f}};
    const auto mp5k = SolveAi(short_gun, 1.0f);
    CHECK(mp5k.has_value());
    if (mp5k)
        CHECK(Near(mp5k->hand.to, PLAYER_WRIST + short_gun.offset));
}

TEST("grip: a campaign clip's far forward hand comes back to the gun")
{
    // crouch_alert_a_twitch has it at x 1.65, in the air on a short gun
    const auto move = SolveAi({{1.2f, 0.0f, 0.0f}, {-42.0f, 58.0f, 0.0f}}, 1.0f, 1.0f, glm::vec3(1.65f, 2.51f, 0.04f));
    CHECK(move.has_value());
    if (move)
        CHECK(Near(move->hand.to, PLAYER_WRIST + glm::vec3(1.2f, 0.0f, 0.0f)));
}

TEST("grip: a campaign clip's hand off the gun is left to it")
{
    // a signal, up by the head
    CHECK(!SolveAi(AK, 1.0f, 1.0f, glm::vec3(-20.0f, 14.0f, 12.0f)).has_value());
    CHECK(GetHoldDistance(AI_WRIST) < 3.0f);
    CHECK(GetHoldDistance(glm::vec3(1.65f, 2.51f, 0.04f)) < 3.0f);
    CHECK(GetHoldDistance(glm::vec3(-12.8f, 6.4f, 0.1f)) > 5.0f);  // a pistol's hand on the other wrist
}

TEST("grip: a campaign reload stands aside as players' do")
{
    CHECK(!SolveAi(AK, 1.0f, 0.0f).has_value());
}

TEST("grip: between a player's clip and a campaign one the hand goes as they blend")
{
    const auto half = SolveAi(AK, 0.5f);
    const auto own = SolveAi(AK, 0.0f);
    const auto place = SolveAi(AK, 1.0f);
    CHECK(half.has_value() && own.has_value() && place.has_value());
    if (half && own && place)
        CHECK(Near(half->hand.to, 0.5f * (own->hand.to + place->hand.to), 0.05f));
}

TEST("grip: a gun whose file has no grip goes to the spot itself")
{
    const auto move = SolveAi({}, 1.0f);
    CHECK(move.has_value());
    if (move)
        CHECK(Near(move->hand.to, PLAYER_WRIST));
}

TEST("grip: a pistol held as a rifle has the left hand around the right, as the players' clips do")
{
    const auto move = SolveAi({}, 1.0f, 1.0f, AI_WRIST, GripKind::Pistol);
    CHECK(move.has_value());
    if (!move)
        return;
    CHECK(Near(move->hand.to, PISTOL_WRIST));
    const auto turned = glm::normalize(move->hand.turn * AiWristTurn());
    CHECK(Near(turned * glm::vec3(1, 0, 0), glm::vec3(0.728f, -0.685f, -0.015f), 0.01f));
    CHECK(Near(turned * glm::vec3(0, 1, 0), glm::vec3(-0.427f, -0.471f, 0.772f), 0.01f));

    // the AI's own pistol clips have it elsewhere, and keep it
    CHECK(!SolveAi({}, 1.0f, 1.0f, glm::vec3(-20.0f, 14.0f, 12.0f), GripKind::Pistol).has_value());
    // as does a reload
    CHECK(!SolveAi({}, 1.0f, 0.0f, AI_WRIST, GripKind::Pistol).has_value());
}

TEST("grip: on the players' clips a gun with no grip isn't touched")
{
    // not even the elbow, which a solve would put on the game's bend
    CHECK(!SolveAi({}, 0.0f).has_value());
    CHECK(!SolveAi({}, 0.0f, 1.0f, AI_WRIST, GripKind::Pistol).has_value());
}

TEST("grip: what isn't held as a gun keeps the rule of the spot")
{
    CHECK(!SolveAi(AK, 1.0f, 1.0f, AI_WRIST, GripKind::Launcher).has_value());
    CHECK(!SolveAi({}, 1.0f, 1.0f, AI_WRIST, GripKind::Other).has_value());
    // a thing held, near its spot: some of the offset on top, as ever, and no place
    const auto held = SolveAi(AK, 1.0f, 1.0f, AI_WRIST, GripKind::Held);
    CHECK(held.has_value());
    if (held)
        CHECK(glm::distance(held->hand.to, PLAYER_WRIST + AK.offset) > 1.0f);
}

TEST("grip: the forearm twists by what the hand turns about it")
{
    // the way the game turns it for a pitch, and more, as the forearm turns too
    const auto move =
        Solve({SHOULDER}, {ELBOW}, {WRIST, WristTurn()}, {}, AK, GripKind::Gun, glm::vec3(0.0f), 1.0f, 1.0f);
    CHECK(move.has_value());
    if (move)
        CHECK(move->twist < -30.0f && move->twist > -90.0f);
}

TEST("grip: the campaign's share of what's shown goes by the weights")
{
    ActorLogic::TreeWeights weights;
    const auto isAi = [](std::string_view name) { return name == "casual_stand_idle"; };
    CHECK(GetCampaignShare(weights, isAi) == 0.0f);
    weights.animations.push_back({.animation = "casual_stand_idle", .weight = 1.0f});
    CHECK(GetCampaignShare(weights, isAi) == 1.0f);
    // an upper body clip of the players' over its legs: the legs weigh next to nothing
    weights.animations[0].weight = 0.01f;
    weights.animations.push_back({.animation = "pt_stand_shoot", .weight = 1.0f});
    CHECK(GetCampaignShare(weights, isAi) < 0.02f);
}

TEST("grip: lying down goes by what the actor shows")
{
    ActorLogic::TreeWeights weights;
    weights.animations.push_back({.animation = "pb_prone_aim_ads", .weight = 0.7f});
    weights.animations.push_back({.animation = "pb_stand_alert", .weight = 0.3f});
    CHECK(ActorLogic::IsProneShown(weights));
    weights.animations[0].weight = 0.2f;
    CHECK(!ActorLogic::IsProneShown(weights));
}
