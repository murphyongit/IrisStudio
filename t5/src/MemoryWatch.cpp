// Harness-only memory watch (built with /p:T5Harness=true), see MemoryWatch.hpp
#include "StdInclude.hpp"
#include "MemoryWatch.hpp"

#include <intrin.h>
#include <map>
#include <optional>
#include <Psapi.h>

#include "Utilities/HookManager.hpp"

namespace IWXMVM::T5::MemoryWatch
{
    namespace
    {
        constexpr double MB = 1048576.0;

        // the modules' images, sorted, made as the watch goes on (the hooks only read it while on)
        struct Module
        {
            std::uintptr_t start = 0, end = 0;
            std::string name;
            bool allocator = false;  // the heap's and the runtime's own code: its frames say nothing of who asked
        };
        std::vector<Module> modules;

        // the game's own code, inside the bootstrapper's image
        constexpr std::uintptr_t GAME_CODE_START = 0x401000, GAME_CODE_END = 0xA00000;
        const Module GAME_MODULE{GAME_CODE_START, GAME_CODE_END, "game", false};

        // a heap's blocks of this size and up are given memory of their own (VirtualAlloc), back to the system when freed
        constexpr std::uint64_t LARGE_BLOCK = 0x7F000;
        // the game's physical memory, where map zones and sound banks load: its two
        // stacks, each a name, a count and the position it has reached (PoolGrowth.cpp's PhysicalMemory)
        constexpr std::uintptr_t PHYSICAL_MEMORY = 0x397D688, PHYSICAL_MEMORY_STRIDE = 0x10C, PHYSICAL_MEMORY_POSITION = 8;

        std::uintptr_t GetPhysicalMemoryPosition(std::size_t stack)
        {
            return *reinterpret_cast<const std::uint32_t*>(PHYSICAL_MEMORY + stack * PHYSICAL_MEMORY_STRIDE +
                                                           PHYSICAL_MEMORY_POSITION);
        }

        constexpr std::size_t FRAMES = 16;
        using Caller = std::array<std::uint32_t, FRAMES>;

        // the tables allocate from a heap of their own, which the hooks let through untouched
        HANDLE ownHeap = nullptr;

        template <typename T>
        struct OwnAllocator
        {
            using value_type = T;
            OwnAllocator() = default;
            template <typename U>
            OwnAllocator(const OwnAllocator<U>&) noexcept
            {
            }
            T* allocate(std::size_t count)
            {
                if (const auto memory = HeapAlloc(ownHeap, 0, count * sizeof(T)))
                    return static_cast<T*>(memory);
                throw std::bad_alloc();
            }
            void deallocate(T* pointer, std::size_t) noexcept
            {
                HeapFree(ownHeap, 0, pointer);
            }
            template <typename U>
            bool operator==(const OwnAllocator<U>&) const noexcept
            {
                return true;
            }
        };

        struct Held
        {
            std::uint32_t size = 0;
            std::uint64_t serial = 0;
            Caller caller{};
            HANDLE heap = nullptr;  // null for a reservation of address space
        };
        using Table = std::unordered_map<std::uintptr_t, Held, std::hash<std::uintptr_t>, std::equal_to<std::uintptr_t>,
                                         OwnAllocator<std::pair<const std::uintptr_t, Held>>>;
        struct Freed
        {
            std::uint64_t bytes = 0;
            std::uint64_t count = 0;
        };
        using FreedTable = std::map<Caller, Freed, std::less<Caller>, OwnAllocator<std::pair<const Caller, Freed>>>;
        // the heap blocks each caller holds now and held at most since the mark: the heap keeps the address space of
        // its peaks (its segments are never given back), so who made a peak is what a report of what's held misses
        struct CallerHash
        {
            std::size_t operator()(const Caller& caller) const noexcept
            {
                std::size_t hash = 0;
                for (const auto frame : caller)
                    hash = hash * 1000003u ^ frame;
                return hash;
            }
        };
        struct Usage
        {
            std::int64_t live = 0, peak = 0;
            std::int64_t blocks = 0;
        };
        using UsageTable = std::unordered_map<Caller, Usage, CallerHash, std::equal_to<Caller>,
                                              OwnAllocator<std::pair<const Caller, Usage>>>;

        SRWLOCK lock = SRWLOCK_INIT;
        std::optional<Table> blocks, reservations;
        std::optional<FreedTable> freedSinceMark;  // of what was held at the mark and freed since, by caller (heap
                                                   // blocks and reservations apart: the heap's in [0], see Report)
        std::optional<FreedTable> freedReservationsSinceMark;
        std::optional<UsageTable> usage;
        std::int64_t liveTotal = 0, peakTotal = 0;  // of the heap blocks watched, under the lock
        std::uint64_t serial = 0;  // under the lock
        std::uint64_t markSerial = 0;
        std::atomic<bool> on = false;
        std::atomic<std::size_t> minimum = 0;

        // The thread holding the lock (exclusively) is let through by the hooks: what it allocates or frees isn't
        // followed. Its tables grow the heap of their own, which reserves address space, and the lock isn't reentrant.
        // A flag per thread was tried first: a thread_local read in a hook as a new thread's loader allocates that
        // thread's TLS, then an explicit TLS slot, whose TlsSetValue past slot 64 allocates the thread's expansion
        // slots through RtlAllocateHeap under the loader's PEB lock: the game overflowed its stack (0xC00000FD) and
        // then froze at its first zone load. A thread id needs nothing allocated.
        std::atomic<DWORD> lockOwner = 0;

        void Lock()
        {
            AcquireSRWLockExclusive(&lock);
            lockOwner = GetCurrentThreadId();
        }

        void Unlock()
        {
            lockOwner = 0;
            ReleaseSRWLockExclusive(&lock);
        }

        const Module* FindModule(std::uintptr_t address)
        {
            if (address >= GAME_CODE_START && address < GAME_CODE_END)
                return &GAME_MODULE;
            const auto found = std::upper_bound(modules.begin(), modules.end(), address,
                                                [](std::uintptr_t value, const Module& module) {
                                                    return value < module.start;
                                                });
            if (found == modules.begin())
                return nullptr;
            const auto& module = *std::prev(found);
            return address < module.end ? &module : nullptr;
        }

        // whether the code before an address ends with a call, so that the address is a return address
        bool FollowsCall(std::uintptr_t address)
        {
            const auto code = reinterpret_cast<const std::uint8_t*>(address);
            __try
            {
                if (code[-5] == 0xE8)  // call rel32
                    return true;
                if (code[-6] == 0xFF && (code[-5] == 0x15 || (code[-5] & 0xF8) == 0x90))  // [disp32], [reg+disp32]
                    return true;
                if (code[-7] == 0xFF && code[-6] == 0x94)  // [sib+disp32]
                    return true;
                if (code[-3] == 0xFF && (code[-2] & 0xF8) == 0x50)  // [reg+disp8]
                    return true;
                if (code[-4] == 0xFF && code[-3] == 0x54)  // [sib+disp8]
                    return true;
                if (code[-2] == 0xFF && ((code[-1] & 0xF8) == 0xD0 || (code[-1] & 0xF8) == 0x10))  // reg, [reg]
                    return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return false;
        }

        // The committed executable memory of the modules, sorted, made with them: a word of the stack is read as a
        // return address only inside one, so the bytes before it are read without a fault. With every allocation
        // watched, words that only looked like addresses in Plutonium's 342 MB image were read where nothing is
        // committed, and each fault went through the exception handlers of the game and Plutonium first: the game froze
        // or overflowed its stack.
        struct CodeRegion
        {
            std::uintptr_t start = 0, end = 0;
            bool allocator = false;
        };
        std::vector<CodeRegion> codeRegions;

        void ReadCodeRegions()
        {
            codeRegions.clear();
            const auto add = [](std::uintptr_t start, std::uintptr_t end, bool allocator) {
                MEMORY_BASIC_INFORMATION info{};
                for (auto at = start; at < end && VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info));
                     at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize)
                {
                    constexpr DWORD EXECUTABLE =
                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                    if (info.State != MEM_COMMIT || !(info.Protect & EXECUTABLE) || (info.Protect & PAGE_GUARD))
                        continue;
                    const auto regionStart = std::max(start, reinterpret_cast<std::uintptr_t>(info.BaseAddress));
                    const auto regionEnd = std::min<std::uintptr_t>(end, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
                    if (!codeRegions.empty() && codeRegions.back().end == regionStart &&
                        codeRegions.back().allocator == allocator)
                        codeRegions.back().end = regionEnd;
                    else
                        codeRegions.push_back({regionStart, regionEnd, allocator});
                }
            };
            add(GAME_CODE_START, GAME_CODE_END, false);
            for (const auto& module : modules)
            {
                // (the game's code is in the bootstrapper's image, taken above)
                if (module.start <= GAME_CODE_START && module.end >= GAME_CODE_END)
                {
                    add(module.start, GAME_CODE_START, module.allocator);
                    add(GAME_CODE_END, module.end, module.allocator);
                }
                else
                    add(module.start, module.end, module.allocator);
            }
            std::ranges::sort(codeRegions, {}, &CodeRegion::start);
        }

        const CodeRegion* FindCode(std::uintptr_t address)
        {
            const auto found = std::upper_bound(codeRegions.begin(), codeRegions.end(), address,
                                                [](std::uintptr_t value, const CodeRegion& region) {
                                                    return value < region.start;
                                                });
            if (found == codeRegions.begin())
                return nullptr;
            const auto& region = *std::prev(found);
            return address <= region.end && address >= region.start + 7 ? &region : nullptr;
        }

        // the watch's own functions (filled as it goes on): words left on a stack by an earlier call of a hook are stale
        std::array<std::uintptr_t, 8> ownCode{};

        bool IsOwnCode(std::uintptr_t address)
        {
            return std::ranges::any_of(ownCode, [&](std::uintptr_t start) {
                return start && address >= start && address < start + 0x400;
            });
        }

        // the return addresses on the stack from a point, past the heap's and the runtime's own (as the game's code
        // has no frame pointers, one may be left over from a call that returned)
        Caller Capture(const void* from)
        {
            Caller caller{};
            std::size_t found = 0;
            const auto top = reinterpret_cast<std::uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
            auto word = static_cast<const std::uint32_t*>(from);
            for (int i = 0; i < 400 && found < FRAMES && reinterpret_cast<std::uintptr_t>(word + 1) <= top; ++i, ++word)
            {
                const auto value = *word;
                const auto region = FindCode(value);
                if (region && !region->allocator && !IsOwnCode(value) && FollowsCall(value))
                    caller[found++] = value;
            }
            return caller;
        }

        // a heap block gone from its caller's usage (under the lock)
        void Uncount(const Held& block)
        {
            if (!block.heap)
                return;
            if (const auto found = usage->find(block.caller); found != usage->end())
            {
                found->second.live -= block.size;
                --found->second.blocks;
            }
            liveTotal -= block.size;
        }

        void Note(HANDLE heap, const void* pointer, std::size_t size, const Caller& caller)
        {
            Lock();
            try
            {
                auto& table = heap ? *blocks : *reservations;
                const auto address = reinterpret_cast<std::uintptr_t>(pointer);
                if (heap)
                {
                    if (const auto old = table.find(address); old != table.end())
                        Uncount(old->second);
                    auto& use = (*usage)[caller];
                    use.live += static_cast<std::int64_t>(size);
                    use.peak = std::max(use.peak, use.live);
                    ++use.blocks;
                    liveTotal += static_cast<std::int64_t>(size);
                    peakTotal = std::max(peakTotal, liveTotal);
                }
                table.insert_or_assign(address, Held{static_cast<std::uint32_t>(size), ++serial, caller, heap});
            }
            catch (...)
            {
            }
            Unlock();
        }

        void Forget(bool heap, std::uintptr_t address)
        {
            auto& table = heap ? *blocks : *reservations;
            AcquireSRWLockShared(&lock);
            const bool held = table.contains(address);
            ReleaseSRWLockShared(&lock);
            if (!held)
                return;
            Lock();
            if (const auto found = table.find(address); found != table.end())
            {
                Uncount(found->second);
                if (found->second.serial <= markSerial)
                {
                    try
                    {
                        auto& freed = (heap ? *freedSinceMark : *freedReservationsSinceMark)[found->second.caller];
                        freed.bytes += found->second.size;
                        ++freed.count;
                    }
                    catch (...)
                    {
                    }
                }
                table.erase(found);
            }
            Unlock();
        }

        bool IsFollowed()
        {
            return on.load(std::memory_order_relaxed) && GetCurrentThreadId() != lockOwner.load(std::memory_order_relaxed);
        }

        using RtlAllocateHeap_t = void*(NTAPI*)(void* heap, ULONG flags, SIZE_T size);
        using RtlReAllocateHeap_t = void*(NTAPI*)(void* heap, ULONG flags, void* pointer, SIZE_T size);
        using RtlFreeHeap_t = BOOLEAN(NTAPI*)(void* heap, ULONG flags, void* pointer);
        using NtAllocateVirtualMemory_t = LONG(NTAPI*)(HANDLE process, void** base, ULONG_PTR zeroBits, SIZE_T* size,
                                                       ULONG type, ULONG protect);
        using NtAllocateVirtualMemoryEx_t = LONG(NTAPI*)(HANDLE process, void** base, SIZE_T* size, ULONG type,
                                                         ULONG protect, void* parameters, ULONG count);
        using NtFreeVirtualMemory_t = LONG(NTAPI*)(HANDLE process, void** base, SIZE_T* size, ULONG type);
        RtlAllocateHeap_t RtlAllocateHeap_Trampoline = nullptr;
        RtlReAllocateHeap_t RtlReAllocateHeap_Trampoline = nullptr;
        RtlFreeHeap_t RtlFreeHeap_Trampoline = nullptr;
        NtAllocateVirtualMemory_t NtAllocateVirtualMemory_Trampoline = nullptr;
        NtAllocateVirtualMemoryEx_t NtAllocateVirtualMemoryEx_Trampoline = nullptr;
        NtFreeVirtualMemory_t NtFreeVirtualMemory_Trampoline = nullptr;

        void* NTAPI RtlAllocateHeap_Hook(void* heap, ULONG flags, SIZE_T size)
        {
            const auto pointer = RtlAllocateHeap_Trampoline(heap, flags, size);
            if (pointer && heap != ownHeap && size >= minimum.load(std::memory_order_relaxed) && IsFollowed())
                Note(heap, pointer, size, Capture(_AddressOfReturnAddress()));
            return pointer;
        }

        void* NTAPI RtlReAllocateHeap_Hook(void* heap, ULONG flags, void* pointer, SIZE_T size)
        {
            if (heap == ownHeap || !IsFollowed())
                return RtlReAllocateHeap_Trampoline(heap, flags, pointer, size);
            if (pointer)
                Forget(true, reinterpret_cast<std::uintptr_t>(pointer));
            const auto moved = RtlReAllocateHeap_Trampoline(heap, flags, pointer, size);
            if (moved && size >= minimum.load(std::memory_order_relaxed))
                Note(heap, moved, size, Capture(_AddressOfReturnAddress()));
            return moved;
        }

        BOOLEAN NTAPI RtlFreeHeap_Hook(void* heap, ULONG flags, void* pointer)
        {
            // (forgotten before it is freed: another thread may be given the same address right after)
            if (pointer && heap != ownHeap && IsFollowed())
                Forget(true, reinterpret_cast<std::uintptr_t>(pointer));
            return RtlFreeHeap_Trampoline(heap, flags, pointer);
        }

        LONG NTAPI NtAllocateVirtualMemory_Hook(HANDLE process, void** base, ULONG_PTR zeroBits, SIZE_T* size,
                                                ULONG type, ULONG protect)
        {
            const auto status = NtAllocateVirtualMemory_Trampoline(process, base, zeroBits, size, type, protect);
            if (status >= 0 && (type & MEM_RESERVE) && process == GetCurrentProcess() && base && *base && size &&
                IsFollowed())
                Note(nullptr, *base, *size, Capture(_AddressOfReturnAddress()));
            return status;
        }

        LONG NTAPI NtAllocateVirtualMemoryEx_Hook(HANDLE process, void** base, SIZE_T* size, ULONG type, ULONG protect,
                                                  void* parameters, ULONG count)
        {
            const auto status =
                NtAllocateVirtualMemoryEx_Trampoline(process, base, size, type, protect, parameters, count);
            if (status >= 0 && (type & MEM_RESERVE) && process == GetCurrentProcess() && base && *base && size &&
                IsFollowed())
                Note(nullptr, *base, *size, Capture(_AddressOfReturnAddress()));
            return status;
        }

        LONG NTAPI NtFreeVirtualMemory_Hook(HANDLE process, void** base, SIZE_T* size, ULONG type)
        {
            if ((type & MEM_RELEASE) && process == GetCurrentProcess() && base && *base && IsFollowed())
                Forget(false, reinterpret_cast<std::uintptr_t>(*base));
            return NtFreeVirtualMemory_Trampoline(process, base, size, type);
        }

        void ReadModules()
        {
            modules.clear();
            std::vector<HMODULE> handles(1024);
            DWORD needed = 0;
            if (!EnumProcessModules(GetCurrentProcess(), handles.data(),
                                    static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed))
                return;
            handles.resize(std::min<std::size_t>(handles.size(), needed / sizeof(HMODULE)));
            for (const auto handle : handles)
            {
                MODULEINFO info{};
                char name[MAX_PATH]{};
                if (!GetModuleInformation(GetCurrentProcess(), handle, &info, sizeof(info)) ||
                    !GetModuleBaseNameA(GetCurrentProcess(), handle, name, MAX_PATH))
                    continue;
                std::string lower = name;
                std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool allocator = lower == "ntdll.dll" || lower == "kernelbase.dll" || lower == "kernel32.dll" ||
                                       lower == "ucrtbase.dll" || lower == "msvcrt.dll" || lower.starts_with("msvcr") ||
                                       lower.starts_with("msvcp") || lower.starts_with("vcruntime");
                const auto start = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
                modules.push_back({start, start + info.SizeOfImage, name, allocator});
            }
            std::ranges::sort(modules, {}, &Module::start);
            ReadCodeRegions();
        }

        std::string FormatAddress(std::uint32_t address)
        {
            if (address >= GAME_CODE_START && address < GAME_CODE_END)
                return std::format("{:X}", address);
            if (const auto module = FindModule(address))
                return std::format("{}+{:X}", module->name, address - module->start);
            return std::format("{:X}", address);
        }

        std::string FormatCaller(const Caller& caller)
        {
            std::string text;
            for (const auto frame : caller)
                if (frame)
                    text += (text.empty() ? "" : " < ") + FormatAddress(frame);
            return text.empty() ? "(no frame found)" : text;
        }

        // the module a caller's first frame is in
        std::string GetCallerModule(const Caller& caller)
        {
            if (!caller[0])
                return "(no frame found)";
            const auto module = FindModule(caller[0]);
            return module ? module->name : "(unknown)";
        }

        template <typename Function>
        HeapUse Walk(HANDLE heap, Function&& onEntry)
        {
            HeapUse use{heap};
            if (!HeapLock(heap))
                return use;
            __try
            {
                PROCESS_HEAP_ENTRY entry{};
                while (HeapWalk(heap, &entry))
                {
                    if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
                    {
                        use.busy += entry.cbData;
                        ++use.blocks;
                        if (entry.cbData >= LARGE_BLOCK)
                            use.large += entry.cbData;
                    }
                    else if (entry.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE)
                        use.uncommitted += entry.cbData;
                    else if (!(entry.wFlags & PROCESS_HEAP_REGION))
                        use.free += entry.cbData;
                    onEntry(entry);
                }
                use.walked = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                use.walked = false;
            }
            HeapUnlock(heap);
            return use;
        }
    }  // namespace

    HeapUse WalkHeap(HANDLE heap)
    {
        return Walk(heap, [](const PROCESS_HEAP_ENTRY&) {});
    }

    bool Start(std::size_t minimumBytes, std::string* error)
    {
        minimum = minimumBytes;
        if (on)
            return true;
        if (!ownHeap)
            ownHeap = HeapCreate(0, 0, 0);
        if (!ownHeap)
        {
            if (error)
                *error = "no heap of its own";
            return false;
        }
        // (the tables are cleared, not made again: a thread may still be in a hook from before the watch went off)
        Lock();
        for (auto* table : {&blocks, &reservations})
            table->has_value() ? (*table)->clear() : (void)table->emplace();
        for (auto* table : {&freedSinceMark, &freedReservationsSinceMark})
            table->has_value() ? (*table)->clear() : (void)table->emplace();
        usage.has_value() ? usage->clear() : (void)usage.emplace();
        liveTotal = peakTotal = 0;
        serial = 0;
        markSerial = 0;
        Unlock();
        ReadModules();

        ownCode = {reinterpret_cast<std::uintptr_t>(RtlAllocateHeap_Hook),
                   reinterpret_cast<std::uintptr_t>(RtlReAllocateHeap_Hook),
                   reinterpret_cast<std::uintptr_t>(RtlFreeHeap_Hook),
                   reinterpret_cast<std::uintptr_t>(NtAllocateVirtualMemory_Hook),
                   reinterpret_cast<std::uintptr_t>(NtAllocateVirtualMemoryEx_Hook),
                   reinterpret_cast<std::uintptr_t>(NtFreeVirtualMemory_Hook),
                   reinterpret_cast<std::uintptr_t>(Note),
                   reinterpret_cast<std::uintptr_t>(Forget)};
        static bool hooked = false;
        if (!hooked)
        {
            const auto ntdll = GetModuleHandleA("ntdll.dll");
            const auto find = [&](const char* name) {
                return reinterpret_cast<std::uintptr_t>(GetProcAddress(ntdll, name));
            };
            try
            {
                HookManager::CreateHook(find("RtlAllocateHeap"), (std::uintptr_t)RtlAllocateHeap_Hook,
                                        (std::uintptr_t*)&RtlAllocateHeap_Trampoline);
                HookManager::CreateHook(find("RtlReAllocateHeap"), (std::uintptr_t)RtlReAllocateHeap_Hook,
                                        (std::uintptr_t*)&RtlReAllocateHeap_Trampoline);
                HookManager::CreateHook(find("RtlFreeHeap"), (std::uintptr_t)RtlFreeHeap_Hook,
                                        (std::uintptr_t*)&RtlFreeHeap_Trampoline);
                HookManager::CreateHook(find("NtAllocateVirtualMemory"), (std::uintptr_t)NtAllocateVirtualMemory_Hook,
                                        (std::uintptr_t*)&NtAllocateVirtualMemory_Trampoline);
                if (const auto ex = find("NtAllocateVirtualMemoryEx"))
                    HookManager::CreateHook(ex, (std::uintptr_t)NtAllocateVirtualMemoryEx_Hook,
                                            (std::uintptr_t*)&NtAllocateVirtualMemoryEx_Trampoline);
                HookManager::CreateHook(find("NtFreeVirtualMemory"), (std::uintptr_t)NtFreeVirtualMemory_Hook,
                                        (std::uintptr_t*)&NtFreeVirtualMemory_Trampoline);
            }
            catch (const std::exception& exception)
            {
                if (error)
                    *error = exception.what();
                return false;
            }
            hooked = true;
        }
        on = true;
        return true;
    }

    void Stop()
    {
        // (the hooks stay, letting everything through: a thread may be in one)
        on = false;
    }

    bool IsOn()
    {
        return on;
    }

    void Mark()
    {
        Lock();
        markSerial = serial;
        if (freedSinceMark)
            freedSinceMark->clear();
        if (freedReservationsSinceMark)
            freedReservationsSinceMark->clear();
        if (usage)
            for (auto& [caller, use] : *usage)
                use.peak = use.live;
        peakTotal = liveTotal;
        Unlock();
    }

    std::vector<std::string> Report(bool sinceMark, std::size_t rows)
    {
        struct Sum
        {
            std::uint64_t bytes = 0, count = 0;
        };
        std::map<Caller, Sum> heapSums;
        std::map<std::string, Sum> heapModules, reservationModules;
        // (copied under the lock into the watch's own heap: a thread growing the process's heap holds that heap's lock
        // while it waits for the watch's, so nothing allocates from it under the watch's lock)
        std::vector<std::pair<std::uintptr_t, Held>, OwnAllocator<std::pair<std::uintptr_t, Held>>> blocksHeld, held;
        std::vector<std::pair<Caller, Freed>, OwnAllocator<std::pair<Caller, Freed>>> freedBlocks, freedReservations;
        Lock();
        if (!blocks)
        {
            Unlock();
            return {"MEMWATCH never on"};
        }
        try
        {
            blocksHeld.reserve(blocks->size());
            for (const auto& [address, block] : *blocks)
                if (!sinceMark || block.serial > markSerial)
                    blocksHeld.emplace_back(address, block);
            for (const auto& [address, reservation] : *reservations)
                if (!sinceMark || reservation.serial > markSerial)
                    held.emplace_back(address, reservation);
            if (sinceMark)
            {
                freedBlocks.assign(freedSinceMark->begin(), freedSinceMark->end());
                freedReservations.assign(freedReservationsSinceMark->begin(), freedReservationsSinceMark->end());
            }
        }
        catch (...)
        {
        }
        const auto minimumBytes = minimum.load();
        Unlock();
        // each held block checked against a walk of its heap: one the walk hasn't in use was freed some way the hooks
        // didn't see (a heap destroyed whole), and isn't counted
        std::uint64_t staleBytes = 0, staleCount = 0;
        {
            std::ranges::sort(blocksHeld, {}, [](const auto& each) { return std::pair(each.second.heap, each.first); });
            std::vector<char> found(blocksHeld.size());
            for (std::size_t from = 0; from < blocksHeld.size();)
            {
                const auto heap = blocksHeld[from].second.heap;
                auto to = from;
                while (to < blocksHeld.size() && blocksHeld[to].second.heap == heap)
                    ++to;
                // (nothing allocates during the walk: the heap walked may be the one allocated from)
                Walk(heap, [&](const PROCESS_HEAP_ENTRY& entry) {
                    if (!(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY))
                        return;
                    const auto address = reinterpret_cast<std::uintptr_t>(entry.lpData);
                    const auto at = std::lower_bound(blocksHeld.begin() + from, blocksHeld.begin() + to, address,
                                                     [](const auto& each, std::uintptr_t value) {
                                                         return each.first < value;
                                                     });
                    if (at != blocksHeld.begin() + to && at->first == address)
                        found[at - blocksHeld.begin()] = 1;
                });
                from = to;
            }
            for (std::size_t i = 0; i < blocksHeld.size(); ++i)
                if (!found[i])
                {
                    staleBytes += blocksHeld[i].second.size;
                    ++staleCount;
                }
            std::size_t kept = 0;
            for (std::size_t i = 0; i < blocksHeld.size(); ++i)
                if (found[i])
                    blocksHeld[kept++] = blocksHeld[i];
            blocksHeld.resize(kept);
        }
        for (const auto& [address, block] : blocksHeld)
        {
            auto& sum = heapSums[block.caller];
            sum.bytes += block.size;
            ++sum.count;
        }

        // what of each reservation is committed now
        std::uint64_t reservedTotal = 0, committedTotal = 0;
        std::map<Caller, std::pair<Sum, std::uint64_t>> reservationTotals;
        for (const auto& [address, reservation] : held)
        {
            std::uint64_t committed = 0;
            MEMORY_BASIC_INFORMATION info{};
            for (auto at = address; at < address + reservation.size &&
                                    VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info));
                 at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize)
            {
                if (reinterpret_cast<std::uintptr_t>(info.AllocationBase) != address)
                    break;
                if (info.State == MEM_COMMIT)
                    committed += info.RegionSize;
            }
            auto& [sum, sumCommitted] = reservationTotals[reservation.caller];
            sum.bytes += reservation.size;
            ++sum.count;
            sumCommitted += committed;
            reservedTotal += reservation.size;
            committedTotal += committed;
            auto& module = reservationModules[GetCallerModule(reservation.caller)];
            module.bytes += committed;
            ++module.count;
        }

        std::vector<std::string> lines;
        std::uint64_t heapTotal = 0, heapCount = 0;
        for (const auto& [caller, sum] : heapSums)
        {
            heapTotal += sum.bytes;
            heapCount += sum.count;
            auto& module = heapModules[GetCallerModule(caller)];
            module.bytes += sum.bytes;
            module.count += sum.count;
        }
        const auto since = sinceMark ? " taken since the mark" : "";
        lines.push_back(std::format("MEMWATCH heap blocks of {} bytes and up{}, held: {:.1f} MB in {} ({:.1f} MB in {} "
                                    "more were gone from their heaps, not counted)",
                                    minimumBytes, since, heapTotal / MB, heapCount, staleBytes / MB, staleCount));
        const auto describeModules = [&](const std::map<std::string, Sum>& sums) {
            std::vector<std::pair<std::string, Sum>> sorted(sums.begin(), sums.end());
            std::ranges::sort(sorted, std::greater{}, [](const auto& each) { return each.second.bytes; });
            std::string text;
            for (const auto& [module, sum] : sorted)
                text += std::format(" {} {:.1f} MB ({}),", module, sum.bytes / MB, sum.count);
            return text.empty() ? std::string(" none") : text.substr(0, text.size() - 1);
        };
        lines.push_back("MEMWATCH heap by the first frame's module:" + describeModules(heapModules));
        {
            std::vector<std::pair<Caller, Sum>> sorted(heapSums.begin(), heapSums.end());
            std::ranges::sort(sorted, std::greater{}, [](const auto& each) { return each.second.bytes; });
            for (std::size_t i = 0; i < std::min(rows, sorted.size()); ++i)
                lines.push_back(std::format("MEMWATCH heap {:.2f} MB in {}: {}", sorted[i].second.bytes / MB,
                                            sorted[i].second.count, FormatCaller(sorted[i].first)));
        }
        lines.push_back(std::format("MEMWATCH reservations{}, held: {:.1f} MB reserved, {:.1f} MB committed now, in {}",
                                    since, reservedTotal / MB, committedTotal / MB, held.size()));
        lines.push_back("MEMWATCH reservations' committed by the first frame's module:" +
                        describeModules(reservationModules));
        {
            std::vector<std::pair<Caller, std::pair<Sum, std::uint64_t>>> sorted(reservationTotals.begin(),
                                                                                  reservationTotals.end());
            std::ranges::sort(sorted, std::greater{}, [](const auto& each) { return each.second.second; });
            for (std::size_t i = 0; i < std::min(rows, sorted.size()); ++i)
                lines.push_back(std::format("MEMWATCH reservation {:.2f} MB committed of {:.2f} in {}: {}",
                                            sorted[i].second.second / MB, sorted[i].second.first.bytes / MB,
                                            sorted[i].second.first.count, FormatCaller(sorted[i].first)));
        }
        if (sinceMark)
        {
            for (auto* freed : {&freedBlocks, &freedReservations})
            {
                std::ranges::sort(*freed, std::greater{}, [](const auto& each) { return each.second.bytes; });
                std::uint64_t total = 0, count = 0;
                for (const auto& [caller, each] : *freed)
                {
                    total += each.bytes;
                    count += each.count;
                }
                const auto kind = freed == &freedBlocks ? "heap blocks" : "reservations";
                lines.push_back(std::format("MEMWATCH {} held at the mark and freed since: {:.1f} MB in {}", kind,
                                            total / MB, count));
                for (std::size_t i = 0; i < std::min(rows / 2, freed->size()); ++i)
                    lines.push_back(std::format("MEMWATCH freed {} {:.2f} MB in {}: {}", kind,
                                                (*freed)[i].second.bytes / MB, (*freed)[i].second.count,
                                                FormatCaller((*freed)[i].first)));
            }
        }
        return lines;
    }

    std::vector<std::string> ReportPeaks(std::size_t rows)
    {
        std::vector<std::pair<Caller, Usage>, OwnAllocator<std::pair<Caller, Usage>>> copied;
        std::int64_t live = 0, peak = 0;
        Lock();
        try
        {
            if (usage)
                copied.assign(usage->begin(), usage->end());
        }
        catch (...)
        {
        }
        live = liveTotal;
        peak = peakTotal;
        Unlock();
        std::ranges::sort(copied, std::greater{}, [](const auto& each) { return each.second.peak; });
        std::vector<std::string> lines{std::format(
            "MEMWATCH heap blocks of {} bytes and up: at most {:.1f} MB held at once since the mark, {:.1f} MB now",
            minimum.load(), peak / MB, live / MB)};
        for (std::size_t i = 0; i < std::min(rows, copied.size()); ++i)
            lines.push_back(std::format("MEMWATCH peak {:.2f} MB, now {:.2f} MB in {}: {}", copied[i].second.peak / MB,
                                        copied[i].second.live / MB, copied[i].second.blocks,
                                        FormatCaller(copied[i].first)));
        return lines;
    }

    std::vector<std::string> DescribeAddressSpace(std::size_t rows)
    {
        if (modules.empty())
            ReadModules();

        // the allocations: the regions of one allocation base
        struct Allocation
        {
            std::uintptr_t base = 0;
            std::uint64_t reserved = 0, committed = 0;
            DWORD type = 0, protect = 0;
        };
        std::vector<Allocation> allocations;
        std::vector<std::uint64_t> pieces;
        MEMORY_BASIC_INFORMATION info{};
        for (std::uintptr_t address = 0x10000; VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info));)
        {
            if (info.State == MEM_FREE)
                pieces.push_back(info.RegionSize);
            else
            {
                const auto base = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
                if (allocations.empty() || allocations.back().base != base)
                    allocations.push_back({base, 0, 0, info.Type, info.AllocationProtect});
                allocations.back().reserved += info.RegionSize;
                if (info.State == MEM_COMMIT)
                    allocations.back().committed += info.RegionSize;
            }
            const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (next <= address)
                break;
            address = next;
        }

        // the heaps' regions and large blocks, by allocation base
        std::map<std::uintptr_t, HANDLE> heapOf;
        std::vector<HeapUse> heaps;
        {
            std::vector<HANDLE> handles(GetProcessHeaps(0, nullptr) + 16);
            handles.resize(GetProcessHeaps(static_cast<DWORD>(handles.size()), handles.data()));
            for (const auto heap : handles)
            {
                if (heap == ownHeap)
                    continue;
                // (reserved first: nothing allocates while the walk holds the heap, which may be the one walked)
                std::vector<std::uintptr_t> bases;
                bases.reserve(16384);
                heaps.push_back(Walk(heap, [&](const PROCESS_HEAP_ENTRY& entry) {
                    if (((entry.wFlags & PROCESS_HEAP_REGION) ||
                         ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) && entry.cbData >= LARGE_BLOCK)) &&
                        bases.size() < bases.capacity())
                        bases.push_back(reinterpret_cast<std::uintptr_t>(entry.lpData));
                }));
                // (VirtualQuery outside the heap's lock)
                for (const auto at : bases)
                {
                    MEMORY_BASIC_INFORMATION region{};
                    if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)))
                        heapOf[reinterpret_cast<std::uintptr_t>(region.AllocationBase)] = heap;
                }
            }
        }

        // the reservations the watch saw made (copied into its own heap under its lock, as in Report)
        std::vector<std::pair<std::uintptr_t, Caller>, OwnAllocator<std::pair<std::uintptr_t, Caller>>> copied;
        Lock();
        try
        {
            if (reservations)
                for (const auto& [address, reservation] : *reservations)
                    copied.emplace_back(address, reservation.caller);
        }
        catch (...)
        {
        }
        Unlock();
        const std::map<std::uintptr_t, Caller> watched(copied.begin(), copied.end());

        struct Sum
        {
            std::uint64_t reserved = 0, committed = 0, count = 0;
        };
        std::map<std::string, Sum> images, mapped, byCaller;
        std::map<HANDLE, Sum> heapRegions;
        Sum zoneMemory, other, ownTotal;
        // (the stacks' positions are offsets into it, not addresses: it's the one private block of 256 MB and more, 550 MB
        // here, taken as the game starts)
        std::uintptr_t physicalBase = 0;
        for (const auto& allocation : allocations)
            if (allocation.type == MEM_PRIVATE && allocation.committed >= (256ull << 20) &&
                !heapOf.contains(allocation.base))
                physicalBase = allocation.base;
        std::vector<Allocation> others;
        for (const auto& allocation : allocations)
        {
            const auto add = [&](Sum& sum) {
                sum.reserved += allocation.reserved;
                sum.committed += allocation.committed;
                ++sum.count;
            };
            if (allocation.type == MEM_IMAGE || allocation.type == MEM_MAPPED)
            {
                char path[MAX_PATH]{};
                std::string name = "(no file)";
                if (GetMappedFileNameA(GetCurrentProcess(), reinterpret_cast<void*>(allocation.base), path, MAX_PATH))
                {
                    name = path;
                    name = name.substr(name.find_last_of('\\') + 1);
                }
                add((allocation.type == MEM_IMAGE ? images : mapped)[name]);
            }
            else if (const auto heap = heapOf.find(allocation.base); heap != heapOf.end())
                add(heapRegions[heap->second]);
            else if (const auto caller = watched.find(allocation.base); caller != watched.end())
                add(byCaller[FormatCaller(caller->second)]);
            else if (allocation.base == physicalBase)
                add(zoneMemory);
            else
            {
                add(other);
                others.push_back(allocation);
            }
        }

        std::vector<std::string> lines;
        const auto describe = [&](const char* what, const std::map<std::string, Sum>& sums, bool committedOnly) {
            Sum total;
            std::vector<std::pair<std::string, Sum>> sorted(sums.begin(), sums.end());
            for (const auto& [name, sum] : sorted)
            {
                total.reserved += sum.reserved;
                total.committed += sum.committed;
                total.count += sum.count;
            }
            std::ranges::sort(sorted, std::greater{}, [](const auto& each) { return each.second.reserved; });
            std::string text;
            for (std::size_t i = 0; i < std::min(rows, sorted.size()); ++i)
                text += committedOnly ? std::format(" {} {:.1f},", sorted[i].first, sorted[i].second.committed / MB)
                                      : std::format(" {} {:.1f}/{:.1f},", sorted[i].first,
                                                    sorted[i].second.committed / MB, sorted[i].second.reserved / MB);
            lines.push_back(std::format("MEMMAP {} {:.1f} MB committed of {:.1f} reserved, in {}:{}", what,
                                        total.committed / MB, total.reserved / MB, total.count,
                                        text.empty() ? "" : text.substr(0, text.size() - 1)));
        };
        describe("images", images, true);
        describe("mapped", mapped, false);
        for (const auto& use : heaps)
        {
            const auto regions = heapRegions[use.heap];
            lines.push_back(std::format(
                "MEMMAP heap {:X}{}: {:.1f} MB committed of {:.1f} reserved in {} allocations; in use {:.1f} MB in {} "
                "blocks ({:.1f} MB in blocks of 508 KB and up), free inside {:.1f} MB, uncommitted {:.1f} MB",
                reinterpret_cast<std::uintptr_t>(use.heap), use.heap == GetProcessHeap() ? " (the process's)" : "",
                regions.committed / MB, regions.reserved / MB, regions.count, use.busy / MB, use.blocks,
                use.large / MB, use.free / MB, use.uncommitted / MB));
        }
        lines.push_back(std::format("MEMMAP the game's physical memory (map zones, sound banks) {:.1f} MB committed of "
                                    "{:.1f} reserved, {:.1f} MB free between its stacks",
                                    zoneMemory.committed / MB, zoneMemory.reserved / MB,
                                    (GetPhysicalMemoryPosition(1) - GetPhysicalMemoryPosition(0)) / MB));
        describe("reserved while watched, by caller", byCaller, false);
        lines.push_back(std::format("MEMMAP other private {:.1f} MB committed of {:.1f} reserved, in {}",
                                    other.committed / MB, other.reserved / MB, other.count));
        std::ranges::sort(others, std::greater{}, &Allocation::reserved);
        for (std::size_t i = 0; i < std::min(rows, others.size()); ++i)
            lines.push_back(std::format("MEMMAP other {:08X} {:.2f} MB committed of {:.2f} protect {:X}",
                                        others[i].base, others[i].committed / MB, others[i].reserved / MB,
                                        others[i].protect));
        std::ranges::sort(pieces, std::greater{});
        std::uint64_t free = 0, underOne = 0;
        for (const auto piece : pieces)
        {
            free += piece;
            if (piece < 1048576)
                underOne += piece;
        }
        std::string largest;
        for (std::size_t i = 0; i < std::min<std::size_t>(8, pieces.size()); ++i)
            largest += std::format(" {:.1f}", pieces[i] / MB);
        lines.push_back(std::format("MEMMAP free {:.1f} MB in {} pieces, {:.1f} MB of them under 1 MB; largest MB:{}",
                                    free / MB, pieces.size(), underOne / MB, largest));
        return lines;
    }
}  // namespace IWXMVM::T5::MemoryWatch
