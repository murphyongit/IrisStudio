// Harness-only diagnostics (built with /p:T5Harness=true), see Diagnostics.hpp
#include "StdInclude.hpp"
#include "Diagnostics.hpp"

#include <intrin.h>
#include <mutex>
#include <set>
#include <TlHelp32.h>

#include "Addresses.hpp"
#include "Events.hpp"
#include "Structures.hpp"
#include "Hooks/Camera.hpp"
#include "Hooks/ScriptMemory.hpp"
#include "Hooks/EntityClients.hpp"
#include "Components/Rewinding.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::T5::Diagnostics
{
    std::ofstream file;
    std::mutex fileMutex;

    void Write(const std::string& text)
    {
        std::lock_guard lock(fileMutex);
        const auto cg = Structures::GetClientGlobals();
        file << std::format("t {} thread {}{} {}\n", cg ? cg->time : 0, GetCurrentThreadId(),
                            Hooks::Camera::IsMainThread() ? " (main)" : "", text)
             << std::flush;
    }

    // The return addresses into the game's code or this module's found on the stack above a point, nearest first (as
    // the game's code has no frame pointers, some are left over from calls that returned)
    using Stack = std::array<std::uint32_t, 12>;

    std::pair<std::uintptr_t, std::uintptr_t> GetModuleRange();

    Stack CaptureStack(const void* from)
    {
        const auto [moduleStart, moduleEnd] = GetModuleRange();
        const auto top = reinterpret_cast<std::uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
        Stack stack{};
        std::size_t found = 0;
        auto word = reinterpret_cast<const std::uint32_t*>(from);
        for (int i = 0; i < 1024 && reinterpret_cast<std::uintptr_t>(word) + 4 <= top && found < stack.size(); ++i, ++word)
        {
            const auto value = *word;
            if ((value >= 0x401000 && value < 0xA00000) || (value >= moduleStart && value < moduleEnd))
                stack[found++] = value;
        }
        return stack;
    }

    std::string FormatStack(const Stack& stack)
    {
        const auto moduleStart = GetModuleRange().first;
        std::string text;
        for (const auto value : stack)
        {
            if (value == 0)
                break;
            text += value < 0xA00000 ? std::format("{:X} ", value) : std::format("t5+{:X} ", value - moduleStart);
        }
        return text;
    }

    std::string ScanStack(const void* from)
    {
        return FormatStack(CaptureStack(from));
    }

    std::pair<std::uintptr_t, std::uintptr_t> GetModuleRange()
    {
        static const auto [moduleStart, moduleEnd] = [] {
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&ScanStack), &self);
            const auto base = reinterpret_cast<std::uintptr_t>(self);
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            return std::pair(base, base + nt->OptionalHeader.SizeOfImage);
        }();
        return std::pair(moduleStart, moduleEnd);
    }

    // The script memory tree: MT_AllocIndex(instance, bytes) returns the first node of a block, MT_FreeIndex(instance,
    // node, bytes) gives it back. Blocks are a power of two of 16 byte nodes, the power for up to 256 nodes from a table
    // of each instance. Each node's state is kept here, so a block freed while it is free is caught before it breaks
    // the tree (and then not freed again).
    namespace MemoryTree
    {
        constexpr std::uintptr_t MT_ALLOC_INDEX = 0x488240;
        constexpr std::uintptr_t MT_FREE_INDEX = 0x45FCF0;
        constexpr std::uintptr_t MT_INIT = 0x465AC0;  // MT_Init(instance): all of the instance's memory free again
        constexpr std::uintptr_t SIZE_CLASSES = 0x3C6AA80;
        constexpr std::uintptr_t INSTANCE_STRIDE = 0x100380;
        constexpr std::size_t NODES = 0x10038;
        constexpr std::size_t INSTANCES = 2;

        // The free blocks of each size are a binary tree of nodes: the root's index in HEADS, a node's children in
        // its first two words. A free block in it twice (a cycle) is what MT_FreeIndex later spins on.
        constexpr std::uintptr_t HEADS = 0x3C6AB80;  // + instance * INSTANCE_STRIDE + size class * 2
        constexpr std::uintptr_t NODE_LINKS = 0x3B6A880;  // + (instance * NODES + node) * 16
        constexpr int SIZE_CLASSES_COUNT = 17;
        constexpr std::uintptr_t TAIL = 0x100000;  // from an instance's start: what follows its 0x10000 nodes
        constexpr std::size_t TAIL_SIZE = 0x380;
        constexpr std::uintptr_t SYS_ENTER_CRITICAL_SECTION = 0x45A370, SYS_LEAVE_CRITICAL_SECTION = 0x4083B0;
        constexpr int CRITSECT_MEMORY_TREE = 0x1C;

        typedef void(__cdecl* Sys_CriticalSection_t)(int section);

        // the game's lock of the tree (it can be taken again on the same thread), then ours
        struct Lock
        {
            Lock()
            {
                reinterpret_cast<Sys_CriticalSection_t>(SYS_ENTER_CRITICAL_SECTION)(CRITSECT_MEMORY_TREE);
            }
            ~Lock()
            {
                reinterpret_cast<Sys_CriticalSection_t>(SYS_LEAVE_CRITICAL_SECTION)(CRITSECT_MEMORY_TREE);
            }
        };

        enum class State : std::uint8_t
        {
            Unknown,  // allocated when the harness started (or the instance started again)
            Allocated,
            Freed,
        };

        using Node = std::array<std::uint8_t, 16>;

        struct Instance
        {
            std::vector<State> states = std::vector<State>(NODES);
            std::vector<Stack> freedBy = std::vector<Stack>(NODES);  // the stack of the last free
            // for a node that starts a block allocated since the harness started: how many nodes it has, and where
            // the allocation came from; 0 for other nodes
            std::vector<std::uint32_t> allocatedCount = std::vector<std::uint32_t>(NODES);
            std::vector<Stack> allocatedBy = std::vector<Stack>(NODES);
            // the free nodes of the trees as the last operation left them, and what follows the nodes (the size
            // classes, the trees' heads)
            std::vector<std::pair<std::uint16_t, Node>> snapshot;
            std::array<std::uint8_t, TAIL_SIZE> tail{};
        };
        std::array<Instance, INSTANCES> instances;
        std::mutex mutex;
        std::set<DWORD> threads;
        std::size_t reports = 0;
        // the last operation on the tree, for what changed it since
        std::string lastOperation = "none";
        std::uint64_t operations = 0;

        const Node& ReadNode(int instance, std::size_t node)
        {
            return *reinterpret_cast<const Node*>(NODE_LINKS + (instance * NODES + node) * 16);
        }

        // Calls a function for each free block of an instance's trees: its first node and how many nodes it has
        template <typename Visit>
        void ForEachFreeBlock(int instance, Visit visit)
        {
            for (int sizeClass = 0; sizeClass < SIZE_CLASSES_COUNT; ++sizeClass)
            {
                std::vector<std::uint16_t> pending{
                    *reinterpret_cast<const std::uint16_t*>(HEADS + instance * INSTANCE_STRIDE + sizeClass * 2)};
                std::size_t guard = 0;
                while (!pending.empty() && guard++ < NODES)
                {
                    const auto node = pending.back();
                    pending.pop_back();
                    if (node == 0 || node >= NODES)
                        continue;
                    visit(node, std::size_t{1} << sizeClass);
                    const auto links = reinterpret_cast<const std::uint16_t*>(&ReadNode(instance, node));
                    pending.push_back(links[0]);
                    pending.push_back(links[1]);
                }
            }
        }

        const std::uint8_t* GetTail(int instance)
        {
            return reinterpret_cast<const std::uint8_t*>(NODE_LINKS + instance * INSTANCE_STRIDE + TAIL);
        }

        void TakeSnapshot(int instance)
        {
            std::memcpy(instances[instance].tail.data(), GetTail(instance), TAIL_SIZE);
            auto& snapshot = instances[instance].snapshot;
            snapshot.clear();
            ForEachFreeBlock(instance, [&](std::size_t node, std::size_t) {
                snapshot.emplace_back(static_cast<std::uint16_t>(node), ReadNode(instance, node));
            });
        }

        std::string Hex(const Node& node)
        {
            std::string text;
            for (const auto byte : node)
                text += std::format("{:02X}", byte);
            return text;
        }

        // Whether a free node was written since the last operation: by what the game wrote past the end of its
        // block, or through a pointer to memory it had freed
        void VerifySnapshot(const char* now, const void* frame)
        {
            for (int instance = 0; instance < static_cast<int>(INSTANCES); ++instance)
            {
                const auto& tail = instances[instance].tail;
                if (std::memcmp(tail.data(), GetTail(instance), TAIL_SIZE) != 0 && reports++ < 20)
                {
                    std::string changes;
                    for (std::size_t i = 0; i < TAIL_SIZE; ++i)
                        if (tail[i] != GetTail(instance)[i])
                            changes += std::format("+{:X}: {:02X}->{:02X} ", TAIL + i, tail[i], GetTail(instance)[i]);
                    Write(std::format("MT TREE STATE WRITTEN instance {}: {} (after {} operations); since the last "
                                      "operation, {}; found at {} from {}",
                                      instance, changes, operations, lastOperation, now, ScanStack(frame)));
                }
                for (const auto& [node, before] : instances[instance].snapshot)
                {
                    const auto& after = ReadNode(instance, node);
                    if (after == before)
                        continue;
                    if (reports++ < 20)
                    {
                        // the nodes around it, which the writer may have spilled over from
                        std::string around;
                        for (int i = -2; i <= 1; ++i)
                            if (node + i > 0 && node + i < static_cast<int>(NODES))
                                around += std::format("[{}] {} ", node + i, Hex(ReadNode(instance, node + i)));
                        Write(std::format("MT FREE NODE WRITTEN instance {} node {}: {} -> {} (after {} operations); "
                                          "since the last operation, {}; found at {} from {}; around: {}",
                                          instance, node, Hex(before), Hex(after), operations, lastOperation, now,
                                          ScanStack(frame), around));
                    }
                }
            }
        }

        void AfterOperation(const char* what, int instance, int node, int bytes, const void* frame)
        {
            for (int i = 0; i < static_cast<int>(INSTANCES); ++i)
                TakeSnapshot(i);
            ++operations;
            lastOperation = std::format("{} instance {} node {} {} bytes from {}", what, instance, node, bytes,
                                        ScanStack(frame));
        }

        // what is free when the harness starts (or the instance starts again) is known from the trees; the rest is
        // taken as allocated
        void SeedStates(int instance)
        {
            auto& states = instances[instance].states;
            std::ranges::fill(states, State::Unknown);
            std::ranges::fill(instances[instance].allocatedCount, 0u);
            ForEachFreeBlock(instance, [&](std::size_t node, std::size_t count) {
                std::fill(states.begin() + node, states.begin() + std::min(NODES, node + count), State::Freed);
            });
            TakeSnapshot(instance);
        }

        bool hooked = false;  // the operations are followed
        bool heavy = false;   // and checked against snapshots (which slows them, enough that the failures stop)
        void (*onFreed)(int instance, int node, std::size_t count) = nullptr;

        std::size_t CountNodes(int instance, int bytes)
        {
            const auto nodes = (bytes + 15) / 16;
            if (nodes >= 1 && nodes <= 256)
                return std::size_t{1} << *reinterpret_cast<const std::uint8_t*>(SIZE_CLASSES + instance * INSTANCE_STRIDE +
                                                                                nodes - 1);
            std::size_t count = 1;
            while (count < static_cast<std::size_t>(nodes))
                count <<= 1;
            return count;
        }

        void NoteThread(const char* what, const void* frame)
        {
            if (threads.insert(GetCurrentThreadId()).second)
                Write(std::format("MT first {} from this thread: {}", what, ScanStack(frame)));
        }

        typedef int(__cdecl* MT_AllocIndex_t)(int instance, int bytes);
        MT_AllocIndex_t MT_AllocIndex_Trampoline;
        int __cdecl MT_AllocIndex_Hook(int instance, int bytes)
        {
            std::optional<Lock> treeLock;
            if (heavy)
                treeLock.emplace();
            std::lock_guard lock(mutex);
            NoteThread("alloc", _AddressOfReturnAddress());
            if (heavy)
                VerifySnapshot("an alloc", _AddressOfReturnAddress());
            const auto node = MT_AllocIndex_Trampoline(instance, bytes);
            if (instance >= 0 && instance < static_cast<int>(INSTANCES) && node > 0)
            {
                auto& data = instances[instance];
                const auto count = CountNodes(instance, bytes);
                const auto end = std::min(NODES, static_cast<std::size_t>(node) + count);
                std::fill(data.states.begin() + node, data.states.begin() + end, State::Allocated);
                std::fill(data.allocatedCount.begin() + node, data.allocatedCount.begin() + end, 0u);
                data.allocatedCount[node] = static_cast<std::uint32_t>(count);
                data.allocatedBy[node] = CaptureStack(_AddressOfReturnAddress());
            }
            if (heavy)
                AfterOperation("alloc", instance, node, bytes, _AddressOfReturnAddress());
            return node;
        }

        typedef void(__cdecl* MT_FreeIndex_t)(int instance, int node, int bytes);
        MT_FreeIndex_t MT_FreeIndex_Trampoline;
        void __cdecl MT_FreeIndex_Hook(int instance, int node, int bytes)
        {
            std::optional<Lock> treeLock;
            if (heavy)
                treeLock.emplace();
            std::lock_guard lock(mutex);
            NoteThread("free", _AddressOfReturnAddress());
            if (heavy)
                VerifySnapshot("a free", _AddressOfReturnAddress());
            if (instance >= 0 && instance < static_cast<int>(INSTANCES) && node > 0 &&
                static_cast<std::size_t>(node) < NODES)
            {
                auto& [states, freedBy, allocatedCount, allocatedBy, snapshot, tail] = instances[instance];
                const auto count = CountNodes(instance, bytes);
                const auto end = std::min(NODES, static_cast<std::size_t>(node) + count);
                const auto stack = CaptureStack(_AddressOfReturnAddress());
                // a block freed with another size than it was allocated with, or from inside it
                const bool aligned = node % count == 0;
                if (allocatedCount[node] != 0 && allocatedCount[node] != count || !aligned)
                {
                    if (reports++ < 20)
                        Write(std::format("MT BAD FREE instance {} node {}: {} bytes ({} nodes{}), allocated as {} "
                                          "nodes; now: {}; allocated by: {}",
                                          instance, node, bytes, count, aligned ? "" : ", not aligned",
                                          allocatedCount[node], FormatStack(stack), FormatStack(allocatedBy[node])));
                }
                for (auto inside = node + 1; inside < static_cast<int>(end); ++inside)
                {
                    if (allocatedCount[inside] != 0)
                    {
                        if (reports++ < 20)
                            Write(std::format("MT BAD FREE instance {} node {}: {} bytes reach into block {} ({} "
                                              "nodes) allocated by: {}; now: {}",
                                              instance, node, bytes, inside, allocatedCount[inside],
                                              FormatStack(allocatedBy[inside]), FormatStack(stack)));
                        break;
                    }
                }
                std::fill(allocatedCount.begin() + node, allocatedCount.begin() + end, 0u);
                const auto freed = std::find(states.begin() + node, states.begin() + end, State::Freed) - states.begin();
                if (static_cast<std::size_t>(freed) < end)
                {
                    if (reports++ < 20)
                        Write(std::format("MT DOUBLE FREE instance {} node {} ({} bytes, node {} already free); "
                                          "now: {}; before: {}",
                                          instance, node, bytes, freed, FormatStack(stack),
                                          FormatStack(freedBy[freed])));
                    // not again, which would break the tree
                    return;
                }
                std::fill(states.begin() + node, states.begin() + end, State::Freed);
                freedBy[node] = stack;
            }
            MT_FreeIndex_Trampoline(instance, node, bytes);
            if (heavy)
                AfterOperation("free", instance, node, bytes, _AddressOfReturnAddress());
            if (onFreed && instance >= 0 && instance < static_cast<int>(INSTANCES))
                onFreed(instance, node, CountNodes(instance, bytes));
        }

        bool IsFree(int instance, int node)
        {
            return instance >= 0 && instance < static_cast<int>(INSTANCES) && node > 0 &&
                   static_cast<std::size_t>(node) < NODES && instances[instance].states[node] == State::Freed;
        }

        typedef void(__cdecl* MT_Init_t)(int instance);
        MT_Init_t MT_Init_Trampoline;
        void __cdecl MT_Init_Hook(int instance)
        {
            Lock treeLock;
            MT_Init_Trampoline(instance);
            std::lock_guard lock(mutex);
            if (instance >= 0 && instance < static_cast<int>(INSTANCES))
                SeedStates(instance);
            Write(std::format("MT init instance {}", instance));
        }

        bool broken = false;
        std::uint64_t checks = 0;

        // the first size class of an instance whose tree has a node twice, or a node out of range
        std::optional<std::pair<int, std::string>> FindBrokenTree()
        {
            std::vector<bool> visited(NODES);
            for (int instance = 0; instance < static_cast<int>(INSTANCES); ++instance)
            {
                for (int sizeClass = 0; sizeClass < SIZE_CLASSES_COUNT; ++sizeClass)
                {
                    const auto head = *reinterpret_cast<const std::uint16_t*>(HEADS + instance * INSTANCE_STRIDE +
                                                                             sizeClass * 2);
                    std::vector<std::uint16_t> pending{head};
                    std::size_t count = 0;
                    while (!pending.empty())
                    {
                        const auto node = pending.back();
                        pending.pop_back();
                        if (node == 0)
                            continue;
                        if (node >= NODES)
                            return std::pair(instance, std::format("class {} node {} out of range", sizeClass, node));
                        if (visited[node])
                            return std::pair(instance, std::format("class {} node {} twice ({} nodes in)", sizeClass,
                                                                   node, count));
                        visited[node] = true;
                        ++count;
                        const auto links = reinterpret_cast<const std::uint16_t*>(&ReadNode(instance, node));
                        pending.push_back(links[0]);
                        pending.push_back(links[1]);
                    }
                }
                std::fill(visited.begin(), visited.end(), false);
            }
            return std::nullopt;
        }

        void Check()
        {
            if (broken)
                return;
            Lock treeLock;
            std::lock_guard lock(mutex);
            if (heavy)
            {
                VerifySnapshot("a frame", _AddressOfReturnAddress());
                for (int i = 0; i < static_cast<int>(INSTANCES); ++i)
                    TakeSnapshot(i);
            }
            const auto found = FindBrokenTree();
            ++checks;
            if (found)
            {
                broken = true;
                Write(std::format("MT TREE BROKEN instance {}: {} (check {}, fine at the one before)", found->first,
                                  found->second, checks));
            }
            else if (checks % 2000 == 1)
                Write(std::format("MT tree fine (check {}, {} operations)", checks, operations));
        }

        void Install(bool withSnapshots)
        {
            hooked = true;
            heavy = withSnapshots;
            {
                Lock treeLock;
                std::lock_guard lock(mutex);
                for (int instance = 0; instance < static_cast<int>(INSTANCES); ++instance)
                    SeedStates(instance);
            }
            HookManager::CreateHook(MT_INIT, (std::uintptr_t)MT_Init_Hook, (std::uintptr_t*)&MT_Init_Trampoline);
            HookManager::CreateHook(MT_ALLOC_INDEX, (std::uintptr_t)MT_AllocIndex_Hook,
                                    (std::uintptr_t*)&MT_AllocIndex_Trampoline);
            HookManager::CreateHook(MT_FREE_INDEX, (std::uintptr_t)MT_FreeIndex_Hook,
                                    (std::uintptr_t*)&MT_FreeIndex_Trampoline);
        }
    }  // namespace MemoryTree

    // Script strings live in the memory tree: a string's id is its node, which starts with its reference count (and
    // user bits and length), so a reference taken or dropped after the string was freed writes into the tree's
    // links: what the dumps of the hangs show. Those calls are logged with their stack and skipped.
    namespace ScriptStrings
    {
        constexpr std::uintptr_t STRING_BASES = 0x3B6A800;  // + instance * 4: the instance's node 0
        constexpr std::uintptr_t SL_REMOVE_REF = 0x4F65A0;  // SL_RemoveRefToString(instance, string)
        constexpr std::uintptr_t SL_REMOVE_REF_OF_SIZE = 0x5C7850;  // (instance, string, length)
        constexpr std::uintptr_t SL_ADD_REF = 0x5AC4A0;  // SL_AddRefToString(string, instance)
        std::atomic<int> reports = 0;

        std::uint32_t* GetString(int instance, unsigned int string)
        {
            if (instance < 0 || instance > 1 || string == 0 || string >= 0x10000)
                return nullptr;
            const auto base = *reinterpret_cast<std::uint8_t**>(STRING_BASES + instance * 4);
            return base ? reinterpret_cast<std::uint32_t*>(base + string * 16) : nullptr;
        }

        // whether the string has no references left, so it was freed; logged the first times
        bool IsFreed(const char* what, int instance, unsigned int string, const void* frame)
        {
            const auto data = GetString(instance, string);
            if (!data || (*data & 0xFFFF) != 0)
                return false;
            if (reports++ < 20)
            {
                const auto text = reinterpret_cast<const char*>(data) + 4;
                std::string chars;
                for (int i = 0; i < 12; ++i)
                    chars += text[i] >= 32 && text[i] < 127 ? text[i] : '.';
                Write(std::format("SL {} OF A FREED STRING instance {} string {} (header {:08X}, \"{}\"): {}", what,
                                  instance, string, *data, chars, ScanStack(frame)));
            }
            return true;
        }

        typedef void(__cdecl* SL_RemoveRefToString_t)(int instance, unsigned int string);
        SL_RemoveRefToString_t SL_RemoveRefToString_Trampoline;
        void __cdecl SL_RemoveRefToString_Hook(int instance, unsigned int string)
        {
            if (!IsFreed("RELEASE", instance, string, _AddressOfReturnAddress()))
                SL_RemoveRefToString_Trampoline(instance, string);
        }

        typedef void(__cdecl* SL_RemoveRefToStringOfSize_t)(int instance, unsigned int string, unsigned int length);
        SL_RemoveRefToStringOfSize_t SL_RemoveRefToStringOfSize_Trampoline;
        void __cdecl SL_RemoveRefToStringOfSize_Hook(int instance, unsigned int string, unsigned int length)
        {
            if (!IsFreed("RELEASE (OF SIZE)", instance, string, _AddressOfReturnAddress()))
                SL_RemoveRefToStringOfSize_Trampoline(instance, string, length);
        }

        typedef void(__cdecl* SL_AddRefToString_t)(unsigned int string, int instance);
        SL_AddRefToString_t SL_AddRefToString_Trampoline;
        void __cdecl SL_AddRefToString_Hook(unsigned int string, int instance)
        {
            if (!IsFreed("ADD REF", instance, string, _AddressOfReturnAddress()))
                SL_AddRefToString_Trampoline(string, instance);
        }

        void Install()
        {
            HookManager::CreateHook(SL_REMOVE_REF, (std::uintptr_t)SL_RemoveRefToString_Hook,
                                    (std::uintptr_t*)&SL_RemoveRefToString_Trampoline);
            HookManager::CreateHook(SL_REMOVE_REF_OF_SIZE, (std::uintptr_t)SL_RemoveRefToStringOfSize_Hook,
                                    (std::uintptr_t*)&SL_RemoveRefToStringOfSize_Trampoline);
            HookManager::CreateHook(SL_ADD_REF, (std::uintptr_t)SL_AddRefToString_Hook,
                                    (std::uintptr_t*)&SL_AddRefToString_Trampoline);
        }
    }  // namespace ScriptStrings

    // IWXMVM_DIAG_THREADS: which threads change the script strings, variables and memory of each instance (0 the
    // game's scripts, 1 the client scripts), counted per thread, for the harness's "scriptthreads"
    namespace Threads
    {
        enum Kind
        {
            ADD_REF,
            REMOVE_REF,
            ALLOC_VARIABLE,
            MT_FREE,
            KINDS
        };
        constexpr const char* KIND_NAMES[KINDS] = {"addref", "removeref", "allocvariable", "mtfree"};
        struct Slot
        {
            std::atomic<DWORD> thread = 0;
            std::array<std::array<std::atomic<std::uint32_t>, 2>, KINDS> counts{};
        };
        std::array<Slot, 48> slots;
        std::atomic<DWORD> mainThread = 0;
        std::atomic<DWORD> renderThread = 0;

        void __cdecl Count(int kind, int instance)
        {
            if (instance < 0 || instance > 1)
                return;
            const auto id = GetCurrentThreadId();
            for (auto& slot : slots)
            {
                auto thread = slot.thread.load();
                if (thread == 0)
                {
                    DWORD empty = 0;
                    if (slot.thread.compare_exchange_strong(empty, id))
                        thread = id;
                    else
                        thread = empty;
                }
                if (thread == id)
                {
                    ++slot.counts[kind][instance];
                    return;
                }
            }
        }

        typedef void(__cdecl* SL_RemoveRefToString_t)(int instance, unsigned int string);
        SL_RemoveRefToString_t SL_RemoveRefToString_Trampoline;
        void __cdecl SL_RemoveRefToString_Hook(int instance, unsigned int string)
        {
            Count(REMOVE_REF, instance);
            SL_RemoveRefToString_Trampoline(instance, string);
        }

        typedef void(__cdecl* SL_RemoveRefToStringOfSize_t)(int instance, unsigned int string, unsigned int length);
        SL_RemoveRefToStringOfSize_t SL_RemoveRefToStringOfSize_Trampoline;
        void __cdecl SL_RemoveRefToStringOfSize_Hook(int instance, unsigned int string, unsigned int length)
        {
            Count(REMOVE_REF, instance);
            SL_RemoveRefToStringOfSize_Trampoline(instance, string, length);
        }

        typedef void(__cdecl* SL_AddRefToString_t)(unsigned int string, int instance);
        SL_AddRefToString_t SL_AddRefToString_Trampoline;
        void __cdecl SL_AddRefToString_Hook(unsigned int string, int instance)
        {
            Count(ADD_REF, instance);
            SL_AddRefToString_Trampoline(string, instance);
        }

        // AllocVariable 0x8E3E70(instance, ...) and MT_Free 0x4637A0(pointer, size, instance): only counted, every
        // register kept
        std::uintptr_t AllocVariable_Trampoline, MT_Free_Trampoline;
        void __cdecl CountAllocVariable(int instance)
        {
            Count(ALLOC_VARIABLE, instance);
        }
        void __cdecl CountMT_Free(int instance)
        {
            Count(MT_FREE, instance);
        }
        __declspec(naked) void AllocVariable_Hook()
        {
            __asm
            {
                pushad
                pushfd
                mov eax, [esp + 0x28]
                push eax
                call CountAllocVariable
                add esp, 4
                popfd
                popad
                jmp dword ptr [AllocVariable_Trampoline]
            }
        }
        __declspec(naked) void MT_Free_Hook()
        {
            __asm
            {
                pushad
                pushfd
                mov eax, [esp + 0x30]
                push eax
                call CountMT_Free
                add esp, 4
                popfd
                popad
                jmp dword ptr [MT_Free_Trampoline]
            }
        }

        void Install()
        {
            HookManager::CreateHook(ScriptStrings::SL_REMOVE_REF, (std::uintptr_t)SL_RemoveRefToString_Hook,
                                    (std::uintptr_t*)&SL_RemoveRefToString_Trampoline);
            HookManager::CreateHook(ScriptStrings::SL_REMOVE_REF_OF_SIZE,
                                    (std::uintptr_t)SL_RemoveRefToStringOfSize_Hook,
                                    (std::uintptr_t*)&SL_RemoveRefToStringOfSize_Trampoline);
            HookManager::CreateHook(ScriptStrings::SL_ADD_REF, (std::uintptr_t)SL_AddRefToString_Hook,
                                    (std::uintptr_t*)&SL_AddRefToString_Trampoline);
            HookManager::CreateHook(0x8E3E70, (std::uintptr_t)AllocVariable_Hook, &AllocVariable_Trampoline);
            HookManager::CreateHook(0x4637A0, (std::uintptr_t)MT_Free_Hook, &MT_Free_Trampoline);
        }

        std::string Describe()
        {
            std::string text;
            for (const auto& slot : slots)
            {
                const auto thread = slot.thread.load();
                if (thread == 0)
                    break;
                const auto role = thread == mainThread.load()     ? " (main)"
                                  : thread == renderThread.load() ? " (render)"
                                                                  : "";
                text += std::format(" | thread {}{}:", thread, role);
                for (int kind = 0; kind < KINDS; ++kind)
                    text += std::format(" {} {}/{}", KIND_NAMES[kind], slot.counts[kind][0].load(),
                                        slot.counts[kind][1].load());
            }
            return text;
        }
    }  // namespace Threads

    // Hardware watchpoints on the first words of the blocks of the client scripts' instance freed last: what writes
    // into one while it is free is what breaks the tree (a reference count changed after the string was freed). The
    // debug registers are set on every thread by a thread of its own, after each such free and once a second.
    namespace Watch
    {
        constexpr int INSTANCE = 1;
        constexpr std::size_t SLOTS = 4;
        std::array<std::atomic<int>, SLOTS> nodes{};
        std::atomic<unsigned> next = 0;
        HANDLE wake = nullptr;
        DWORD watcherThread = 0;
        std::atomic<int> reports = 0;

        std::uintptr_t GetAddress(int node)
        {
            return MemoryTree::NODE_LINKS + (INSTANCE * MemoryTree::NODES + node) * 16;
        }

        void OnFreed(int instance, int node, std::size_t count)
        {
            if (instance != INSTANCE || count > 4)
                return;
            nodes[next++ % SLOTS] = node;
            SetEvent(wake);
        }

        void ArmAll()
        {
            std::uint32_t dr7 = 0;
            std::array<std::uintptr_t, SLOTS> addresses{};
            for (std::size_t i = 0; i < SLOTS; ++i)
            {
                if (const auto node = nodes[i].load(); node > 0)
                {
                    addresses[i] = GetAddress(node);
                    // enabled locally, on writes, 4 bytes
                    dr7 |= (1u << (i * 2)) | (1u << (16 + i * 4)) | (3u << (18 + i * 4));
                }
            }

            std::vector<HANDLE> threads;
            const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 entry{sizeof(entry)};
            for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == watcherThread)
                    continue;
                if (const auto thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                                   FALSE, entry.th32ThreadID))
                    threads.push_back(thread);
            }
            CloseHandle(snapshot);

            for (const auto thread : threads)
            {
                if (SuspendThread(thread) != static_cast<DWORD>(-1))
                {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(thread, &context))
                    {
                        context.Dr0 = addresses[0];
                        context.Dr1 = addresses[1];
                        context.Dr2 = addresses[2];
                        context.Dr3 = addresses[3];
                        context.Dr7 = dr7;
                        SetThreadContext(thread, &context);
                    }
                    ResumeThread(thread);
                }
                CloseHandle(thread);
            }
        }

        // what the allocator's own functions write is fine
        bool IsAllocatorCode(std::uintptr_t address)
        {
            return (address >= 0x8E2E20 && address < 0x8E3400) || (address >= 0x45FCF0 && address < 0x45FD60) ||
                   (address >= 0x488240 && address < 0x488340) || (address >= 0x465AC0 && address < 0x465C00);
        }

        LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
                return EXCEPTION_CONTINUE_SEARCH;
            auto& context = *info->ContextRecord;
            const auto hits = context.Dr6 & 0xF;
            if (hits == 0)
                return EXCEPTION_CONTINUE_SEARCH;
            context.Dr6 = 0;
            for (std::size_t i = 0; i < SLOTS; ++i)
            {
                const auto node = nodes[i].load();
                if (!(hits & (1u << i)) || node <= 0 || IsAllocatorCode(context.Eip) ||
                    !MemoryTree::IsFree(INSTANCE, node))
                    continue;
                if (reports++ < 20)
                {
                    const auto bytes = reinterpret_cast<const std::uint8_t*>(GetAddress(node));
                    std::string hex;
                    for (int b = 0; b < 16; ++b)
                        hex += std::format("{:02X}", bytes[b]);
                    Write(std::format("WATCH WRITE TO FREE NODE {} of instance {} by {:X} (eax {:X} ecx {:X} edx {:X} "
                                      "ebx {:X} esi {:X} edi {:X}): now {}; stack {}; freed by {}",
                                      node, INSTANCE, context.Eip, context.Eax, context.Ecx, context.Edx, context.Ebx,
                                      context.Esi, context.Edi, hex,
                                      ScanStack(reinterpret_cast<const void*>(context.Esp)),
                                      FormatStack(MemoryTree::instances[INSTANCE].freedBy[node])));
                }
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        void Install()
        {
            wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            MemoryTree::onFreed = OnFreed;
            AddVectoredExceptionHandler(1, OnException);
            std::thread([] {
                watcherThread = GetCurrentThreadId();
                while (true)
                {
                    WaitForSingleObject(wake, 1000);
                    ArmAll();
                }
            }).detach();
        }
    }  // namespace Watch

    // The script variables of each instance: a list of 0x1C byte entries ([0x3DCB180 + instance * 0x80]), objects
    // from entry 1 (index i at entry i + 1) and child variables from entry 0x8000 (index i at entry 0x8000 + i). The
    // free ones of each area are a list whose head is the area's first entry's +8: an index's entry gives its id (+0),
    // the id's entry the next free index (+8, where a used object keeps its reference count), and the next index's
    // entry points back (+4). "Exceeded maximum number of client script variables" with most of them free is that list
    // cut short; checked once a frame, on the main thread (the only one using the client scripts' instance).
    namespace Variables
    {
        constexpr std::uintptr_t GLOBALS = 0x3DCB180;
        constexpr std::uintptr_t INSTANCE_STRIDE = 0x80;
        constexpr std::size_t ENTRY = 0x1C;
        struct Area
        {
            const char* name;
            std::uint32_t first;  // the entry of index 0, whose +8 is the head of the free list
            std::uint32_t limit;  // indices are below it (the children's go on to the end of the list)
        };
        constexpr std::array<Area, 2> AREAS{Area{"objects", 1, 0x7FFF}, Area{"children", 0x8000, 0xFFFFFFFF}};

        struct State
        {
            bool broken = false;
            std::array<std::uint32_t, 2> free{};  // at the last check
        };
        std::array<State, 2> states;
        std::size_t checks = 0;

        const std::uint32_t* GetEntry(std::uintptr_t list, std::uint32_t entry)
        {
            return reinterpret_cast<const std::uint32_t*>(list + entry * ENTRY);
        }

        // what is wrong with the free list of an area, if anything, and how many entries it has
        std::pair<std::optional<std::string>, std::uint32_t> CheckArea(std::uintptr_t list, std::size_t entries,
                                                                      const Area& area)
        {
            const auto limit = std::min<std::size_t>(area.limit, entries > area.first ? entries - area.first : 0);
            auto index = GetEntry(list, area.first)[2];
            std::uint32_t previous = 0, count = 0;
            while (index != 0)
            {
                if (index >= limit)
                    return {std::format("index {} after {} is out of range ({} free so far)", index, previous, count),
                            count};
                const auto entry = GetEntry(list, area.first + index);
                if (entry[1] != previous)
                    return {std::format("index {} (after {}) points back to {} ({} free so far; entry {:08X} {:08X} "
                                        "{:08X} {:08X} {:08X})",
                                        index, previous, entry[1], count, entry[0], entry[1], entry[2], entry[3],
                                        entry[4]),
                            count};
                if (++count > limit)
                    return {std::format("loops ({} free)", count), count};
                const auto id = entry[0];
                if (id >= limit)
                    return {std::format("index {} has id {} out of range ({} free so far)", index, id, count), count};
                previous = index;
                index = GetEntry(list, area.first + id)[2];
            }
            return {std::nullopt, count};
        }

        void Check()
        {
            ++checks;
            for (std::size_t instance = 0; instance < states.size(); ++instance)
            {
                auto& state = states[instance];
                const auto list = *reinterpret_cast<const std::uintptr_t*>(GLOBALS + instance * INSTANCE_STRIDE);
                if (state.broken || list == 0)
                    continue;
                MEMORY_BASIC_INFORMATION region{};
                if (!VirtualQuery(reinterpret_cast<const void*>(list), &region, sizeof(region)) ||
                    region.State != MEM_COMMIT)
                    continue;
                const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
                const auto entries = (end - list) / ENTRY;
                for (std::size_t a = 0; a < AREAS.size(); ++a)
                {
                    const auto [problem, count] = CheckArea(list, entries, AREAS[a]);
                    if (problem)
                    {
                        state.broken = true;
                        Write(std::format("VARIABLES BROKEN instance {} {}: {} (check {}, {} free at the one before)",
                                          instance, AREAS[a].name, *problem, checks, state.free[a]));
                        break;
                    }
                    state.free[a] = count;
                }
                if (!state.broken && checks % 1000 == 1)
                    Write(std::format("variables fine instance {} (check {}): {} objects and {} children free",
                                      instance, checks, state.free[0], state.free[1]));
            }
        }

        // a demo starts the client scripts again (and with them a new list)
        void Reset()
        {
            for (auto& state : states)
                state.broken = false;
        }
    }  // namespace Variables

    namespace LoadFrames
    {
        extern std::atomic<int> frame;
    }

    // IWXMVM_DIAG_STALE: references taken or dropped of the client scripts' strings whose memory is free (in the tree
    // of free blocks), and blocks freed while free, with the stacks of the last allocations and frees of the block, to
    // find what frees memory the client scripts still use. Only the main thread uses the client scripts' instance, and
    // nothing is formatted inside the game's functions: what is found is kept and logged by the next frame.
    namespace Stale
    {
        constexpr int INSTANCE = 1;
        constexpr int SIZE_CLASSES = 17;
        constexpr std::size_t NODES = 0x10000;

        struct NodeEvent
        {
            std::int32_t frame = 0;
            std::int32_t bytes = 0;
            bool freed = false;
            Stack stack{};
        };
        // of each node of the instance, its last allocations and frees as the start of a block, newest first
        std::vector<std::array<NodeEvent, 3>> history;

        enum Kind
        {
            ADD_REF,
            REMOVE_REF,
            REMOVE_REF_OF_SIZE,
            FOUND,  // SL_GetStringOfSize gave a string that is there already
            FREED_TWICE,
            KINDS
        };
        constexpr const char* KIND_NAMES[KINDS] = {"ADD REF", "RELEASE", "RELEASE (OF SIZE)", "FOUND",
                                                   "FREE OF A FREE BLOCK"};
        struct Report
        {
            Kind kind = ADD_REF;
            std::int32_t node = 0;
            std::int32_t frame = 0;
            std::array<char, 24> text{};
            Stack stack{};
            std::array<NodeEvent, 3> history{};
        };
        std::array<Report, 40> reports;
        std::atomic<int> reportCount = 0;
        int logged = 0;
        // the instance's calls of each kind, and frees and allocations, checked
        std::array<std::atomic<std::uint32_t>, KINDS + 1> calls{};

        // whether a node lies in a free block: blocks of size class c are 2^c nodes, starting at a multiple of that,
        // found in the class's tree the way MT_FreeIndex places them (see Hooks/ScriptMemory.cpp)
        bool IsNodeFree(int node)
        {
            if (node <= 0 || node >= static_cast<int>(NODES))
                return false;
            for (int sizeClass = 0; sizeClass < SIZE_CLASSES; ++sizeClass)
            {
                const auto start = static_cast<std::uint16_t>(node & ~((1 << sizeClass) - 1));
                auto link = reinterpret_cast<const std::uint16_t*>(MemoryTree::HEADS +
                                                                    INSTANCE * MemoryTree::INSTANCE_STRIDE) +
                            sizeClass;
                std::int64_t position = 0, half = NODES;
                for (int depth = 0; depth < 20 && *link != 0; ++depth)
                {
                    if (*link == start)
                        return start != 0;
                    half >>= 1;
                    const bool left = start < position;
                    const auto links = reinterpret_cast<const std::uint16_t*>(
                        MemoryTree::NODE_LINKS + (INSTANCE * MemoryTree::NODES + *link) * 16);
                    link = &links[left ? 0 : 1];
                    position += left ? -half : half;
                }
            }
            return false;
        }

        void AddReport(Kind kind, int node, const void* frame)
        {
            const auto index = reportCount.load();
            if (index >= static_cast<int>(reports.size()))
                return;
            auto& report = reports[index];
            report.kind = kind;
            report.node = node;
            report.frame = LoadFrames::frame.load();
            const auto data = reinterpret_cast<const char*>(MemoryTree::NODE_LINKS +
                                                            (INSTANCE * MemoryTree::NODES + node) * 16);
            std::memcpy(report.text.data(), data, report.text.size());
            report.stack = CaptureStack(frame);
            report.history = history[node];
            reportCount = index + 1;
        }

        void Record(int node, int bytes, bool freed, const void* frame)
        {
            auto& events = history[node];
            events[2] = events[1];
            events[1] = events[0];
            events[0] = {LoadFrames::frame.load(), bytes, freed, CaptureStack(frame)};
        }

        void __cdecl OnAllocated(int instance, int node, int bytes)
        {
            if (instance == INSTANCE && node > 0 && node < static_cast<int>(NODES))
            {
                ++calls[KINDS];
                Record(node, bytes, false, _AddressOfReturnAddress());
            }
        }

        void __cdecl OnFreeing(int instance, int node, int bytes)
        {
            if (instance != INSTANCE || node <= 0 || node >= static_cast<int>(NODES))
                return;
            ++calls[FREED_TWICE];
            if (IsNodeFree(node))
                AddReport(FREED_TWICE, node, _AddressOfReturnAddress());
            Record(node, bytes, true, _AddressOfReturnAddress());
        }

        typedef void(__cdecl* SL_RemoveRefToString_t)(int instance, unsigned int string);
        SL_RemoveRefToString_t SL_RemoveRefToString_Trampoline;
        void __cdecl SL_RemoveRefToString_Hook(int instance, unsigned int string)
        {
            if (instance == INSTANCE && (++calls[REMOVE_REF], IsNodeFree(static_cast<int>(string))))
                AddReport(REMOVE_REF, static_cast<int>(string), _AddressOfReturnAddress());
            SL_RemoveRefToString_Trampoline(instance, string);
        }

        typedef void(__cdecl* SL_RemoveRefToStringOfSize_t)(int instance, unsigned int string, unsigned int length);
        SL_RemoveRefToStringOfSize_t SL_RemoveRefToStringOfSize_Trampoline;
        void __cdecl SL_RemoveRefToStringOfSize_Hook(int instance, unsigned int string, unsigned int length)
        {
            if (instance == INSTANCE && (++calls[REMOVE_REF_OF_SIZE], IsNodeFree(static_cast<int>(string))))
                AddReport(REMOVE_REF_OF_SIZE, static_cast<int>(string), _AddressOfReturnAddress());
            SL_RemoveRefToStringOfSize_Trampoline(instance, string, length);
        }

        typedef void(__cdecl* SL_AddRefToString_t)(unsigned int string, int instance);
        SL_AddRefToString_t SL_AddRefToString_Trampoline;
        void __cdecl SL_AddRefToString_Hook(unsigned int string, int instance)
        {
            if (instance == INSTANCE && (++calls[ADD_REF], IsNodeFree(static_cast<int>(string))))
                AddReport(ADD_REF, static_cast<int>(string), _AddressOfReturnAddress());
            SL_AddRefToString_Trampoline(string, instance);
        }

        // SL_GetStringOfSize(instance, string, user, length): the string's id, found in the hash table or made
        constexpr std::uintptr_t SL_GET_STRING_OF_SIZE = 0x5E4010;
        typedef unsigned int(__cdecl* SL_GetStringOfSize_t)(int instance, const char* string, int user,
                                                            unsigned int length);
        SL_GetStringOfSize_t SL_GetStringOfSize_Trampoline;
        unsigned int __cdecl SL_GetStringOfSize_Hook(int instance, const char* string, int user, unsigned int length)
        {
            const auto id = SL_GetStringOfSize_Trampoline(instance, string, user, length);
            if (instance == INSTANCE && (++calls[FOUND], IsNodeFree(static_cast<int>(id))))
                AddReport(FOUND, static_cast<int>(id), _AddressOfReturnAddress());
            return id;
        }

        std::string FormatHistory(const std::array<NodeEvent, 3>& events)
        {
            std::string text;
            for (const auto& event : events)
                if (event.stack[0] != 0)
                    text += std::format(" | {} {} bytes at load frame {}: {}", event.freed ? "freed" : "allocated",
                                        event.bytes, event.frame, FormatStack(event.stack));
            return text;
        }

        // walks every free tree of the instance and checks that IsNodeFree finds each block's first and last node
        void SelfTest()
        {
            std::size_t blocks = 0, freeNodes = 0, misses = 0, misaligned = 0;
            for (int sizeClass = 0; sizeClass < SIZE_CLASSES; ++sizeClass)
            {
                std::vector<std::uint16_t> pending{*(reinterpret_cast<const std::uint16_t*>(
                                                       MemoryTree::HEADS + INSTANCE * MemoryTree::INSTANCE_STRIDE) +
                                                   sizeClass)};
                while (!pending.empty() && blocks < 100000)
                {
                    const auto node = pending.back();
                    pending.pop_back();
                    if (node == 0)
                        continue;
                    ++blocks;
                    const int size = 1 << sizeClass;
                    freeNodes += size;
                    misaligned += (node & (size - 1)) != 0;
                    misses += !IsNodeFree(node) || !IsNodeFree(node + size - 1);
                    const auto links = reinterpret_cast<const std::uint16_t*>(
                        MemoryTree::NODE_LINKS + (INSTANCE * MemoryTree::NODES + node) * 16);
                    pending.push_back(links[0]);
                    pending.push_back(links[1]);
                }
            }
            Write(std::format("STALE self test: {} free blocks, {} free nodes, {} misaligned, {} not found; calls "
                              "addref {} release {} release-of-size {} get {} free {} alloc {}",
                              blocks, freeNodes, misaligned, misses, calls[ADD_REF].load(), calls[REMOVE_REF].load(),
                              calls[REMOVE_REF_OF_SIZE].load(), calls[FOUND].load(), calls[FREED_TWICE].load(),
                              calls[KINDS].load()));
        }

        void OnFrame()
        {
            if (history.empty())
                return;
            if (const auto frame = LoadFrames::frame.load(); frame == 1 || frame == 280)
                SelfTest();
            const auto count = reportCount.load();
            for (; logged < count; ++logged)
            {
                const auto& report = reports[logged];
                std::string chars;
                for (const auto c : report.text)
                    chars += c >= 32 && c < 127 ? c : '.';
                Write(std::format("STALE {} node {} at load frame {} (block now \"{}\"): {}; history{}",
                                  KIND_NAMES[report.kind], report.node, report.frame, chars,
                                  FormatStack(report.stack), FormatHistory(report.history)));
            }
        }

        void Install()
        {
            history.resize(NODES);
            HookManager::CreateHook(ScriptStrings::SL_REMOVE_REF, (std::uintptr_t)SL_RemoveRefToString_Hook,
                                    (std::uintptr_t*)&SL_RemoveRefToString_Trampoline);
            HookManager::CreateHook(ScriptStrings::SL_REMOVE_REF_OF_SIZE,
                                    (std::uintptr_t)SL_RemoveRefToStringOfSize_Hook,
                                    (std::uintptr_t*)&SL_RemoveRefToStringOfSize_Trampoline);
            HookManager::CreateHook(ScriptStrings::SL_ADD_REF, (std::uintptr_t)SL_AddRefToString_Hook,
                                    (std::uintptr_t*)&SL_AddRefToString_Trampoline);
            HookManager::CreateHook(SL_GET_STRING_OF_SIZE, (std::uintptr_t)SL_GetStringOfSize_Hook,
                                    (std::uintptr_t*)&SL_GetStringOfSize_Trampoline);
            Hooks::ScriptMemory::SetObservers(OnAllocated, OnFreeing);
        }
    }  // namespace Stale

    // The first frames of a load, from PreDemoLoad on: when the script strings and memory of an instance start again
    // (SL_Init), how long each frame took, the client's times and how many of the client scripts' variables are free
    // (LOADFRAME lines), to tell the loads that break the client scripts from those that don't
    namespace LoadFrames
    {
        constexpr int FRAMES = 300;
        constexpr std::uintptr_t SL_INIT = 0x5049E0;  // SL_Init(instance): MT_Init, then the hash table emptied
        std::atomic<int> frame = -1;                  // since PreDemoLoad, -1 before
        LARGE_INTEGER last{};
        // SL_Init's calls, logged by the next frame (nothing is written from inside the game's functions)
        std::array<std::atomic<int>, 2> initFrames{-2, -2};

        typedef void(__cdecl* SL_Init_t)(int instance);
        SL_Init_t SL_Init_Trampoline;
        void __cdecl SL_Init_Hook(int instance)
        {
            if (instance >= 0 && instance < 2)
                initFrames[instance] = frame.load();
            SL_Init_Trampoline(instance);
        }

        void OnPreDemoLoad()
        {
            QueryPerformanceCounter(&last);
            frame = 0;
        }

        // OnEarlyServerCommands' latest, logged by the next frame
        std::atomic<int> earlyFrom = 0, earlyTo = 0, earlyRuns = 0;

        void OnFrame()
        {
            if (const auto runs = earlyRuns.exchange(0))
                Write(std::format("EARLY SERVER COMMANDS {} times, the latest {} to {} (load frame {})", runs,
                                  earlyFrom.load(), earlyTo.load(), frame.load()));
            for (std::size_t instance = 0; instance < initFrames.size(); ++instance)
                if (const auto at = initFrames[instance].exchange(-2); at != -2)
                    Write(std::format("SL_Init instance {} (load frame {})", instance, at));
            const auto index = frame.load();
            if (index < 0 || index >= FRAMES)
                return;
            LARGE_INTEGER now, frequency;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const auto milliseconds = (now.QuadPart - last.QuadPart) * 1000 / frequency.QuadPart;
            last = now;
            const auto cg = Structures::GetClientGlobals();
            Write(std::format("LOADFRAME {} {} ms cg.time {} serverTime {} realtime {} free {}/{} repairs {}", index,
                              milliseconds, cg ? cg->time : 0, Structures::GetClientActive()->serverTime,
                              Structures::GetClientStatic()->realtime, Variables::states[1].free[0],
                              Variables::states[1].free[1], Hooks::ScriptMemory::GetRepairCount()));
            frame = index + 1;
        }

        void Install()
        {
            HookManager::CreateHook(SL_INIT, (std::uintptr_t)SL_Init_Hook, (std::uintptr_t*)&SL_Init_Trampoline);
        }
    }  // namespace LoadFrames

    // The client scripts' entity fields (0x57FD40(entity, localClientNum, field)) raise "Lost connection to host.
    // Please try again." (EXE_ENTITY_BEFORE_SNAPSHOT) when the local client's cg.nextSnap is null: the field asked
    // for, the entity, the local client and its times, and where from, are kept and logged by the next frame
    namespace EntityFields
    {
        constexpr std::uintptr_t CG_GET_ENTITY_FIELD = 0x57FD40;
        constexpr std::uintptr_t CG_ARRAY = 0xD4F178;  // cg_t of local client 0, then of the others by 0x71980
        constexpr std::size_t CG_SIZE = 0x71980;

        struct Seen
        {
            std::int32_t entity = 0, localClientNum = 0, field = 0, time = 0, frame = 0;
            const void* snap = nullptr;
            Stack stack{};
        };
        std::array<Seen, 8> seen;
        std::atomic<int> count = 0;
        int logged = 0;

        typedef void(__cdecl* CG_GetEntityField_t)(std::int32_t entity, std::int32_t localClientNum, std::int32_t field);
        CG_GetEntityField_t CG_GetEntityField_Trampoline;
        void __cdecl CG_GetEntityField_Hook(std::int32_t entity, std::int32_t localClientNum, std::int32_t field)
        {
            const auto cgs = *reinterpret_cast<const std::uint8_t* const*>(CG_ARRAY);
            // (a local client the game doesn't have is logged as it is, without reading its cg)
            const bool known = cgs && localClientNum >= 0 && localClientNum < 4;
            const auto cg = known ? cgs + localClientNum * CG_SIZE : nullptr;
            if (!known || *reinterpret_cast<const void* const*>(cg + 0x2C) == nullptr)
            {
                if (const auto index = count.load(); index < static_cast<int>(seen.size()))
                {
                    seen[index] = {entity,
                                   localClientNum,
                                   field,
                                   cg ? *reinterpret_cast<const std::int32_t*>(cg + 0x40688) : -1,
                                   LoadFrames::frame.load(),
                                   cg ? *reinterpret_cast<const void* const*>(cg + 0x28) : nullptr,
                                   CaptureStack(_AddressOfReturnAddress())};
                    count = index + 1;
                }
            }
            CG_GetEntityField_Trampoline(entity, localClientNum, field);
        }

        void OnFrame()
        {
            for (const auto now = count.load(); logged < now; ++logged)
            {
                const auto& s = seen[logged];
                Write(std::format("ENTITY FIELD WITHOUT NEXT SNAPSHOT: entity {} local client {} field {} cg.time {} "
                                  "snap {} at load frame {}: {}",
                                  s.entity, s.localClientNum, s.field, s.time, s.snap, s.frame, FormatStack(s.stack)));
            }
        }

        void Install()
        {
            HookManager::CreateHook(CG_GET_ENTITY_FIELD, (std::uintptr_t)CG_GetEntityField_Hook,
                                    (std::uintptr_t*)&CG_GetEntityField_Trampoline);
        }
    }  // namespace EntityFields

    // The local client of each entity of the snapshot as its pose has it (the byte at +4, which the client scripts'
    // calls for the entity use): logged the first time one isn't 0 (the only local client of a demo), by the next
    // frame; with IWXMVM_DIAG_WATCHENT=<entity>, a hardware watchpoint on that entity's byte logs what writes it
    namespace EntityClients
    {
        constexpr std::size_t CLIENT_BYTE = 4;
        std::array<std::uint8_t, Structures::MAX_GENTITIES> reported{};
        int watched = -1;
        std::uintptr_t watchedAddress = 0;
        DWORD watcherThread = 0;

        struct Written
        {
            std::uint32_t eip = 0, thread = 0;
            std::uint8_t value = 0;
            std::int32_t frame = 0;
            Stack stack{};
        };
        std::array<Written, 24> writes;
        std::atomic<int> writeCount = 0;
        int writesLogged = 0;

        void Arm(std::uintptr_t address)
        {
            const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 entry{sizeof(entry)};
            for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId())
                    continue;
                if (const auto thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                                   FALSE, entry.th32ThreadID))
                {
                    if (SuspendThread(thread) != static_cast<DWORD>(-1))
                    {
                        CONTEXT context{};
                        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                        if (GetThreadContext(thread, &context))
                        {
                            // DR0, enabled locally, on writes of one byte
                            context.Dr0 = address;
                            context.Dr7 = (context.Dr7 & ~0xF0003u) | 1u | (1u << 16);
                            SetThreadContext(thread, &context);
                        }
                        ResumeThread(thread);
                    }
                    CloseHandle(thread);
                }
            }
            CloseHandle(snapshot);
        }

        LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
        {
            if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 1) ||
                watchedAddress == 0)
                return EXCEPTION_CONTINUE_SEARCH;
            info->ContextRecord->Dr6 &= ~1u;
            if (const auto index = writeCount.load(); index < static_cast<int>(writes.size()))
            {
                writes[index] = {info->ContextRecord->Eip, GetCurrentThreadId(),
                                 *reinterpret_cast<const std::uint8_t*>(watchedAddress), LoadFrames::frame.load(),
                                 CaptureStack(reinterpret_cast<const void*>(info->ContextRecord->Esp))};
                writeCount = index + 1;
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // IWXMVM_DIAG_PROTECTENT: the entities are read-only from the frame after the load's until the demo plays,
        // and the first writes into each page of them are logged with what wrote them (the page is then writable)
        bool protect = false;
        std::uintptr_t protectedStart = 0, protectedEnd = 0;
        struct PageWrite
        {
            std::uint32_t eip = 0, address = 0, thread = 0, value = 0;
            std::int32_t frame = 0;
            std::array<std::uint32_t, 64> raw{};  // the stack from esp, all of it
        };

        // "module+offset" for an address in a loaded module, else empty
        std::string DescribeAddress(std::uint32_t address)
        {
            HMODULE module = nullptr;
            if (address < 0x10000 ||
                !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(address)), &module))
                return {};
            char path[MAX_PATH]{};
            GetModuleFileNameA(module, path, sizeof(path));
            const auto name = std::filesystem::path(path).filename().string();
            return std::format("{}+{:X}", name, address - reinterpret_cast<std::uintptr_t>(module));
        }
        std::array<PageWrite, 64> pageWrites;
        std::atomic<int> pageWriteCount = 0;
        int pageWritesLogged = 0;

        LONG CALLBACK OnAccessViolation(EXCEPTION_POINTERS* info)
        {
            const auto& record = *info->ExceptionRecord;
            if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record.NumberParameters < 2 ||
                record.ExceptionInformation[0] != 1)
                return EXCEPTION_CONTINUE_SEARCH;
            const auto address = static_cast<std::uintptr_t>(record.ExceptionInformation[1]);
            if (address < protectedStart || address >= protectedEnd)
                return EXCEPTION_CONTINUE_SEARCH;
            if (const auto index = pageWriteCount.load(); index < static_cast<int>(pageWrites.size()))
            {
                auto& w = pageWrites[index];
                w.eip = info->ContextRecord->Eip;
                w.address = address;
                w.thread = GetCurrentThreadId();
                w.frame = LoadFrames::frame.load();
                w.value = info->ContextRecord->Eax;
                const auto top = reinterpret_cast<std::uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
                for (std::size_t i = 0; i < w.raw.size(); ++i)
                {
                    const auto at = info->ContextRecord->Esp + i * 4;
                    w.raw[i] = at + 4 <= top ? *reinterpret_cast<const std::uint32_t*>(at) : 0;
                }
                pageWriteCount = index + 1;
            }
            DWORD old = 0;
            VirtualProtect(reinterpret_cast<void*>(address & ~0xFFFu), 0x1000, PAGE_READWRITE, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        void Unprotect()
        {
            if (protectedStart == 0)
                return;
            DWORD old = 0;
            VirtualProtect(reinterpret_cast<void*>(protectedStart), protectedEnd - protectedStart, PAGE_READWRITE, &old);
            Write(std::format("PROTECTENT ended at load frame {}", LoadFrames::frame.load()));
            protectedStart = protectedEnd = 0;
        }

        void OnPreDemoLoad()
        {
            reported.fill(0);
        }

        void OnFrame()
        {
            const auto entities = reinterpret_cast<const std::uint8_t*>(Structures::GetEntities());
            if (!entities)
                return;
            if (protect)
            {
                const auto frame = LoadFrames::frame.load();
                const auto cg = Structures::GetClientGlobals();
                if (frame == 2 && protectedStart == 0)
                {
                    // whole pages inside the entities, so nothing next to them is caught
                    const auto start = (reinterpret_cast<std::uintptr_t>(entities) + 0xFFF) & ~0xFFFu;
                    const auto end = (reinterpret_cast<std::uintptr_t>(entities) +
                                      Structures::MAX_GENTITIES * Structures::CENTITY_SIZE) & ~0xFFFu;
                    DWORD old = 0;
                    if (VirtualProtect(reinterpret_cast<void*>(start), end - start, PAGE_READONLY, &old))
                    {
                        protectedStart = start;
                        protectedEnd = end;
                        Write(std::format("PROTECTENT {:X}-{:X} read-only at load frame {}", start, end, frame));
                    }
                }
                else if (protectedStart != 0 && ((cg && cg->time > 0) || frame >= LoadFrames::FRAMES))
                {
                    Unprotect();
                }
                for (const auto now = pageWriteCount.load(); pageWritesLogged < now; ++pageWritesLogged)
                {
                    const auto& w = pageWrites[pageWritesLogged];
                    const auto offset = w.address - reinterpret_cast<std::uintptr_t>(entities);
                    std::string stack;
                    for (const auto value : w.raw)
                        if (const auto where = DescribeAddress(value); !where.empty())
                            stack += where + " ";
                    Write(std::format("PROTECTENT write to entity {} +{:X} by {:X} ({}) eax {:X} on thread {} at load "
                                      "frame {}: {}",
                                      offset / Structures::CENTITY_SIZE, offset % Structures::CENTITY_SIZE, w.eip,
                                      DescribeAddress(w.eip), w.value, w.thread, w.frame, stack));
                }
            }
            for (std::size_t i = 0; i < Structures::MAX_GENTITIES; ++i)
            {
                const auto value = entities[i * Structures::CENTITY_SIZE + CLIENT_BYTE];
                if (value != 0 && reported[i] != value)
                {
                    reported[i] = value;
                    Write(std::format("ENTITY {} has local client {} at load frame {}", i, value,
                                      LoadFrames::frame.load()));
                }
            }
            if (watched >= 0)
            {
                const auto address = reinterpret_cast<std::uintptr_t>(entities) +
                                     static_cast<std::uintptr_t>(watched) * Structures::CENTITY_SIZE + CLIENT_BYTE;
                if (address != watchedAddress)
                {
                    watchedAddress = address;
                    // (from a thread of its own, so that this one is armed too)
                    std::thread(Arm, address).join();
                    Write(std::format("WATCHENT armed on entity {} at {:X} (now {})", watched, address,
                                      *reinterpret_cast<const std::uint8_t*>(address)));
                }
            }
            for (const auto now = writeCount.load(); writesLogged < now; ++writesLogged)
            {
                const auto& w = writes[writesLogged];
                Write(std::format("WATCHENT write of {} by {:X} on thread {} at load frame {}: {}", w.value, w.eip,
                                  w.thread, w.frame, FormatStack(w.stack)));
            }
        }

        void Install()
        {
            char value[16]{};
            if (GetEnvironmentVariableA("IWXMVM_DIAG_WATCHENT", value, sizeof(value)))
            {
                watched = std::atoi(value);
                AddVectoredExceptionHandler(1, OnException);
            }
            if (GetEnvironmentVariableA("IWXMVM_DIAG_PROTECTENT", value, sizeof(value)))
            {
                protect = true;
                AddVectoredExceptionHandler(1, OnAccessViolation);
            }
        }
    }  // namespace EntityClients

    // The crashes seen while a demo loads: CG_DoControllers finding no cg_entities (0x63D8D5), a player's weapon
    // without a WeaponVariantDef (0x529FFB, BG_GetWeaponDef), and the scene's DObjs where one added this frame is gone
    // (0x58E014, its scene entity in esi with the DObj's handle at +0x6E: actors take theirs from 0x5FF down)
    std::size_t CountWeaponVariants()
    {
        const auto variants =
            reinterpret_cast<Structures::WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        std::size_t count = 0;
        for (std::size_t index = 1; index < Structures::MAX_WEAPON_VARIANTS; ++index)
            count += variants[index] != nullptr;
        return count;
    }

    LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
    {
        const auto& record = *info->ExceptionRecord;
        const auto address = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
        if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
            (address != 0x63D8D5 && address != 0x529FFB && address != 0x58E014))
            return EXCEPTION_CONTINUE_SEARCH;
        static std::atomic<int> logged = 0;
        if (logged++ > 3)
            return EXCEPTION_CONTINUE_SEARCH;

        const auto& context = *info->ContextRecord;
        const auto cg = Structures::GetClientGlobals();
        const auto entities = Structures::GetEntities();
        std::string entity;
        if (address == 0x529FFB && entities)
        {
            // BG_GetWeaponDef(weapon) for the entity in esi of its caller
            const auto cent = reinterpret_cast<const Structures::centity_t*>(context.Esi);
            const auto number = cent - entities;
            if (number >= 0 && number < static_cast<std::ptrdiff_t>(Structures::MAX_GENTITIES))
                entity = std::format("entity {} eType {} weapon {} primary {} client {}", number,
                                     static_cast<int>(cent->nextState.eType), cent->nextState.weapon,
                                     cent->nextState.primaryWeapon, cent->nextState.clientNum);
        }
        if (address == 0x58E014)
            entity = std::format("scene entity handle {:X}", *reinterpret_cast<const std::uint16_t*>(context.Esi + 0x6E));
        Write(std::format("CRASH at {:X} reading {:X}: eax {:X} ebx {:X} ecx {:X} edx {:X} esi {:X} edi {:X} ebp {:X}; "
                          "cg_entities {} cg {} snap {} rewinding {}; {} weapon variants; {}; stack {}",
                          address, record.ExceptionInformation[1], context.Eax, context.Ebx, context.Ecx, context.Edx,
                          context.Esi, context.Edi, context.Ebp, static_cast<const void*>(entities),
                          static_cast<const void*>(cg), cg ? static_cast<const void*>(cg->snap) : nullptr,
                          Components::Rewinding::IsRewinding(), CountWeaponVariants(), entity,
                          ScanStack(reinterpret_cast<const void*>(context.Esp))));
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void OnMainThreadFrame()
    {
        Threads::mainThread = GetCurrentThreadId();
        MemoryTree::Check();
        Variables::Check();
        LoadFrames::OnFrame();
        Stale::OnFrame();
        EntityFields::OnFrame();
        EntityClients::OnFrame();
        static std::size_t repairs = 0;
        if (const auto now = Hooks::ScriptMemory::GetRepairCount(); now != repairs)
        {
            Write(std::format("MT GUARD repairs {} (+{})", now, now - repairs));
            repairs = now;
        }
        static std::size_t entityFixes = 0;
        if (const auto now = Hooks::EntityClients::GetFixCount(); now != entityFixes)
        {
            Write(std::format("ENTITY GUARD put {} entities' local client right (+{}) at load frame {}", now,
                              now - entityFixes, LoadFrames::frame.load()));
            entityFixes = now;
        }
    }

    void* exceptionHandler = nullptr;

    void Install(const std::filesystem::path& directory)
    {
        file.open(directory / "diag.txt");
        const auto [moduleStart, moduleEnd] = GetModuleRange();
        Write(std::format("diagnostics installed, module {:X}-{:X}", moduleStart, moduleEnd));
        // IWXMVM_DIAG_STRINGS: report (and skip) reference changes of strings that have none left; it changes what
        // the game does, so it is off unless asked for
        if (char flag[8]{}; GetEnvironmentVariableA("IWXMVM_DIAG_STRINGS", flag, sizeof(flag)))
            ScriptStrings::Install();
        // with IWXMVM_DIAG_MT set; the tree is checked once a frame either way
        // IWXMVM_DIAG_WATCH: hardware watchpoints on freed blocks, with the hooks only keeping what is free
        char value[8]{};
        const bool snapshots = GetEnvironmentVariableA("IWXMVM_DIAG_MT", value, sizeof(value)) != 0;
        const bool watch = GetEnvironmentVariableA("IWXMVM_DIAG_WATCH", value, sizeof(value)) != 0;
        // (they hook MT_AllocIndex and MT_FreeIndex, which Hooks/ScriptMemory.cpp has hooked since; IWXMVM_DIAG_STALE
        // keeps each block's history through the guard instead)
        if (snapshots || watch)
            Write("IWXMVM_DIAG_MT and IWXMVM_DIAG_WATCH are left out: the script memory guard hooks the same functions");
        if (GetEnvironmentVariableA("IWXMVM_DIAG_THREADS", value, sizeof(value)))
            Threads::Install();
        if (GetEnvironmentVariableA("IWXMVM_DIAG_STALE", value, sizeof(value)))
            Stale::Install();
        exceptionHandler = AddVectoredExceptionHandler(1, OnException);
        LoadFrames::Install();
        EntityFields::Install();
        EntityClients::Install();

        // when the demo loads and rewinds, against which the reports are read
        Events::RegisterListener(EventType::PreDemoLoad, [] {
            Write("EVENT PreDemoLoad");
            Variables::Reset();
            LoadFrames::OnPreDemoLoad();
            EntityClients::OnPreDemoLoad();
        });
        Events::RegisterListener(EventType::PostDemoLoad,
                                 [] { Write(std::format("EVENT PostDemoLoad, {} weapon variants", CountWeaponVariants())); });
        Events::RegisterListener(EventType::OnRewindCompleted, [] { Write("EVENT rewind completed"); });
    }
    void Uninstall()
    {
        if (exceptionHandler)
            RemoveVectoredExceptionHandler(exceptionHandler);
        exceptionHandler = nullptr;
    }

    void OnEarlyServerCommands(int from, int to)
    {
        LoadFrames::earlyFrom = from;
        LoadFrames::earlyTo = to;
        ++LoadFrames::earlyRuns;
    }

    void OnRenderThreadFrame()
    {
        Threads::renderThread = GetCurrentThreadId();
    }

    std::string DescribeScriptThreads()
    {
        return Threads::Describe();
    }
}  // namespace IWXMVM::T5::Diagnostics
