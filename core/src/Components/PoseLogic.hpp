#pragma once
#include "Types/Actor.hpp"
#include "Types/AnimationPose.hpp"

#include <span>

namespace IWXMVM::PoseLogic
{
    // Where a model's bones are, worked out from the bones alone: no game, so the tests in tests/actors-tests build it
    // on its own. A game decodes its animation data into each bone's own transform, relative to the bone it hangs
    // from, and this puts them together.

    // a bone's transform relative to the bone it hangs from
    struct LocalBone
    {
        glm::quat rotation{1, 0, 0, 0};
        glm::vec3 translation{0.0f};
        std::int32_t parent = -1;  // a bone before it; -1 (or any bone not before it) for one on the model's root
    };

    // Where each bone is in the model, into the pose's positions, rotations and parents
    void Compose(std::span<const LocalBone> bones, Types::AnimationPose& pose);

    // a bone's transform in the model: its axes, and where it is
    glm::mat4 GetTransform(const Types::AnimationPose& pose, std::size_t bone);

    // How far apart two transforms are: the farthest apart of their origins and of the points a length along each of
    // their axes. With about the length of a hand or a magazine, it tells how far apart they would be drawn.
    float GetTransformDistance(const glm::mat4& a, const glm::mat4& b, float length);

    // The spine of an MP body under animations made for SP characters (Black Ops' AI and Zombies ones; BO2's MP
    // bodies have this rig too, as yorojii/bo2sp found). MP bodies hang j_spinelower from torso_stabilizer (on pelvis,
    // on j_mainroot), whose rest rotation turns it upright, and their animations rotate that bone; SP bodies hang
    // j_spinelower from j_mainroot itself, whose axes lie along the spine. SP animations leave the stabilizer alone, so
    // their spine rotations, made relative to j_mainroot, come out turned by its rest rotation: the torso bends forward
    // by about 90 degrees. For them the stabilizer is lined up with j_mainroot instead, and moved so the spine starts
    // where it does at rest.
    enum class SpineKind
    {
        Moves,   // animations made for MP bodies move the stabilizer
        Lined,   // those made for SP bodies move the body (j_mainroot) and the spine but not it: it is lined up
        AtRest,  // those that move neither (MP upper body ones) leave it at rest
    };
    // what an animation with tracks for these bones does with the stabilizer
    SpineKind GetSpineKind(std::span<const std::string_view> animatedBones);

    // the stabilizer's own transform that lines it up: its rotation relative to pelvis, and how far it is moved from
    // its rest position, in pelvis's axes
    struct SpineLineUp
    {
        glm::quat rotation{1, 0, 0, 0};
        glm::vec3 offset{0.0f};
    };
    // from the rest rotations of pelvis and the stabilizer (each relative to the bone it hangs from) and where
    // j_spinelower rests relative to the stabilizer
    SpineLineUp LineUpSpine(glm::quat pelvisRest, glm::quat stabilizerRest, glm::vec3 spineRest);
    // The same that aims it ahead under upper body animations made for MP bodies over legs that don't move it (made
    // for SP bodies: the AI's idles under the players' upper body clips). The players' legs hold it upright facing the
    // way the body does (the model's axes), whatever their pelvis does, and the upper body animations aim from there;
    // at rest it turns with the legs' pelvis instead, so the torso and the gun aimed as far to the side as the legs
    // turn it (37 degrees crouching with the AI's idle). From the rotation the legs give j_mainroot (in the model's
    // space), the spine starting where it rests.
    SpineLineUp AimSpine(glm::quat pelvisRest, glm::quat stabilizerRest, glm::vec3 spineRest, glm::quat mainroot);
    // the rotation in the middle of some near each other (as an idle sways its pelvis), identity for none
    glm::quat AverageRotation(std::span<const glm::quat> rotations);

    // The other way round: animations made for MP bodies (the players' pb_ and pt_ ones) rotate j_spinelower relative
    // to the stabilizer, so on an SP body, which hangs it from j_mainroot, the torso turns by about 90 degrees. Such an
    // actor gets the MP spine from a model of its own put first (the spine rig, see Actors.cpp in the T5 port): a
    // model's bones of the same names as the first's are the first's, so the SP body's j_spinelower hangs from the
    // rig's stabilizer. This is that chain, from j_mainroot to j_spinelower, each bone relative to the one before it
    // (parents aren't used).
    struct SpineChain
    {
        LocalBone pelvis;      // on j_mainroot
        LocalBone stabilizer;  // on pelvis
        LocalBone spine;       // j_spinelower on the stabilizer
    };
    // The chain with j_spinelower resting where a body has it relative to j_mainroot (MP and SP bodies rest in the same
    // pose, give or take rounding), so the SP body's mesh rests exactly as it was made
    SpineChain FitSpineChain(SpineChain chain, const LocalBone& bodySpine);
    // A tag of a model between two of its bones that nothing hangs from, tag_sync if it is one; none if there's none.
    // A player's body takes the stabilizer in its place (see Actors::GetPlayerBody in the T5 port). parents: each bone's,
    // -1 for a root bone.
    std::optional<std::size_t> FindSpareTag(std::span<const std::string_view> names,
                                            std::span<const std::int32_t> parents, std::size_t after, std::size_t before);
    // What an animation does with a bone: the rotation it has for it (none to leave it at rest), and how far it moves
    // it from its rest position
    struct BoneKeys
    {
        std::optional<glm::quat> rotation;
        glm::vec3 offset{0.0f};
    };
    // j_spinelower relative to j_mainroot through the chain, with pelvis, the stabilizer and j_spinelower as an
    // animation has them: what the SP body's own j_spinelower is drawn as
    LocalBone ComposeSpine(const SpineChain& chain, const BoneKeys& pelvis, const BoneKeys& stabilizer,
                           const BoneKeys& spine);

    // What an animation moves and the rig it was made for, from the bones it has tracks for. Animations for MP bodies
    // key torso_stabilizer, or j_spinelower without the body's root (the upper body ones), and SP ones key j_mainroot
    // and j_spinelower without it; the face ones only a face's bones, the dog ones bones of its own (j_tail...).
    struct AnimationRig
    {
        Types::AnimationKind kind = Types::AnimationKind::Other;
        Types::AnimationFamily family = Types::AnimationFamily::Other;
    };
    AnimationRig ClassifyAnimation(std::span<const std::string_view> animatedBones);
    // the bones of a dog's rig and of a face, by the start of their names
    inline constexpr std::array<std::string_view, 4> DOG_BONES = {"j_tail", "j_hip_base_", "j_shoulder_base_",
                                                                  "j_pelvis"};
    inline constexpr std::array<std::string_view, 14> FACE_BONES = {
        "j_brow",  "j_cheek", "j_eye",   "j_jaw",    "j_levator", "j_lip",     "j_mouth",
        "j_nose",  "j_chin",  "j_tongue", "j_teeth", "tag_eye",   "j_forehead", "j_bridge"};
    bool StartsWithAny(std::string_view bone, std::span<const std::string_view> prefixes);
}  // namespace IWXMVM::PoseLogic
