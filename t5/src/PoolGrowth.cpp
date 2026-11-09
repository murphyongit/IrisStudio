#include "StdInclude.hpp"
#include "PoolGrowth.hpp"

#include <TlHelp32.h>

#include "Addresses.hpp"
#include "PoolGrowthTables.hpp"
#include "Components/ThreadStopLogic.hpp"

namespace IWXMVM::T5::PoolGrowth
{
    // A pool is a free list over a static array of entries (DB_AllocXAssetHeader pops, the free handler pushes), and
    // nothing walks the array but the renderer's streaming code, which finds an image or model by its place in it
    // (two functions a pool turn an entry into its index and back) and keeps arrays sized for the pool in .data. So a
    // pool grows by a second array of entries linked into its free list after the free ones, its streaming arrays move
    // to copies twice as big (the tables, from tools/t5-maps/poolpatch.py: every reference to them in the streaming
    // functions and the sizes that go with them), and the two functions jump to ones that know both arrays.
    //
    // Those functions are machine code in a block of memory of their own, never freed with the copies and the new
    // entries, as the game uses them after the mod has gone (Exit, an unload): the block starts with what an injection
    // after it reads to find the pools grown.
    //
    // An entry outside both arrays (the mod's copies of models, drawn as DObjs: magazines, spine rigs, heads) got an
    // index far outside the streaming arrays from the game's functions, which read and wrote whatever lay there in
    // .data; with the arrays moved it crashed (open item 1). Such an entry gets the
    // pool's last index, which no entry takes (it is left out of the free list) and whose entry is zeros: the
    // streaming code then finds no surfaces or texture to stream. Each is noted with where it came from.
    //
    // The scene's lighting records move too (R_AddAllSceneEntSurfacesCamera, 0x6D15A0): one of 8 bytes for each DObj,
    // model and dynamic model of a frame, in a static array of 1024 that the first two lists are flushed to fit, but
    // nothing checks the dynamic models, which multiplayer's maps never have that many of and cuba has 1948 of. Past
    // the array lie the console commands: the frame read a record out of r_smc_flush's function (primary light 32,
    // reflection probe 7 of the level's 3) and crashed at 0x7420E7.
    //
    // The zones grow here too. The game keeps two tables of them in .data, with room for
    // 32: a film of 15 campaign guns with their sounds' banks filled them. Both move to copies with room for what an
    // asset's entry can name (it holds its zone's index in a byte), and the code that points into them is changed, every
    // reference of the game's (the tables, the same way as the pools'). The check of "Max zone count exceeded" is jumped
    // past, as its byte can't hold the new count; ContentPacks keeps to GetMaxZones. The game's physical memory keeps its
    // allocations by a pointer to their name, for a zone its entry in the names' table, and frees by comparing pointers:
    // the zones already loaded get the new table's (else the next unload of one ended the game with "free does not match
    // allocation", the error box of "Disconnected from server").
    //
    // And the character zones' memory. OpenAssetTools lays a zone out from offset 0 of each block, so a pointer into an
    // earlier part of the zone is right only if the game's alignments fall where the tool's did, which takes a block
    // aligned as the largest alignment of the loader (2048; the game's own zone memory is 4096). A character zone's blocks
    // come from malloc (allocation flag 0x40000000), which aligns to 8: the blocks that happened to start at 8 past 16
    // drifted 8 bytes at the zone's first 16-aligned array, and every pointer of the zone to anything after that read 8
    // bytes on, as a name's letters. Which zones depended on the heap: the Walther P38's material got ",mc_l_sm..." read
    // as its technique set and stopped the game at 0x72A776. So those blocks are aligned here too, the pointer malloc
    // gave kept before them for free: machine code in the same block, as the game frees them after the mod has gone.
    //
    // When. A thread caught in one of the functions that use what moves would go on with an old address in a register,
    // so this waits until no thread is. That takes as long as a zone's load: the function that loads one holds its entry
    // of the zones' table for all of it, and at the menu, where Iris Studio injects, zones load for about 15 s more. So
    // the mod's hooks go in first, this runs on a thread of its own and tries until it can, and a level's load waits for
    // it (WaitUntilGrown), so that it's still done before the first map.

    using namespace Tables;
    namespace ThreadStopLogic = Components::ThreadStopLogic;

    inline constexpr std::array<char, 16> MAGIC{"T5iris pools 1"};
    inline constexpr std::size_t STRAY_SLOTS = 64;
    inline constexpr std::size_t BLOCK_SIZE = 0x4000;
    inline constexpr std::size_t CODE_OFFSET = 0x1000;  // the image pool's index function starts there
    inline constexpr std::size_t CODE_SIZE = 0x400;     // of a pool's two functions
    inline constexpr std::size_t ZONE_CODE_OFFSET = CODE_OFFSET + 2 * CODE_SIZE;  // the two for the zones' blocks
    inline constexpr std::size_t ZONE_CODE_SIZE = 0x80;

    struct StraySlot
    {
        std::uint32_t entry;
        std::uint32_t caller;
        std::uint32_t times;
        std::uint32_t unused;
    };
    static_assert(sizeof(StraySlot) == 16);

    // what the block holds of a pool (read by the code: keep the layout)
    struct PoolBlock
    {
        std::uint32_t type;
        std::uint32_t entries;  // the game's array
        std::uint32_t entrySize;
        std::uint32_t count;      // the game's entries
        std::uint32_t extension;  // the new entries
        std::uint32_t extra;      // of them, the last reserved
        std::uint32_t strays;     // pointers outside both that reached the streaming code
        std::uint32_t unused;
        StraySlot straySlots[STRAY_SLOTS];  // by their address
    };

    struct Block
    {
        std::array<char, 16> magic;
        PoolBlock pools[2];
        // the zones' tables, moved (none in a block of a build before they were)
        std::uint32_t zoneInfo;
        std::uint32_t zoneMemory;
    };
    static_assert(sizeof(Block) <= CODE_OFFSET);

    struct Pool
    {
        std::int32_t type;
        const char* name;
        std::uint32_t freeList, entries, entrySize, count;
        std::uint32_t indexOf, atIndex;
        std::span<const std::uint8_t> indexOfBytes, atIndexBytes;
        std::span<const Array> arrays;
        std::span<const Write> writes;
    };
    const std::array<Pool, 2> POOLS{{
        {8, "image", ImagePool, ImageEntries, ImageEntrySize, ImageCount, ImageIndexOf, ImageAtIndex,
         ImageIndexOfBytes, ImageAtIndexBytes, ImageArrays, ImageWrites},
        {5, "model", ModelPool, ModelEntries, ModelEntrySize, ModelCount, ModelIndexOf, ModelAtIndex,
         ModelIndexOfBytes, ModelAtIndexBytes, ModelArrays, ModelWrites},
    }};
    inline constexpr std::uint32_t GAME_CODE_START = 0x401000, GAME_CODE_END = 0x9D0000;
    // how often a thread is looked for that is in the way; how long before the first line in the log says so
    inline constexpr DWORD STOP_RETRY_MS = 50;
    inline constexpr std::uint64_t FIRST_WAITING_NOTE_MS = 30000;

    inline constexpr std::uint32_t LIGHTING_RECORDS = 0x53A7C18;
    // a dynamic model's id is 16 bits, and a list starts after at most the 1024 records the others take
    inline constexpr std::size_t LIGHTING_RECORDS_SIZE = (0x10000 + 0x400) * 8;
    inline constexpr std::array<std::uint32_t, 5> LIGHTING_RECORD_SITES{0x6D15E8, 0x6D1670, 0x6D168C, 0x6D1709,
                                                                        0x6D1751};
    // A dynamic model the lighting cache had no room for is marked not drawn in the visible list by its place in the
    // list where its id goes (mov byte ptr [ebx+eax], 0 with ebx the place, esi the id; the other lists have just the
    // place). It was then drawn with a record nothing wrote. The byte is the instruction's SIB. (ModelLighting takes the
    // function over while the mod runs, for what the game can't light; this stays when it's gone.)
    inline constexpr std::uint32_t DYNAMIC_MODEL_UNLIT_SITE = 0x6D1322;
    inline constexpr std::uint8_t DYNAMIC_MODEL_UNLIT_OLD = 0x03, DYNAMIC_MODEL_UNLIT_NEW = 0x06;

    // DB_AllocXBlocks' call of malloc for the zones of the flag 0x40000000 and the free of them (0x685730)
    inline constexpr std::uint32_t ZONE_BLOCK_ALLOC_CALL = 0x61843C, ZONE_BLOCK_MALLOC = 0x436C50;
    inline constexpr std::uint32_t ZONE_BLOCK_FREE_CALL = 0x68574F, ZONE_BLOCK_FREE = 0x520CF0;
    inline constexpr std::uint32_t ZONE_BLOCK_ALIGNMENT = 0x1000;
    inline constexpr std::uint32_t ZONE_MEMORY_COUNT = 0x24709A4;  // the zones the game has loaded
    // the game's physical memory: its two stacks, each an allocation's name and count, its position, and then 32
    // allocations of a name and a position (PMem_BeginAlloc 0x651290, PMem_Free 0x543500)
    inline constexpr std::uint32_t PHYSICAL_MEMORY = 0x397D688, PHYSICAL_MEMORY_SITE = 0x543535;
    struct PhysicalMemory
    {
        std::uint32_t name;
        std::uint32_t count;
        std::uint32_t position;
        struct
        {
            std::uint32_t name;
            std::uint32_t position;
        } allocations[32];
    };
    static_assert(sizeof(PhysicalMemory) == 0x10C);
    // where threads may be in the middle of those
    inline constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 2> ZONE_BLOCK_FUNCTIONS{
        {{0x6183D0, 0x618530}, {0x685730, 0x685770}}};
    // of the zones the tables have room for, those kept for the game's own: its loads aren't checked any more
    inline constexpr std::uint32_t ZONES_KEPT_FOR_THE_GAME = 14;

    std::atomic<Block*> block = nullptr;
    std::atomic<bool> willGrow = false;  // the code is as the tables expect, or a harness asked
    std::atomic<bool> waiting = false;   // for the thread that grows them
    HANDLE worker = nullptr;
    std::string outcome = "not tried";
    std::mutex stateMutex;  // for the outcome: never held while the growth waits
#ifdef IWXMVM_T5_HARNESS
    int growthDelay = 0;  // milliseconds
#endif

    // x86 code, with its labels: the two functions of a pool
    struct Code
    {
        std::vector<std::uint8_t> bytes;

        void Add(std::initializer_list<std::uint8_t> values)
        {
            bytes.insert(bytes.end(), values);
        }
        void Add32(std::uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
                bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        }
        // jae to a label set later; returns where its offset goes
        std::size_t JumpIfAboveOrEqual()
        {
            Add({0x73, 0x00});
            return bytes.size() - 1;
        }
        // je to a label set later; returns where its offset goes
        std::size_t JumpIfZero()
        {
            Add({0x74, 0x00});
            return bytes.size() - 1;
        }
        void Land(std::size_t jump)
        {
            bytes[jump] = static_cast<std::uint8_t>(bytes.size() - (jump + 1));
        }
    };

    // the entry's index: its place in the game's array, else the count and its place in the new entries, else the
    // reserved index and a note of it (eax, ecx and edx change, as with the game's)
    Code MakeIndexOf(const PoolBlock& pool, const StraySlot* slots, const std::uint32_t* strays)
    {
        Code code;
        code.Add({0x8B, 0x44, 0x24, 0x04});  // mov eax, [esp+4]
        code.Add({0x2D});                    // sub eax, entries
        code.Add32(pool.entries);
        code.Add({0x3D});  // cmp eax, count * size
        code.Add32(pool.count * pool.entrySize);
        const auto toExtension = code.JumpIfAboveOrEqual();
        code.Add({0x31, 0xD2, 0xB9});  // xor edx, edx; mov ecx, size
        code.Add32(pool.entrySize);
        code.Add({0xF7, 0xF1, 0xC3});  // div ecx; ret
        code.Land(toExtension);
        code.Add({0x8B, 0x44, 0x24, 0x04});  // mov eax, [esp+4]
        code.Add({0x2D});                    // sub eax, extension
        code.Add32(pool.extension);
        code.Add({0x3D});  // cmp eax, extra * size
        code.Add32(pool.extra * pool.entrySize);
        const auto toStray = code.JumpIfAboveOrEqual();
        code.Add({0x31, 0xD2, 0xB9});  // xor edx, edx; mov ecx, size
        code.Add32(pool.entrySize);
        code.Add({0xF7, 0xF1, 0x05});  // div ecx; add eax, count
        code.Add32(pool.count);
        code.Add({0xC3});
        code.Land(toStray);
        code.Add({0x8B, 0x44, 0x24, 0x04});  // mov eax, [esp+4]
        code.Add({0x89, 0xC1});              // mov ecx, eax
        code.Add({0xC1, 0xE9, 0x04});        // shr ecx, 4
        code.Add({0x83, 0xE1, static_cast<std::uint8_t>(STRAY_SLOTS - 1)});  // and ecx, slots - 1
        code.Add({0xC1, 0xE1, 0x04});                                        // shl ecx, 4
        code.Add({0x81, 0xC1});                                              // add ecx, slots
        code.Add32(reinterpret_cast<std::uint32_t>(slots));
        code.Add({0x89, 0x01});              // mov [ecx], eax
        code.Add({0x8B, 0x14, 0x24});        // mov edx, [esp]
        code.Add({0x89, 0x51, 0x04});        // mov [ecx+4], edx
        code.Add({0xFF, 0x41, 0x08});        // inc dword ptr [ecx+8]
        code.Add({0xFF, 0x05});              // inc dword ptr [strays]
        code.Add32(reinterpret_cast<std::uint32_t>(strays));
        code.Add({0xB8});  // mov eax, reserved
        code.Add32(pool.count + pool.extra - 1);
        code.Add({0xC3});
        return code;
    }

    // the entry at an index (only eax changes, as with the game's)
    Code MakeAtIndex(const PoolBlock& pool)
    {
        Code code;
        code.Add({0x8B, 0x44, 0x24, 0x04});  // mov eax, [esp+4]
        code.Add({0x3D});                    // cmp eax, count
        code.Add32(pool.count);
        const auto toExtension = code.JumpIfAboveOrEqual();
        code.Add({0x69, 0xC0});  // imul eax, eax, size
        code.Add32(pool.entrySize);
        code.Add({0x05});  // add eax, entries
        code.Add32(pool.entries);
        code.Add({0xC3});
        code.Land(toExtension);
        code.Add({0x2D});  // sub eax, count
        code.Add32(pool.count);
        code.Add({0x69, 0xC0});  // imul eax, eax, size
        code.Add32(pool.entrySize);
        code.Add({0x05});  // add eax, extension
        code.Add32(pool.extension);
        code.Add({0xC3});
        return code;
    }

    // malloc(size + alignment + 4), the block aligned after the pointer it gave, that pointer right before the block
    Code MakeZoneBlockAlloc()
    {
        Code code;
        code.Add({0x8B, 0x44, 0x24, 0x04});  // mov eax, [esp+4]
        code.Add({0x05});                    // add eax, alignment + 4
        code.Add32(ZONE_BLOCK_ALIGNMENT + 4);
        code.Add({0x50, 0xB8});  // push eax; mov eax, malloc
        code.Add32(ZONE_BLOCK_MALLOC);
        code.Add({0xFF, 0xD0, 0x83, 0xC4, 0x04});  // call eax; add esp, 4
        code.Add({0x85, 0xC0});                    // test eax, eax
        const auto failed = code.JumpIfZero();
        code.Add({0x8D, 0x88});  // lea ecx, [eax + alignment + 3]
        code.Add32(ZONE_BLOCK_ALIGNMENT + 3);
        code.Add({0x81, 0xE1});  // and ecx, -alignment
        code.Add32(~(ZONE_BLOCK_ALIGNMENT - 1));
        code.Add({0x89, 0x41, 0xFC});  // mov [ecx-4], eax
        code.Add({0x89, 0xC8});        // mov eax, ecx
        code.Land(failed);
        code.Add({0xC3});
        return code;
    }

    // free of what is kept before the block (eax, ecx and edx change, as with the game's)
    Code MakeZoneBlockFree()
    {
        Code code;
        code.Add({0x8B, 0x44, 0x24, 0x04});  // mov eax, [esp+4]
        code.Add({0x85, 0xC0});              // test eax, eax
        const auto none = code.JumpIfZero();
        code.Add({0xFF, 0x70, 0xFC, 0xB8});  // push dword ptr [eax-4]; mov eax, free
        code.Add32(ZONE_BLOCK_FREE);
        code.Add({0xFF, 0xD0, 0x83, 0xC4, 0x04});  // call eax; add esp, 4
        code.Land(none);
        code.Add({0xC3});
        return code;
    }

    struct PlannedWrite
    {
        std::uint32_t site;
        std::uint32_t value;
        std::uint32_t size;
    };

    // the block of an earlier injection, if the pools were grown by one
    Block* FindGrownBlock()
    {
        const auto code = reinterpret_cast<const std::uint8_t*>(ImageIndexOf);
        if (code[0] != 0xE9)
            return nullptr;
        const auto target = ImageIndexOf + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
        const auto candidate = reinterpret_cast<Block*>(target - CODE_OFFSET);
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(candidate, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return nullptr;
        return candidate->magic == MAGIC ? candidate : nullptr;
    }

    // A thread that stood in the way, for the log. Filled in while threads are stopped, so nothing here allocates.
    struct Obstacle
    {
        DWORD thread;
        std::uint32_t threadStart;
        std::uint32_t eip, esp;
        std::uint32_t address;  // what fell in the code: Eip or a return address
        std::int32_t depth;     // which word above the stack pointer, -1 for Eip
        std::uint32_t times, firstAttempt, lastAttempt;
        std::array<std::uint32_t, 12> frames;  // the addresses of the game's code on its stack, from the top
    };
    struct StopReport
    {
        std::array<Obstacle, 32> obstacles;
        std::size_t count;
        std::uint32_t overflow;
        std::uint32_t attempts;
        std::uint32_t threads;
    };
    StopReport stopReport{};

    using QueryThread = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    QueryThread queryThread = nullptr;

    void NoteObstacle(const Obstacle& found, std::uint32_t attempt)
    {
        auto& report = stopReport;
        for (std::size_t i = 0; i < report.count; ++i)
        {
            auto& known = report.obstacles[i];
            if (known.thread == found.thread && known.address == found.address && known.depth == found.depth)
            {
                ++known.times;
                known.lastAttempt = attempt;
                return;
            }
        }
        if (report.count == report.obstacles.size())
        {
            ++report.overflow;
            return;
        }
        auto& added = report.obstacles[report.count++];
        added = found;
        added.times = 1;
        added.firstAttempt = added.lastAttempt = attempt;
    }

    // Where a thread would be caught halfway: the functions the change touches, and only those. The streaming code's
    // functions lie around the render thread's main loop.
    bool IsInCode(std::uint32_t address)
    {
        if (address < GAME_CODE_START || address >= GAME_CODE_END)
            return false;
        const auto inFunction = [&](const auto& function) {
            return address >= function.first && address < function.second;
        };
        if (std::ranges::any_of(StreamingFunctions, inFunction) || std::ranges::any_of(ZoneFunctions, inFunction) ||
            std::ranges::any_of(ZONE_BLOCK_FUNCTIONS, inFunction))
            return true;
        return std::ranges::any_of(POOLS, [&](const Pool& pool) {
            return (address >= pool.indexOf && address < pool.indexOf + pool.indexOfBytes.size()) ||
                   (address >= pool.atIndex && address < pool.atIndex + pool.atIndexBytes.size());
        });
    }

    bool FollowsCall(std::uint32_t address)
    {
        return ThreadStopLogic::FollowsCall(std::span<const std::uint8_t, ThreadStopLogic::BYTES_BEFORE>(
            reinterpret_cast<const std::uint8_t*>(address) - ThreadStopLogic::BYTES_BEFORE,
            ThreadStopLogic::BYTES_BEFORE));
    }

    // Whether a stopped thread is where a change would catch it halfway: in those functions, or called from them
    bool IsInTheWay(HANDLE thread, DWORD id, std::uint32_t attempt)
    {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread, &context))
            return false;
        std::span<const std::uint32_t> stack;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(context.Esp), &info, sizeof(info)) && info.State == MEM_COMMIT)
        {
            const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            stack = {reinterpret_cast<const std::uint32_t*>(context.Esp), (end - context.Esp) / 4};
        }
        const auto match = ThreadStopLogic::FindInCode(context.Eip, stack, IsInCode, FollowsCall);
        if (!match)
            return false;

        Obstacle found{id};
        found.eip = context.Eip;
        found.esp = context.Esp;
        found.address = match->address;
        found.depth = match->depth;
        if (queryThread)
            queryThread(thread, 9, &found.threadStart, sizeof(found.threadStart), nullptr);  // its start address
        std::size_t frames = 0;
        for (const auto word : stack)
            if (word >= GAME_CODE_START && word < GAME_CODE_END && frames < found.frames.size())
                found.frames[frames++] = word;
        NoteObstacle(found, attempt);
        return true;
    }

    // what stood in the way so far, the most often first; the threads run while this logs
    void LogObstacles(std::string_view when, std::uint64_t milliseconds)
    {
        const auto& report = stopReport;
        LOG_INFO("Pool growth {} after {} ms: attempt {}, {} threads, {} different things in the way", when, milliseconds,
                 report.attempts, report.threads, report.count + report.overflow);
        std::array<const Obstacle*, 32> sorted{};
        for (std::size_t i = 0; i < report.count; ++i)
            sorted[i] = &report.obstacles[i];
        const std::span list(sorted.data(), report.count);
        std::ranges::sort(list, std::greater{}, &Obstacle::times);
        for (const auto found : list.first(std::min<std::size_t>(list.size(), 6)))
        {
            std::string frames;
            for (const auto frame : found->frames)
                if (frame)
                    frames += std::format(" {:X}", frame);
            LOG_INFO("Pool growth waited on thread {} (starts at {:08X}): attempts {} to {}, {} times, eip {:08X} esp "
                     "{:08X}, {:08X} at word {}; stack{}",
                     found->thread, found->threadStart, found->firstAttempt, found->lastAttempt, found->times, found->eip,
                     found->esp, found->address, found->depth, frames);
        }
    }

    std::atomic<bool> stopWaiting = false;

    // Stops the game's other threads where none is in the way (see IsInTheWay), trying again until it can; false when
    // told to stop trying. Nothing may allocate from the heap until they go on (a stopped thread may hold its lock), so
    // the lists have their room already.
    bool StopThreads(std::vector<HANDLE>& stopped, std::vector<DWORD>& ids)
    {
        queryThread = reinterpret_cast<QueryThread>(
            GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
        const auto started = GetTickCount64();
        auto nextNote = FIRST_WAITING_NOTE_MS;
        DWORD lastInTheWay = 0;
        for (std::uint32_t attempt = 0; !stopWaiting; ++attempt)
        {
            ids.clear();
            stopped.clear();
            const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 entry{sizeof(entry)};
            for (auto ok = Thread32First(snapshot, &entry); ok && ids.size() < ids.capacity();
                 ok = Thread32Next(snapshot, &entry))
                if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId())
                    ids.push_back(entry.th32ThreadID);
            CloseHandle(snapshot);
            // the thread in the way last time first: it mostly still is, and the others then aren't stopped for nothing
            if (const auto last = std::ranges::find(ids, lastInTheWay); last != ids.end())
                std::iter_swap(ids.begin(), last);

            bool inTheWay = false;
            for (const auto id : ids)
            {
                const auto thread =
                    OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
                if (!thread)
                    continue;
                if (SuspendThread(thread) == static_cast<DWORD>(-1))
                {
                    CloseHandle(thread);
                    continue;
                }
                stopped.push_back(thread);
                if (IsInTheWay(thread, id, attempt))
                {
                    inTheWay = true;
                    lastInTheWay = id;
                    break;
                }
            }
            stopReport.attempts = attempt + 1;
            stopReport.threads = static_cast<std::uint32_t>(ids.size());
            if (!inTheWay)
                return true;
            for (const auto thread : stopped)
            {
                ResumeThread(thread);
                CloseHandle(thread);
            }
            stopped.clear();
            if (const auto waited = GetTickCount64() - started; waited >= nextNote)
            {
                LogObstacles("still waiting", waited);
                nextNote *= 4;
            }
            Sleep(STOP_RETRY_MS);
        }
        return false;
    }

    // Why the game's code and data aren't as the tables have them, if they aren't
    std::string CheckCode()
    {
        const auto pools = reinterpret_cast<void**>(GetGameAddresses().DB_XAssetPool());
        for (const auto& pool : POOLS)
        {
            if (reinterpret_cast<std::uint32_t>(pools[pool.type]) != pool.freeList)
                return std::format("the {} pool isn't at {:08X}", pool.name, pool.freeList);
            if (std::memcmp(reinterpret_cast<const void*>(pool.indexOf), pool.indexOfBytes.data(),
                            pool.indexOfBytes.size()) != 0 ||
                std::memcmp(reinterpret_cast<const void*>(pool.atIndex), pool.atIndexBytes.data(),
                            pool.atIndexBytes.size()) != 0)
                return std::format("the {} pool's index functions aren't the ones expected", pool.name);
            std::size_t differ = 0;
            std::string first;
            for (const auto& write : pool.writes)
            {
                std::uint32_t now = 0;
                std::memcpy(&now, reinterpret_cast<const void*>(write.site), write.kind == Kind::Byte ? 1 : 4);
                if (now != write.old && !differ++)
                    first = std::format("{:08X} has {:08X}, not {:08X}", write.site, now, write.old);
            }
            if (differ)
                return std::format("{} of the {} pool's {} sites aren't the ones expected (first {})", differ,
                                   pool.name, pool.writes.size(), first);
        }
        for (const auto site : LIGHTING_RECORD_SITES)
            if (*reinterpret_cast<const std::uint32_t*>(site) != LIGHTING_RECORDS)
                return std::format("{:08X} doesn't hold the lighting records", site);
        if (*reinterpret_cast<const std::uint8_t*>(DYNAMIC_MODEL_UNLIT_SITE) != DYNAMIC_MODEL_UNLIT_OLD)
            return std::format("{:08X} isn't the dynamic models' instruction", DYNAMIC_MODEL_UNLIT_SITE);
        for (const auto& write : ZoneWrites)
        {
            std::uint32_t now = 0;
            std::memcpy(&now, reinterpret_cast<const void*>(write.site), write.kind == Kind::Byte ? 1 : 4);
            if (now != write.old)
                return std::format("{:08X} has {:08X}, not {:08X} (zones)", write.site, now, write.old);
        }
        if (*reinterpret_cast<const std::uint32_t*>(PHYSICAL_MEMORY_SITE) != PHYSICAL_MEMORY)
            return std::format("{:08X} doesn't hold the physical memory", PHYSICAL_MEMORY_SITE);
        for (const auto [call, target] : {std::pair(ZONE_BLOCK_ALLOC_CALL, ZONE_BLOCK_MALLOC),
                                          std::pair(ZONE_BLOCK_FREE_CALL, ZONE_BLOCK_FREE)})
            if (*reinterpret_cast<const std::uint8_t*>(call) != 0xE8 ||
                call + 5 + *reinterpret_cast<const std::uint32_t*>(call + 1) != target)
                return std::format("{:08X} doesn't call {:08X}", call, target);
        return {};
    }

    std::string Grow()
    {
        // (again, now that the hooks are in: none may have changed a site)
        if (auto wrong = CheckCode(); !wrong.empty())
            return wrong;
        const auto pools = reinterpret_cast<void**>(GetGameAddresses().DB_XAssetPool());
        std::vector<PlannedWrite> writes;
        writes.reserve(512);

        // everything the change needs, made before the threads stop (nothing allocates then)
        const auto memory = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!memory)
            return "no memory for the index functions";
        auto& grown = *reinterpret_cast<Block*>(memory);
        std::vector<std::vector<std::uint8_t*>> copies(POOLS.size());
        for (std::size_t p = 0; p < POOLS.size(); ++p)
        {
            const auto& pool = POOLS[p];
            for (const auto& array : pool.arrays)
            {
                const auto copy = static_cast<std::uint8_t*>(
                    VirtualAlloc(nullptr, array.size * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                if (!copy)
                    return std::format("no memory for the {} pool's arrays", pool.name);
                copies[p].push_back(copy);
            }
            const auto extension = static_cast<std::uint8_t*>(
                VirtualAlloc(nullptr, pool.count * pool.entrySize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!extension)
                return std::format("no memory for the {} pool's entries", pool.name);

            auto& info = grown.pools[p];
            info = {static_cast<std::uint32_t>(pool.type), pool.entries, pool.entrySize, pool.count,
                    reinterpret_cast<std::uint32_t>(extension), pool.count};
            const auto indexOf = memory + CODE_OFFSET + p * CODE_SIZE;
            const auto atIndex = indexOf + CODE_SIZE / 2;
            const auto indexOfCode = MakeIndexOf(info, info.straySlots, &info.strays).bytes;
            const auto atIndexCode = MakeAtIndex(info).bytes;
            std::ranges::copy(indexOfCode, indexOf);
            std::ranges::copy(atIndexCode, atIndex);

            for (const auto& write : pool.writes)
            {
                if (write.kind == Kind::Address)
                {
                    const auto& array = pool.arrays[write.value];
                    const auto offset = write.old - array.start;
                    writes.push_back({write.site,
                                      reinterpret_cast<std::uint32_t>(copies[p][write.value]) +
                                          (array.block ? offset * 2 : offset),
                                      4});
                }
                else
                    writes.push_back({write.site, write.value, write.kind == Kind::Byte ? 1u : 4u});
            }
            // the game's functions jump to these
            for (const auto [from, to] : {std::pair(pool.indexOf, indexOf), std::pair(pool.atIndex, atIndex)})
            {
                writes.push_back({from, 0xE9, 1});
                writes.push_back({from + 1, reinterpret_cast<std::uint32_t>(to) - (from + 5), 4});
            }
        }

        // the zones' blocks
        const auto blockAlloc = memory + ZONE_CODE_OFFSET;
        const auto blockFree = blockAlloc + ZONE_CODE_SIZE;
        std::ranges::copy(MakeZoneBlockAlloc().bytes, blockAlloc);
        std::ranges::copy(MakeZoneBlockFree().bytes, blockFree);
        for (const auto [call, to] : {std::pair(ZONE_BLOCK_ALLOC_CALL, blockAlloc), std::pair(ZONE_BLOCK_FREE_CALL, blockFree)})
            writes.push_back({call + 1, reinterpret_cast<std::uint32_t>(to) - (call + 5), 4});
        FlushInstructionCache(GetCurrentProcess(), memory, BLOCK_SIZE);

        // the zones' tables
        const std::array<std::uint32_t, 2> zoneSizes{ZoneInfoCount * ZoneInfoEntrySize,
                                                     ZoneMemoryCount * ZoneMemoryEntrySize};
        std::array<std::uint8_t*, 2> zoneCopies{};
        for (std::size_t a = 0; a < zoneCopies.size(); ++a)
        {
            zoneCopies[a] =
                static_cast<std::uint8_t*>(VirtualAlloc(nullptr, zoneSizes[a], MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!zoneCopies[a])
                return "no memory for the zones' tables";
        }
        for (const auto& write : ZoneWrites)
        {
            if (write.kind == Kind::Address)
                writes.push_back({write.site,
                                  reinterpret_cast<std::uint32_t>(zoneCopies[write.value]) +
                                      (write.old - ZoneArrays[write.value].start),
                                  4});
            else if (write.kind == Kind::End)
                writes.push_back({write.site, reinterpret_cast<std::uint32_t>(zoneCopies[write.value]) + zoneSizes[write.value], 4});
            else
                writes.push_back({write.site, write.value, write.kind == Kind::Byte ? 1u : 4u});
        }

        const auto records = VirtualAlloc(nullptr, LIGHTING_RECORDS_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!records)
            return "no memory for the lighting records";
        for (const auto site : LIGHTING_RECORD_SITES)
            writes.push_back({site, reinterpret_cast<std::uint32_t>(records), 4});
        writes.push_back({DYNAMIC_MODEL_UNLIT_SITE, DYNAMIC_MODEL_UNLIT_NEW, 1});

        std::vector<HANDLE> stopped;
        std::vector<DWORD> ids;
        stopped.reserve(1024);
        ids.reserve(1024);
        const auto stopStart = GetTickCount64();
        if (!StopThreads(stopped, ids))
            return "stopped before it could";

        for (std::size_t p = 0; p < POOLS.size(); ++p)
        {
            const auto& pool = POOLS[p];
            for (std::size_t a = 0; a < pool.arrays.size(); ++a)
            {
                const auto& array = pool.arrays[a];
                const auto from = reinterpret_cast<const std::uint8_t*>(array.start);
                if (!array.block)
                {
                    std::memcpy(copies[p][a], from, array.size);
                    continue;
                }
                constexpr std::size_t BLOCK_ARRAY = 0x240;
                for (std::size_t i = 0; i < array.size / BLOCK_ARRAY; ++i)
                    std::memcpy(copies[p][a] + i * BLOCK_ARRAY * 2, from + i * BLOCK_ARRAY, BLOCK_ARRAY);
            }
        }
        for (std::size_t a = 0; a < zoneCopies.size(); ++a)
            std::memcpy(zoneCopies[a], reinterpret_cast<const void*>(ZoneArrays[a].start), ZoneArrays[a].size);
        grown.zoneInfo = reinterpret_cast<std::uint32_t>(zoneCopies[0]);
        grown.zoneMemory = reinterpret_cast<std::uint32_t>(zoneCopies[1]);
        // the zones loaded so far are named by their old entries
        const auto moveName = [&](std::uint32_t& name) {
            if (name >= ZoneArrays[0].start && name < ZoneArrays[0].start + ZoneArrays[0].size)
                name += grown.zoneInfo - ZoneArrays[0].start;
        };
        for (auto& memory : std::span(reinterpret_cast<PhysicalMemory*>(PHYSICAL_MEMORY), 2))
        {
            moveName(memory.name);
            for (std::uint32_t i = 0; i < std::min<std::uint32_t>(memory.count, 32); ++i)
                moveName(memory.allocations[i].name);
        }
        for (const auto& write : writes)
        {
            DWORD protection = 0;
            VirtualProtect(reinterpret_cast<void*>(write.site), write.size, PAGE_EXECUTE_READWRITE, &protection);
            std::memcpy(reinterpret_cast<void*>(write.site), &write.value, write.size);
            VirtualProtect(reinterpret_cast<void*>(write.site), write.size, protection, &protection);
        }
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        // the new entries after the free ones, but the last
        for (std::size_t p = 0; p < POOLS.size(); ++p)
        {
            const auto& info = grown.pools[p];
            auto last = static_cast<std::uint8_t**>(pools[POOLS[p].type]);
            while (*last)
                last = reinterpret_cast<std::uint8_t**>(*last);
            for (std::uint32_t i = 0; i + 1 < info.extra; ++i)
            {
                *last = reinterpret_cast<std::uint8_t*>(info.extension) + i * info.entrySize;
                last = reinterpret_cast<std::uint8_t**>(*last);
            }
            *last = nullptr;
        }
        grown.magic = MAGIC;
        block = &grown;

        for (const auto thread : stopped)
        {
            ResumeThread(thread);
            CloseHandle(thread);
        }
        LogObstacles("done", GetTickCount64() - stopStart);
        return std::format("grown: {} writes, {} threads stopped; images {} -> {}, models {} -> {}, zones 32 -> {}",
                           writes.size(), stopped.size(), ImageCount, ImageCount * 2 - 1, ModelCount, ModelCount * 2 - 1,
                           ZoneMemoryCount);
    }

    // Until the pools are grown, while no zone's load is in the way (see the top of the file)
    DWORD WINAPI GrowThread(LPVOID)
    {
#ifdef IWXMVM_T5_HARNESS
        for (auto left = growthDelay; left > 0 && !stopWaiting; left -= 50)
            Sleep(50);
#endif
        if (stopWaiting)
        {
            waiting = false;
            return 0;
        }
        FILETIME created{}, unused{}, now{};
        GetProcessTimes(GetCurrentProcess(), &created, &unused, &unused, &unused);
        GetSystemTimeAsFileTime(&now);
        const auto seconds = [](const FILETIME& time) {
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32 | time.dwLowDateTime) / 10000000.0;
        };
        LOG_INFO("Pool growth starts {:.1f} s after the game did", seconds(now) - seconds(created));
        const auto started = GetTickCount64();
        auto result = Grow();
        LOG_INFO("Pool growth took {} ms", GetTickCount64() - started);
        {
            std::lock_guard lock(stateMutex);
            outcome = result;
        }
        if (block)
            LOG_INFO("The image and model pools are twice as big ({})", result);
        else if (stopWaiting)
            LOG_INFO("The pools' growth stopped, as the mod unloads");
        else
            LOG_ERROR("The image and model pools didn't grow, so campaign maps don't load: {}", result);
        waiting = false;
        return 0;
    }

    void StartGrowing()
    {
        if (!willGrow || block)
            return;
        Stop();
        stopWaiting = false;
        waiting = true;
        worker = CreateThread(nullptr, 0, GrowThread, nullptr, 0, nullptr);
        if (!worker)
        {
            waiting = false;
            LOG_ERROR("The image and model pools didn't grow, so campaign maps don't load: no thread for it");
        }
    }

    void Stop()
    {
        stopWaiting = true;
        if (!worker)
            return;
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
        worker = nullptr;
    }

    bool WaitUntilGrown(std::chrono::milliseconds limit)
    {
        if (!waiting)
            return IsGrown();
        const auto started = std::chrono::steady_clock::now();
        const auto waited = [&] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        };
        while (waiting && waited() < limit)
            Sleep(10);
        LOG_INFO("A level's load waited {} ms for the pools to grow", waited().count());
        return IsGrown();
    }

    std::string GrowNow()
    {
        if (IsGrown())
            return "grown already";
        willGrow = true;
        StartGrowing();
        WaitUntilGrown(std::chrono::seconds(60));
        std::lock_guard lock(stateMutex);
        return outcome;
    }

    bool Install()
    {
        if (const auto earlier = FindGrownBlock())
        {
            block = earlier;
            std::lock_guard lock(stateMutex);
            outcome = "grown by an earlier injection";
            LOG_INFO("The image and model pools were grown by an earlier injection");
            return true;
        }
#ifdef IWXMVM_T5_HARNESS
        // for comparisons: the harness starts the game without growth (launch.ps1 -NoPoolGrowth), or has it wait
        // (-PoolGrowthDelay), as a level's load waits for it
        char value[16]{};
        if (GetEnvironmentVariableA("IWXMVM_POOL_GROWTH", value, sizeof(value)))
        {
            if (std::string_view(value) == "off")
            {
                std::lock_guard lock(stateMutex);
                outcome = "off (IWXMVM_POOL_GROWTH)";
                LOG_INFO("The image and model pools stay as they are (IWXMVM_POOL_GROWTH=off)");
                return false;
            }
            growthDelay = std::atoi(value);
        }
#endif
        auto wrong = CheckCode();
        if (!wrong.empty())
        {
            LOG_ERROR("The image and model pools couldn't grow, so campaign maps don't load: {}", wrong);
            std::lock_guard lock(stateMutex);
            outcome = std::move(wrong);
            return false;
        }
        willGrow = true;
        // (a level's load waits from here on, in case one starts before the hooks are all in)
        waiting = true;
        std::lock_guard lock(stateMutex);
        outcome = "waiting";
        return true;
    }

    bool WillGrow()
    {
        return willGrow || block;
    }

    bool IsGrown()
    {
        return block != nullptr;
    }

    std::uintptr_t GetZoneInfo()
    {
        const Block* const grown = block;
        return grown && grown->zoneInfo ? grown->zoneInfo : GetGameAddresses().g_zoneInfo();
    }

    std::uint32_t GetPhysicalMemoryFree()
    {
        const auto memory = reinterpret_cast<const PhysicalMemory*>(PHYSICAL_MEMORY);
        return memory[1].position - memory[0].position;
    }

    std::size_t CountUnalignedZoneBlocks()
    {
        struct XBlock
        {
            std::uint32_t data;
            std::uint32_t size;
        };
        struct ZoneMemory
        {
            std::uint16_t index;
            std::uint32_t flags;
            XBlock blocks[7];
        };
        static_assert(sizeof(ZoneMemory) == ZoneMemoryEntrySize);
        const Block* const grown = block;
        if (!grown || !grown->zoneMemory)
            return 0;
        constexpr std::uint32_t CHARACTER_ZONE_MEMORY = 0x40000000;
        const auto zones = reinterpret_cast<const ZoneMemory*>(grown->zoneMemory);
        std::size_t unaligned = 0;
        for (std::int32_t i = 0; i < *reinterpret_cast<const std::int32_t*>(ZONE_MEMORY_COUNT); ++i)
            if (zones[i].flags & CHARACTER_ZONE_MEMORY)
                unaligned += std::ranges::count_if(zones[i].blocks, [](const XBlock& zoneBlock) {
                    return zoneBlock.size && (zoneBlock.data & (ZONE_BLOCK_ALIGNMENT - 1));
                });
        return unaligned;
    }

    std::size_t GetZoneInfoCount()
    {
        const Block* const grown = block;
        return grown && grown->zoneInfo ? ZoneInfoCount : ZoneArrays[0].size / ZoneInfoEntrySize;
    }

    std::size_t GetMaxZones()
    {
        const Block* const grown = block;
        return grown && grown->zoneInfo ? ZoneMemoryCount - ZONES_KEPT_FOR_THE_GAME
                                        : ZoneArrays[1].size / ZoneMemoryEntrySize;
    }

    // a name at an address, when it reads as one
    std::string ReadName(std::uintptr_t address)
    {
        const auto readable = [](std::uintptr_t at, std::size_t size) {
            MEMORY_BASIC_INFORMATION info{};
            if (!at || !VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) ||
                info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                return false;
            return at + size <= reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        };
        if (!readable(address, 4))
            return {};
        const auto name = *reinterpret_cast<const char* const*>(address);
        if (!readable(reinterpret_cast<std::uintptr_t>(name), 64))
            return {};
        const auto length = strnlen(name, 64);
        if (length == 0 || length == 64 ||
            !std::all_of(name, name + length, [](char c) { return c >= 0x20 && c <= 0x7E; }))
            return {};
        return std::string(name, length);
    }

    State GetState()
    {
        std::lock_guard lock(stateMutex);
        const Block* const grown = block;
        State state{grown != nullptr, outcome};
        for (std::size_t p = 0; p < POOLS.size(); ++p)
        {
            auto& pool = state.pools.emplace_back();
            pool.type = POOLS[p].type;
            pool.count = POOLS[p].count;
            if (!grown)
                continue;
            const auto& info = grown->pools[p];
            pool.count = info.count + info.extra - 1;
            pool.strays = info.strays;
            pool.extension = info.extension;
            pool.extensionEnd = info.extension + info.extra * info.entrySize;
            for (const auto& slot : info.straySlots)
            {
                if (!slot.entry)
                    continue;
                // a model's name is its first member, an image's at 0x2C
                const auto name = ReadName(slot.entry + (POOLS[p].type == 8 ? 0x2C : 0));
                pool.strayEntries.push_back({slot.entry, slot.caller, slot.times, name});
            }
        }
        return state;
    }

    void LogNewStrays()
    {
        static std::uint32_t seen = 0;
        static std::set<std::pair<std::int32_t, std::uintptr_t>> logged;
        const Block* const grown = block;
        if (!grown)
            return;
        const auto total = grown->pools[0].strays + grown->pools[1].strays;
        if (total == seen)
            return;
        seen = total;
        for (const auto& pool : GetState().pools)
        {
            for (const auto& stray : pool.strayEntries)
            {
                if (!logged.emplace(pool.type, stray.entry).second)
                    continue;
                LOG_INFO("The streaming code was handed {} {:08X} '{}' from outside the pool (at {:08X}): it gets the "
                         "reserved index",
                         pool.type == 8 ? "an image" : "a model", stray.entry, stray.name, stray.caller);
            }
        }
    }

    std::size_t HoldGameEntries(std::int32_t type, bool hold)
    {
        static std::map<std::int32_t, std::vector<std::uint8_t*>> held;
        const auto pool = std::ranges::find(POOLS, type, &Pool::type);
        if (pool == POOLS.end() || !IsGrown())
            return 0;
        const auto head = static_cast<std::uint8_t**>(reinterpret_cast<void**>(GetGameAddresses().DB_XAssetPool())[type]);
        auto& entries = held[type];
        if (!hold)
        {
            const auto count = entries.size();
            for (const auto entry : entries)
            {
                *reinterpret_cast<std::uint8_t**>(entry) = *head;
                *head = entry;
            }
            entries.clear();
            return count;
        }
        const auto first = reinterpret_cast<std::uint8_t*>(pool->entries);
        const auto end = first + pool->count * pool->entrySize;
        for (auto link = head; *link;)
        {
            const auto entry = *link;
            if (entry >= first && entry < end)
            {
                *link = *reinterpret_cast<std::uint8_t**>(entry);
                entries.push_back(entry);
            }
            else
                link = reinterpret_cast<std::uint8_t**>(entry);
        }
        return entries.size();
    }
}  // namespace IWXMVM::T5::PoolGrowth
