#pragma once
#include "Types/AnimationPose.hpp"
#include "Components/PoseLogic.hpp"
#include "Structures.hpp"

namespace IWXMVM::T5::AnimationPose
{
    // Poses a player's body in one of the player animations, decoded from the animation's data, for previews. The
    // animations and the body come from a player or corpse model of the demo, which the main thread hands over.
    void SetSource(const Structures::DObj* obj);
    void Reset();

    // see GameInterface::GetAnimationPose
    std::optional<Types::AnimationPose> GetPose(std::uint32_t animation, float time);
    // see GameInterface::GetAnimationPoseByName; the animation and model are any loaded ones. An animation made for SP
    // bodies has an MP body's spine lined up, and one made for MP bodies an SP body's spine on the spine rig, as actors
    // are drawn (see GetSpineKind and FitSpineChain).
    std::optional<Types::AnimationPose> GetPose(std::string_view animation, std::string_view model, float time);

    // a model's bones at rest
    Types::AnimationPose GetRestPose(const Structures::XModel& model);
    // where a bone of a DObj rests from its parent (XModel::trans; a root bone at its parent), none for a bone it hasn't
    std::optional<glm::vec3> GetRestPosition(const Structures::DObj& obj, std::uint32_t bone);
    // see GameInterface::GetModelBones
    std::vector<std::string> GetModelBones(std::string_view model);
    // a model's bones' names
    std::vector<std::string> GetBoneNames(const Structures::XModel& model);

    // The rig of an MP body's spine (see PoseLogic::SpineKind): torso_stabilizer, with j_spinelower under it, at rest
    // and lined up; none for a body that hasn't one
    struct SpineRig
    {
        std::uint32_t bone = 0;  // the stabilizer's index in the model
        glm::quat rest{1, 0, 0, 0};
        glm::quat pelvis{1, 0, 0, 0};  // the rest of the bone it hangs from (pelvis), relative to j_mainroot
        glm::vec3 spineAt{0.0f};       // where j_spinelower rests relative to it
        PoseLogic::SpineLineUp lined;
    };
    std::optional<SpineRig> FindSpineRig(const Structures::XModel& model);
    // what an animation does with an MP body's torso_stabilizer
    PoseLogic::SpineKind GetSpineKind(const Structures::XAnimParts& parts);
    // what an animation moves and the rig it was made for
    PoseLogic::AnimationRig ClassifyAnimation(const Structures::XAnimParts& parts);
    // a rotation as models and animations keep it: x, y, z and w in shorts over 32767
    std::array<std::int16_t, 4> ToShorts(glm::quat rotation);
    // a model's bone by name, -1 for none
    std::int32_t FindBone(const Structures::XModel& model, std::string_view name);

    // The MP spine an SP body gets for animations made for MP bodies (see PoseLogic::SpineChain): its rests, from an MP
    // body; none for a model without pelvis and torso_stabilizer between j_mainroot and j_spinelower
    std::optional<PoseLogic::SpineChain> FindSpineChain(const Structures::XModel& model);
    // the chain the map's MP bodies have, which actors' spine rigs are made of (see Actors.cpp) and the decoder
    // composes with; set as the map's characters are listed, none before
    void SetSpineChain(std::optional<PoseLogic::SpineChain> chain);
    std::optional<PoseLogic::SpineChain> GetSpineChain();
    // That chain fitted to a body that hangs j_spinelower from j_mainroot (an SP body or full body), j_spinelower's
    // rotation rounded as a model keeps it; none for a body that has a stabilizer of its own or not that spine, or
    // while no chain is set. Such a body playing an animation of the MP player family gets the spine rig.
    std::optional<PoseLogic::SpineChain> FitSpineChain(const Structures::XModel& body);
}  // namespace IWXMVM::T5::AnimationPose
