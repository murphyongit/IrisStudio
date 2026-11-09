#include "StdInclude.hpp"
#include "ScriptMemory.hpp"

#include "Utilities/HookManager.hpp"
#include "../Addresses.hpp"

namespace IWXMVM::T5::Hooks::ScriptMemory
{
    // The script VMs (the server's and the client's) keep their strings, vectors and other small data in a memory
    // tree: per VM 0x10000 nodes of 16 bytes, handed out in blocks of a power of two nodes. The free blocks of each
    // size are kept in a binary tree whose links are the first two words of each free block. A block is placed in it
    // by its node number: starting from position 0 and half of 0x10000, it goes left when its number is below the
    // position and right when not, the position moving by the half, which halves at each step.
    //
    // The game's client script compiler gives back some blocks it still uses while a demo loads (seen on every load,
    // with or without the mod's features): the reference counts of the strings and vectors later written into them
    // then change the free tree's links, e.g. a link of 0 becomes 0xFFFF. When such a link makes the tree loop, the
    // next block given back spins forever (the hang). Whether it happens depends on what
    // the freed blocks were reused for, so it hung about a third of the demo loads.
    //
    // Before each allocation and free, the free tree of that VM is checked under the game's lock of the tree, and a
    // link that breaks the order it is built in, or leads back into it, is cut. The blocks under it are lost (a few
    // bytes of the VM's memory) instead of hanging the game.
    inline constexpr std::size_t NODES = 0x10000;
    inline constexpr std::uintptr_t INSTANCE_STRIDE = 0x100380;
    inline constexpr std::size_t INSTANCES = 2;
    inline constexpr std::size_t SIZE_CLASSES = 17;
    inline constexpr std::size_t CRITSECT_MEMORY_TREE = 0x1C;

    std::uintptr_t nodes = 0;  // of the first VM; the free blocks' links are their first two words
    std::uintptr_t heads = 0;  // of the first VM: the root of the tree of each size class
    CRITICAL_SECTION* treeLock = nullptr;
    std::atomic<std::size_t> repairs = 0;
#ifdef IWXMVM_T5_HARNESS
    Observer onAllocated = nullptr, onFreeing = nullptr;
#endif

    struct Lock
    {
        Lock()
        {
            EnterCriticalSection(treeLock);
        }
        ~Lock()
        {
            LeaveCriticalSection(treeLock);
        }
    };

    std::uint16_t* GetLinks(std::size_t instance, std::uint16_t node)
    {
        return reinterpret_cast<std::uint16_t*>(nodes + instance * INSTANCE_STRIDE + node * 16);
    }

    // which walk last visited each node, so the walks don't clear it
    std::vector<std::uint32_t> visitedBy(NODES);
    std::uint32_t walk = 0;

    void Repair(std::size_t instance)
    {
        enum class Side
        {
            Root,
            Left,
            Right
        };
        struct Step
        {
            std::uint16_t* link;  // where the node is linked from
            Side side;
            std::int64_t bound;     // the parent's position, which the node's number is compared with
            std::int64_t position;  // signed: the root's left child would be below 0, so none fits there
            std::int64_t half;
        };
        std::array<Step, 64> pending{};

        for (std::size_t sizeClass = 0; sizeClass < SIZE_CLASSES; ++sizeClass)
        {
            ++walk;
            const auto head = reinterpret_cast<std::uint16_t*>(heads + instance * INSTANCE_STRIDE) + sizeClass;
            std::size_t count = 0;
            pending[count++] = {head, Side::Root, 0, 0, static_cast<std::int64_t>(NODES)};
            while (count > 0)
            {
                const auto [link, side, bound, position, half] = pending[--count];
                const auto node = *link;
                if (node == 0)
                    continue;
                // the root may be any node; a child must be where its number would have put it, and be reached once
                const bool placed = side == Side::Root || (side == Side::Left ? node < bound : node >= bound);
                if (!placed || visitedBy[node] == walk)
                {
                    *link = 0;
                    ++repairs;
                    continue;
                }
                visitedBy[node] = walk;

                // the links are the first two words of the free block; below the last step no node can be placed
                const auto links = GetLinks(instance, node);
                const auto childHalf = half >> 1;
                if (childHalf == 0 || count + 2 > pending.size())
                {
                    if (links[0] != 0 || links[1] != 0)
                    {
                        links[0] = links[1] = 0;
                        ++repairs;
                    }
                    continue;
                }
                pending[count++] = {&links[0], Side::Left, position, position - childHalf, childHalf};
                pending[count++] = {&links[1], Side::Right, position, position + childHalf, childHalf};
            }
        }
    }

    typedef int(__cdecl* MT_AllocIndex_t)(int instance, int bytes);
    MT_AllocIndex_t MT_AllocIndex_Trampoline;
    int __cdecl MT_AllocIndex_Hook(int instance, int bytes)
    {
        Lock lock;
        if (static_cast<std::size_t>(instance) < INSTANCES)
            Repair(static_cast<std::size_t>(instance));
#ifdef IWXMVM_T5_HARNESS
        const auto node = MT_AllocIndex_Trampoline(instance, bytes);
        if (onAllocated)
            onAllocated(instance, node, bytes);
        return node;
#else
        return MT_AllocIndex_Trampoline(instance, bytes);
#endif
    }

    typedef void(__cdecl* MT_FreeIndex_t)(int instance, int node, int bytes);
    MT_FreeIndex_t MT_FreeIndex_Trampoline;
    void __cdecl MT_FreeIndex_Hook(int instance, int node, int bytes)
    {
        Lock lock;
        if (static_cast<std::size_t>(instance) < INSTANCES)
            Repair(static_cast<std::size_t>(instance));
#ifdef IWXMVM_T5_HARNESS
        if (onFreeing)
            onFreeing(instance, node, bytes);
#endif
        MT_FreeIndex_Trampoline(instance, node, bytes);
    }

    std::size_t GetRepairCount()
    {
        return repairs.load();
    }

#ifdef IWXMVM_T5_HARNESS
    void SetObservers(Observer allocated, Observer freeing)
    {
        Lock lock;
        onAllocated = allocated;
        onFreeing = freeing;
    }
#endif

    void Install()
    {
        const auto& addresses = GetGameAddresses();
        nodes = addresses.mt_nodes();
        heads = addresses.mt_heads();
        treeLock = reinterpret_cast<CRITICAL_SECTION*>(addresses.s_criticalSections()) + CRITSECT_MEMORY_TREE;

        HookManager::CreateHook(addresses.MT_AllocIndex(), (std::uintptr_t)MT_AllocIndex_Hook,
                                (std::uintptr_t*)&MT_AllocIndex_Trampoline);
        HookManager::CreateHook(addresses.MT_FreeIndex(), (std::uintptr_t)MT_FreeIndex_Hook,
                                (std::uintptr_t*)&MT_FreeIndex_Trampoline);
    }
}  // namespace IWXMVM::T5::Hooks::ScriptMemory
