#include "StdInclude.hpp"
#include "GripLogic.hpp"

#include "Components/ReloadLogic.hpp"

#include "glm/gtx/quaternion.hpp"

namespace IWXMVM::GripLogic
{
    namespace
    {
        // weapType_t and weapClass_t, as the game has them
        constexpr std::int32_t WEAPTYPE_GRENADE = 1;
        constexpr std::int32_t WEAPTYPE_PROJECTILE = 2;
        constexpr std::int32_t WEAPCLASS_MG = 1;
        constexpr std::int32_t WEAPCLASS_SPREAD = 3;
        constexpr std::int32_t WEAPCLASS_PISTOL = 4;
        constexpr std::int32_t WEAPCLASS_GRENADE = 5;

        // how close to the most or the least the arm can reach, as the game keeps a little bend in it
        constexpr float REACH_MARGIN = 0.05f;
        // less than this from the arm's line, the elbow has no side of its own
        constexpr float LEAST_SIDE = 0.01f;

        // the segment of the gun those animations hold it along, from behind the magazine to the fore-end
        constexpr glm::vec3 HOLD_FROM{-6.5f, 2.3f, -2.5f};
        constexpr glm::vec3 HOLD_TO{3.0f, 2.3f, -0.5f};

        glm::quat MakeWristTurn(const glm::vec3& xAxis, const glm::vec3& yAxis)
        {
            const auto x = glm::normalize(xAxis);
            const auto y = glm::normalize(yAxis - x * glm::dot(yAxis, x));
            return glm::normalize(glm::quat_cast(glm::mat3(x, y, glm::cross(x, y))));
        }

        // how a wrist is turned at PLAYER_WRIST and PISTOL_WRIST
        glm::quat GetPlayerWristTurn()
        {
            return MakeWristTurn({0.177f, -0.927f, 0.331f}, {-0.864f, 0.015f, 0.503f});
        }

        glm::quat GetPistolWristTurn()
        {
            return MakeWristTurn({0.728f, -0.685f, -0.015f}, {-0.427f, -0.471f, 0.772f});
        }

        // the rotation of angles of the game: yaw about z, then pitch about y, then roll about x
        glm::quat AnglesToQuat(const glm::vec3& angles)
        {
            return glm::angleAxis(glm::radians(angles.y), glm::vec3(0, 0, 1)) *
                   glm::angleAxis(glm::radians(angles.x), glm::vec3(0, 1, 0)) *
                   glm::angleAxis(glm::radians(angles.z), glm::vec3(1, 0, 0));
        }
    }  // namespace

    bool GunGrip::IsZero() const
    {
        return offset == glm::vec3(0.0f) && angles == glm::vec3(0.0f);
    }

    const GunGrip& PickGrip(const GunGrip& standing, const GunGrip& prone, bool lying)
    {
        return lying && !prone.IsZero() ? prone : standing;
    }

    GripKind GetGripKind(std::int32_t weapType, std::int32_t weapClass, bool offHand)
    {
        if (weapClass == WEAPCLASS_PISTOL)
            return GripKind::Pistol;
        if (weapClass == WEAPCLASS_MG)
            return GripKind::Gun;
        if (weapType == WEAPTYPE_GRENADE && offHand)
            return GripKind::Held;
        if (weapType == WEAPTYPE_PROJECTILE && weapClass != WEAPCLASS_GRENADE)
            return GripKind::Launcher;
        return weapType == WEAPTYPE_PROJECTILE || weapClass <= WEAPCLASS_SPREAD ? GripKind::Gun : GripKind::Other;
    }

    glm::vec3 GetGripSpot(GripKind kind)
    {
        switch (kind)
        {
            case GripKind::Launcher:
                return {-18.0f, 1.6f, 0.0f};
            case GripKind::Held:
                return {1.0f, 7.0f, 0.4f};
            default:
                return {-4.0f, 0.1f, -3.4f};
        }
    }

    float GetGripWeight(float distance)
    {
        if (distance <= GRIP_HOLDS)
            return 1.0f;
        if (distance >= GRIP_LETS_GO)
            return 0.0f;
        return 1.0f - (distance - GRIP_HOLDS) / (GRIP_LETS_GO - GRIP_HOLDS);
    }

    float GetHoldDistance(const glm::vec3& wristOnGun)
    {
        const auto along = HOLD_TO - HOLD_FROM;
        const auto at = std::clamp(glm::dot(wristOnGun - HOLD_FROM, along) / glm::dot(along, along), 0.0f, 1.0f);
        return glm::distance(wristOnGun, HOLD_FROM + along * at);
    }

    float GetCampaignShare(const ActorLogic::TreeWeights& weights,
                           const std::function<bool(std::string_view)>& isCampaign)
    {
        float total = 0.0f, campaign = 0.0f;
        for (const auto& entry : weights.animations)
        {
            total += entry.weight;
            if (isCampaign(entry.clip ? entry.clip->animation : entry.animation))
                campaign += entry.weight;
        }
        return total > 0.0f ? campaign / total : 0.0f;
    }

    float GetGripShare(const ActorLogic::TreeWeights& weights)
    {
        float aside = 0.0f;
        for (const auto& entry : weights.animations)
            if ((entry.clip && !entry.clip->grip) ||
                ReloadLogic::IsReloadName(entry.clip ? entry.clip->animation : entry.animation))
                aside += entry.weight;
        return 1.0f - std::clamp(aside, 0.0f, 1.0f);
    }

    std::optional<ArmMove> Solve(const BonePose& shoulder, const BonePose& elbow, const BonePose& wrist,
                                 const BonePose& gun, const GunGrip& grip, GripKind kind, const glm::vec3& wristShift,
                                 float share, float campaign)
    {
        // the wrist on the gun, as a soldier's would be
        const auto toGun = glm::inverse(gun.rotation);
        const auto wristOnGun = toGun * (wrist.position + wristShift - gun.position);
        const auto wristTurnOnGun = toGun * wrist.rotation;
        const auto hand = wristOnGun + HAND_POINT * (wristTurnOnGun * glm::vec3(1, 0, 0));
        // (a hand on other arms keeps to a soldier's through a reload, wherever it goes: the magazine is drawn on it)
        const bool soldier = wristShift == glm::vec3(0.0f);
        // (a place to go only for what's held as a gun: a launcher or a thing thrown keeps its spot's rule)
        const auto pistol = kind == GripKind::Pistol;
        const auto placed = kind == GripKind::Gun || pistol ? campaign : 0.0f;
        if (soldier && placed <= 0.0f && grip.IsZero())
            return std::nullopt;
        auto fade = glm::mix(GetGripWeight(glm::distance(hand, GetGripSpot(kind))),
                             GetGripWeight(GetHoldDistance(wristOnGun)), placed);
        if (!soldier)
            fade = glm::mix(fade, 1.0f, 1.0f - share);
        const auto weight = soldier ? fade * share : fade;
        if (weight < 0.01f)
            return std::nullopt;

        // the grip moves it along the gun's axes, and turns it in them
        // (or goes to where it is on a player, as much as what's shown is made for the AI and doesn't reload)
        const auto gripTurn = AnglesToQuat(grip.angles);
        const auto toPlace = placed * share;
        const auto heldOnGun = glm::mix(wristOnGun + grip.offset * share,
                                        pistol ? PISTOL_WRIST : PLAYER_WRIST + grip.offset, toPlace);
        const auto heldTurnOnGun = glm::slerp(glm::slerp(glm::quat(1, 0, 0, 0), gripTurn, share) * wristTurnOnGun,
                                              pistol ? GetPistolWristTurn() : gripTurn * GetPlayerWristTurn(), toPlace);
        const auto held = gun.position + gun.rotation * heldOnGun;
        const auto heldTurn = glm::normalize(gun.rotation * heldTurnOnGun);
        auto target = glm::mix(wrist.position, held, fade);
        const auto targetTurn = glm::normalize(glm::slerp(wrist.rotation, heldTurn, fade));

        const auto upperArm = elbow.position - shoulder.position;
        const auto forearm = wrist.position - elbow.position;
        const auto upperLength = glm::length(upperArm), foreLength = glm::length(forearm);
        const auto reachNow = wrist.position - shoulder.position;
        if (upperLength <= 0.0f || foreLength <= 0.0f || glm::length(reachNow) <= 0.0f)
            return std::nullopt;

        // as far as the arm reaches, on the way to it
        auto toTarget = target - shoulder.position;
        auto reach = glm::length(toTarget);
        if (reach <= 0.0f)
            return std::nullopt;
        const auto way = toTarget / reach;
        reach = std::clamp(reach, std::abs(upperLength - foreLength) + REACH_MARGIN,
                           upperLength + foreLength - REACH_MARGIN);
        target = shoulder.position + way * reach;

        // the arm bends about the shoulder's z axis, as the game bends it (within 0.13 units of its elbow), on the side
        // the elbow is on; from where the animation has the elbow as the grip fades in, as it fades in from nothing
        const auto wayNow = glm::normalize(reachNow);
        auto sideNow = upperArm - wayNow * glm::dot(upperArm, wayNow);
        sideNow -= way * glm::dot(sideNow, way);
        const auto hinge = shoulder.rotation * glm::vec3(0, 0, 1);
        auto side = glm::cross(hinge - way * glm::dot(hinge, way), way);
        if (glm::length(side) < LEAST_SIDE)
            side = sideNow;
        if (glm::length(side) < LEAST_SIDE || glm::length(sideNow) < LEAST_SIDE)
            return std::nullopt;
        side = glm::normalize(glm::dot(side, sideNow) < 0.0f ? -side : side);
        side = glm::normalize(glm::mix(glm::normalize(sideNow), side, weight));
        const auto along = (reach * reach + upperLength * upperLength - foreLength * foreLength) / (2.0f * reach);
        const auto out = std::sqrt(std::max(upperLength * upperLength - along * along, 0.0f));
        const auto elbowTo = shoulder.position + way * along + side * out;

        ArmMove move;
        move.weight = weight;
        move.upper = {shoulder.position, shoulder.position,
                      glm::rotation(upperArm / upperLength, glm::normalize(elbowTo - shoulder.position))};
        move.forearm = {elbow.position, elbowTo,
                        glm::normalize(glm::rotation(glm::normalize(move.upper.turn * forearm),
                                                     glm::normalize(target - elbowTo)) *
                                       move.upper.turn)};
        move.hand = {wrist.position, target, glm::normalize(targetTurn * glm::inverse(glm::normalize(wrist.rotation)))};
        move.twist = TWIST_SHARE * grip.angles.x * fade * share;
        if (toPlace > 0.0f)
        {
            // a hand that goes to a place turns by what it takes, not by the gun's pitch: the same share of its turn
            // about the forearm
            const auto axis = glm::normalize(target - elbowTo);
            const auto turned = move.hand.turn * glm::inverse(glm::normalize(move.forearm.turn));
            const auto about = 2.0f * std::atan2(glm::dot(glm::vec3(turned.x, turned.y, turned.z), axis), turned.w);
            const auto degrees = glm::degrees(std::remainder(about, glm::two_pi<float>()));
            move.twist = glm::mix(move.twist, -TWIST_SHARE * degrees, toPlace);
        }
        return move;
    }

    bool IsSoldierArm(const ArmRest& body, const ArmRest& soldier)
    {
        constexpr float ROUNDING = 0.02f;
        return glm::distance(body.clavicle, soldier.clavicle) < ROUNDING &&
               glm::distance(body.shoulder, soldier.shoulder) < ROUNDING &&
               glm::distance(body.elbow, soldier.elbow) < ROUNDING && glm::distance(body.wrist, soldier.wrist) < ROUNDING;
    }

    glm::vec3 GetWristShift(const ArmRest& left, const ArmParents& leftParents, const ArmRest& right,
                            const ArmParents& rightParents)
    {
        const auto along = [](const ArmRest& soldier, const ArmRest& body, const ArmParents& parents) {
            return parents.spine * (soldier.clavicle - body.clavicle) + parents.clavicle * (soldier.shoulder - body.shoulder) +
                   parents.shoulder * (soldier.elbow - body.elbow) + parents.elbow * (soldier.wrist - body.wrist);
        };
        return along(SOLDIER_LEFT_ARM, left, leftParents) - along(SOLDIER_RIGHT_ARM, right, rightParents);
    }

    bool IsSoldierGunTag(const glm::vec3& rest)
    {
        constexpr float ROUNDING = 0.1f;
        return glm::distance(rest, SOLDIER_GUN_TAG) < ROUNDING;
    }
}  // namespace IWXMVM::GripLogic
