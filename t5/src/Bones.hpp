#pragma once
#include "Types/BoneData.hpp"
#include "Types/BoneCamera.hpp"

namespace IWXMVM::T5::Bones
{
    // Bones are posed on the main thread while frames are built there, but core's UI asks for them on the render
    // thread. On the main thread a bone is looked up right away. Other threads get what the main thread found for the
    // same entity and bone the last time it synced with the render thread, which is the frame being rendered then.
    Types::BoneData GetBoneData(std::int32_t entityId, const std::string& name);

    // A bone of the bone camera's target (GameInterface::GetTargetBone): a player's (through its corpse once it died)
    // as GetBoneData has it, an actor's as it was last drawn (on the main thread, as it was last posed)
    std::optional<Types::TargetBone> GetTargetBone(const Types::BoneCameraTarget& target, const std::string& name);
    // The names of the bones of a target's models (GameInterface::GetTargetBones), the same way
    std::vector<std::string> GetTargetBones(const Types::BoneCameraTarget& target);

    // a player's entity while it is in the demo, else its corpse's; the entity itself when there is none. Main thread.
    std::int32_t ResolvePlayer(std::int32_t entityId);
    // the script string of a bone's name, kept until the next map (main thread)
    std::uint16_t GetScriptString(const std::string& name);

    // main thread, while the render thread is idle
    void UpdateRequestedBones();
    // main thread, once the actors were drawn: the actors' bones asked for
    void UpdateRequestedActorBones();

    void Install();
}  // namespace IWXMVM::T5::Bones
