#pragma once
#include "Components/ActorLogic.hpp"

namespace IWXMVM::GripLogic
{
    // The left hand on a gun, as the game has it for its players (52A2D0 in the exe). All
    // the animations of a gun held in both hands keep the left wrist at one spot of tag_weapon_right, whichever gun it
    // is, and the weapon file says how far the hand goes from there along the gun and how it turns (ikLeftHandOffset
    // and ikLeftHandRotation): an AK's by the handguard, a short SMG's close in. The offset is added to where the
    // animation has the wrist, so a reload, which takes the hand off the gun, needs no grip of its own: the grip fades
    // out as the hand leaves that spot. The arm then bends to reach it, about the shoulder's z axis, the shoulder fixed
    // and the lengths kept. Nothing here keeps a state between frames, so a rewind or a jump gives the same hand.

    // the weapon file's grip, along tag_weapon_right's axes: forward, right and up, and pitch, yaw and roll
    struct GunGrip
    {
        glm::vec3 offset{0.0f};
        glm::vec3 angles{0.0f};
        bool IsZero() const;
    };
    // the grip a stance uses: lying down the prone one, unless the weapon file has none
    const GunGrip& PickGrip(const GunGrip& standing, const GunGrip& prone, bool lying);

    // What the spot on the gun is, as the game sorts its guns
    enum class GripKind
    {
        Gun,       // rifles, SMGs, shotguns and machine guns
        Pistol,    // pistols, and the ballistic knife, a pistol to the game
        Launcher,  // what fires a projectile but isn't a grenade launcher
        Held,      // things thrown or held in the hand that have an animation of their own for it
        Other,     // the rest, what no one holds as a gun
    };
    // which of them a gun is, from its weapon file: weapType, weapClass, and whether it has an off hand use
    GripKind GetGripKind(std::int32_t weapType, std::int32_t weapClass, bool offHand);
    // the spot, a point of the hand (HAND_POINT along the wrist's x axis) on tag_weapon_right
    glm::vec3 GetGripSpot(GripKind kind);
    inline constexpr float HAND_POINT = 2.3f;
    // how far the hand is off that spot where the grip starts to let go, and where it has
    inline constexpr float GRIP_HOLDS = 3.0f;
    inline constexpr float GRIP_LETS_GO = 8.0f;
    // how much of the grip holds with the hand that far from the spot
    float GetGripWeight(float distance);

    // Animations made for the campaign's AI don't keep that spot. They place the hand themselves, each in its own way
    // and mostly already turned as a rifle's is (914 of the clips at (-3.81, 2.11, -2.00), 46 degrees from the spot and
    // 14 from an AK's hold; 60% of the held samples at no shared place), so a gun's offset and turn on top overdo it: a
    // palm up, fingers through the handguard. On those, while the hand is on the gun at all, it goes to the place a
    // player's hand goes on that gun instead: where the spot and the gun's grip put it. A gun whose file has no grip
    // stays at the spot, as on a player. Pistols have no grip to go by, and those animations hold them as a rifle, the
    // left hand out in the air: it goes where the players' pistol animations have it, around the right.
    inline constexpr glm::vec3 PLAYER_WRIST{-4.46f, 2.25f, -4.16f};
    inline constexpr glm::vec3 PISTOL_WRIST{-12.42f, 2.20f, -4.76f};
    // how far a wrist on the gun is from where those animations hold it, along it from the magazine to the fore-end
    float GetHoldDistance(const glm::vec3& wristOnGun);
    // how much of what the actor shows is animations made for the AI (isCampaign tells which), as a share of all of it
    float GetCampaignShare(const ActorLogic::TreeWeights& weights,
                           const std::function<bool(std::string_view)>& isCampaign);

    // How much of the grip an actor shows, by what it plays: its reloads have the hand on the gun's magazine, which actors
    // draw in the hand (the game's players leave it on the gun, so the grip can pull their hand off it), so the
    // grip stands aside as a reload clip blends in, and comes back as it blends out. So it does for a clip whose Left
    // hand is off (ActorClip::grip), which then has the hand as its animation has it.
    float GetGripShare(const ActorLogic::TreeWeights& weights);

    // the share of a pitch of the grip that j_wristtwist_le turns the other way, along the forearm
    inline constexpr float TWIST_SHARE = -0.9f;

    // A bone in the model
    struct BonePose
    {
        glm::vec3 position{0.0f};
        glm::quat rotation{1, 0, 0, 0};
    };

    // How the bones of each part of the arm move: a bone at p goes to to + turn * (p - from), and turns by turn
    struct PartMove
    {
        glm::vec3 from{0.0f};
        glm::vec3 to{0.0f};
        glm::quat turn{1, 0, 0, 0};
    };
    inline glm::vec3 Move(const PartMove& part, const glm::vec3& position)
    {
        return part.to + part.turn * (position - part.from);
    }

    struct ArmMove
    {
        PartMove upper;    // j_shoulder_le and what hangs from it, but the forearm
        PartMove forearm;  // j_elbow_le and what hangs from it, but the hand
        PartMove hand;     // j_wrist_le and what hangs from it
        float twist = 0.0f;   // degrees j_wristtwist_le turns about its x axis, on top of the forearm's move
        float weight = 0.0f;  // how much of the grip it is
    };

    // Bends the arm toward the grip; none when the hand is too far from the gun for the grip to hold. All the bones are
    // in the model. wristShift is how far the hand is from where a soldier's arms would have it (see GetWristShift),
    // share how much of the grip to show (see GetGripShare), campaign how much of what's shown is made for the AI (see
    // GetCampaignShare).
    std::optional<ArmMove> Solve(const BonePose& shoulder, const BonePose& elbow, const BonePose& wrist,
                                 const BonePose& gun, const GunGrip& grip, GripKind kind,
                                 const glm::vec3& wristShift = glm::vec3(0.0f), float share = 1.0f,
                                 float campaign = 0.0f);

    // The animations were made on a soldier's arms. Bodies with other arms (the campaign's women) put the hands
    // elsewhere from the same rotations, and the left hand misses the gun, so the grip is worked out from where a
    // soldier's would be. Where each bone of an arm rests from the bone it hangs from:
    struct ArmRest
    {
        glm::vec3 clavicle{0.0f};
        glm::vec3 shoulder{0.0f};
        glm::vec3 elbow{0.0f};
        glm::vec3 wrist{0.0f};
    };
    inline constexpr ArmRest SOLDIER_LEFT_ARM{{4.639f, -0.166f, 1.357f},
                                              {6.387f, 0.0f, 0.0f},
                                              {11.738f, 0.403f, 0.179f},
                                              {10.179f, -0.051f, -0.179f}};
    inline constexpr ArmRest SOLDIER_RIGHT_ARM{{4.639f, -0.166f, -1.357f},
                                               {-6.387f, 0.0f, 0.0f},
                                               {-11.738f, -0.403f, -0.179f},
                                               {-10.179f, 0.05f, 0.179f}};
    // whether a body's arm rests as a soldier's, give or take rounding
    bool IsSoldierArm(const ArmRest& body, const ArmRest& soldier);

    // the rotations in the model of what the bones of an arm hang from, in the order of ArmRest
    struct ArmParents
    {
        glm::quat spine{1, 0, 0, 0};
        glm::quat clavicle{1, 0, 0, 0};
        glm::quat shoulder{1, 0, 0, 0};
        glm::quat elbow{1, 0, 0, 0};
    };
    // how far a soldier's left wrist would be from this body's, with the gun on the right hand where it is
    glm::vec3 GetWristShift(const ArmRest& left, const ArmParents& leftParents, const ArmRest& right,
                            const ArmParents& rightParents);

    // The gun hangs from tag_weapon_right, which the animations move from where it rests. Soldiers' bodies rest it here
    // from j_wrist_ri. Other bodies keep it at the same place of the model on shorter arms, so on those the gun hangs
    // off the hand by the difference: such a body's tag rests where a soldier's does.
    inline constexpr glm::vec3 SOLDIER_GUN_TAG{-7.58f, 5.052f, -6.895f};
    inline constexpr std::array<std::int16_t, 4> SOLDIER_GUN_TAG_ROTATION{-10718, -9725, -29363, 1435};  // as models keep it
    bool IsSoldierGunTag(const glm::vec3& rest);
}  // namespace IWXMVM::GripLogic
