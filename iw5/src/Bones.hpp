#pragma once
#include "StdInclude.hpp"
#include "Types/BoneData.hpp"

// A model's bones are worked out by the game on its main thread, with memory that is handed out anew every frame.
// The editor asks for them on the thread that draws it, where asking the game ended it (the bone camera did). So the
// editor's requests are kept, answered on the main thread once a frame, and the editor is given the last answers.
namespace IWXMVM::IW5::Bones
{
    // any thread
    Types::BoneData GetBoneData(std::int32_t entityId, const std::string& name);

    // main thread, in a frame of the demo's world
    void UpdateRequestedBones();

    void Install();
}  // namespace IWXMVM::IW5::Bones
