#include "StdInclude.hpp"
#include "PoseLogic.hpp"

namespace IWXMVM::PoseLogic
{
    void Compose(std::span<const LocalBone> bones, Types::AnimationPose& pose)
    {
        pose.positions.resize(bones.size());
        pose.rotations.resize(bones.size());
        pose.parents.resize(bones.size());
        for (std::size_t b = 0; b < bones.size(); ++b)
        {
            const auto& bone = bones[b];
            const auto parent = bone.parent;
            if (parent < 0 || parent >= static_cast<std::int32_t>(b))
            {
                pose.rotations[b] = bone.rotation;
                pose.positions[b] = bone.translation;
                pose.parents[b] = -1;
            }
            else
            {
                pose.rotations[b] = pose.rotations[parent] * bone.rotation;
                pose.positions[b] = pose.positions[parent] + pose.rotations[parent] * bone.translation;
                pose.parents[b] = parent;
            }
        }
    }

    glm::mat4 GetTransform(const Types::AnimationPose& pose, std::size_t bone)
    {
        return glm::translate(glm::mat4(1.0f), pose.positions[bone]) * glm::mat4_cast(pose.rotations[bone]);
    }

    float GetTransformDistance(const glm::mat4& a, const glm::mat4& b, float length)
    {
        float farthest = glm::distance(glm::vec3(a[3]), glm::vec3(b[3]));
        for (int axis = 0; axis < 3; ++axis)
        {
            glm::vec4 point(0.0f, 0.0f, 0.0f, 1.0f);
            point[axis] = length;
            farthest = std::max(farthest, glm::distance(glm::vec3(a * point), glm::vec3(b * point)));
        }
        return farthest;
    }

    SpineKind GetSpineKind(std::span<const std::string_view> animatedBones)
    {
        const auto has = [&](std::string_view name) {
            return std::ranges::find(animatedBones, name) != animatedBones.end();
        };
        if (has("torso_stabilizer"))
            return SpineKind::Moves;
        return has("j_mainroot") && has("j_spinelower") ? SpineKind::Lined : SpineKind::AtRest;
    }

    std::optional<std::size_t> FindSpareTag(std::span<const std::string_view> names,
                                            std::span<const std::int32_t> parents, std::size_t after, std::size_t before)
    {
        std::optional<std::size_t> found;
        for (auto bone = after + 1; bone < before && bone < names.size(); ++bone)
        {
            const bool used = std::ranges::find(parents, static_cast<std::int32_t>(bone)) != parents.end();
            if (!names[bone].starts_with("tag_") || used)
                continue;
            if (names[bone] == "tag_sync")
                return bone;
            found = bone;
        }
        return found;
    }

    SpineLineUp LineUpSpine(glm::quat pelvisRest, glm::quat stabilizerRest, glm::vec3 spineRest)
    {
        // turned back by pelvis's own rotation, it is turned as pelvis's parent is; and the spine, which hung from it
        // at its rest rotation, starts where it did
        SpineLineUp lineUp;
        lineUp.rotation = glm::inverse(glm::normalize(pelvisRest));
        lineUp.offset = glm::normalize(stabilizerRest) * spineRest - lineUp.rotation * spineRest;
        return lineUp;
    }

    SpineLineUp AimSpine(glm::quat pelvisRest, glm::quat stabilizerRest, glm::vec3 spineRest, glm::quat mainroot)
    {
        // turned back by j_mainroot's and pelvis's rotations, it has the model's axes
        SpineLineUp aim;
        aim.rotation = glm::normalize(glm::inverse(glm::normalize(mainroot) * glm::normalize(pelvisRest)));
        aim.offset = glm::normalize(stabilizerRest) * spineRest - aim.rotation * spineRest;
        return aim;
    }

    glm::quat AverageRotation(std::span<const glm::quat> rotations)
    {
        // (each the same way round as the first, as q and -q are the same rotation)
        glm::vec4 sum(0.0f);
        for (const auto& rotation : rotations)
        {
            const auto q = glm::normalize(rotation);
            const auto v = glm::vec4(q.x, q.y, q.z, q.w);
            sum += glm::dot(v, sum) < 0.0f ? -v : v;
        }
        if (glm::length(sum) < 1e-6f)
            return glm::quat(1, 0, 0, 0);
        sum = glm::normalize(sum);
        return glm::quat(sum.w, sum.x, sum.y, sum.z);
    }

    namespace
    {
        // a bone's transform relative to its parent, and another's relative to it: the other's relative to the parent
        LocalBone Then(const LocalBone& outer, const LocalBone& inner)
        {
            return {.rotation = outer.rotation * inner.rotation,
                    .translation = outer.translation + outer.rotation * inner.translation,
                    .parent = outer.parent};
        }

        // a bone at rest, rotated and moved as an animation has it
        LocalBone Keyed(const LocalBone& rest, const BoneKeys& keys)
        {
            return {.rotation = glm::normalize(keys.rotation.value_or(rest.rotation)),
                    .translation = rest.translation + keys.offset,
                    .parent = rest.parent};
        }
    }  // namespace

    SpineChain FitSpineChain(SpineChain chain, const LocalBone& bodySpine)
    {
        // the stabilizer at rest relative to j_mainroot, and the body's j_spinelower relative to that
        const auto stabilizer = Then(chain.pelvis, chain.stabilizer);
        const auto inverse = glm::inverse(glm::normalize(stabilizer.rotation));
        chain.spine.rotation = glm::normalize(inverse * glm::normalize(bodySpine.rotation));
        chain.spine.translation = inverse * (bodySpine.translation - stabilizer.translation);
        return chain;
    }

    LocalBone ComposeSpine(const SpineChain& chain, const BoneKeys& pelvis, const BoneKeys& stabilizer,
                           const BoneKeys& spine)
    {
        return Then(Then(Keyed(chain.pelvis, pelvis), Keyed(chain.stabilizer, stabilizer)), Keyed(chain.spine, spine));
    }

    bool StartsWithAny(std::string_view bone, std::span<const std::string_view> prefixes)
    {
        return std::ranges::any_of(prefixes, [&](std::string_view prefix) { return bone.starts_with(prefix); });
    }

    AnimationRig ClassifyAnimation(std::span<const std::string_view> animatedBones)
    {
        using Kind = Types::AnimationKind;
        using Family = Types::AnimationFamily;
        const auto has = [&](std::string_view name) {
            return std::ranges::find(animatedBones, name) != animatedBones.end();
        };
        const auto starts = [](std::string_view bone, std::span<const std::string_view> prefixes) {
            return StartsWithAny(bone, prefixes);
        };
        const auto any = [&](std::span<const std::string_view> prefixes) {
            return std::ranges::any_of(animatedBones, [&](std::string_view bone) { return starts(bone, prefixes); });
        };
        // the bones of the game's rigs, by the start of their names
        constexpr std::array<std::string_view, 5> LEGS = {"j_hip_", "j_hiptwist_", "j_knee_", "j_ankle_", "j_ball_"};
        constexpr std::array<std::string_view, 3> SPINE = {"j_spine", "j_neck", "j_head"};
        constexpr std::array<std::string_view, 4> ARMS = {"j_clavicle", "j_shoulder", "j_elbow", "j_wrist"};
        const auto& DOG = DOG_BONES;
        const auto& FACE = FACE_BONES;

        if (any(DOG))
            return {Kind::Other, Family::Dog};
        const bool body = has("j_mainroot") || any(LEGS);
        if (has("torso_stabilizer"))
            return {body ? Kind::FullBody : Kind::Torso, Family::MpPlayer};
        if (body)
            return {Kind::FullBody, has("j_spinelower") ? Family::SpHuman : Family::Human};
        if (!animatedBones.empty() &&
            std::ranges::all_of(animatedBones, [&](std::string_view bone) { return starts(bone, FACE); }))
            return {Kind::Face, Family::Human};
        // an upper body alone: with j_spinelower, MP's (keyed relative to the stabilizer)
        if (has("j_spinelower"))
            return {Kind::Torso, Family::MpPlayer};
        if (any(SPINE))
            return {Kind::Torso, Family::Human};
        // arms without a body
        if (any(ARMS))
            return {Kind::Other, Family::Viewmodel};
        return {Kind::Other, Family::Other};
    }
}  // namespace IWXMVM::PoseLogic
