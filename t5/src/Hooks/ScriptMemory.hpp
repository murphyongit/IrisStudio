#pragma once

namespace IWXMVM::T5::Hooks::ScriptMemory
{
    // Keeps the script memory tree's lists of free blocks from looping, which hung demo loads (see ScriptMemory.cpp)
    void Install();

    // how many damaged links were cut since the game started, for the harness
    std::size_t GetRepairCount();

#ifdef IWXMVM_T5_HARNESS
    // the harness's diagnostics, told of each block handed out and given back (under the tree's lock)
    typedef void (*Observer)(int instance, int node, int bytes);
    void SetObservers(Observer allocated, Observer freeing);
#endif
}  // namespace IWXMVM::T5::Hooks::ScriptMemory
