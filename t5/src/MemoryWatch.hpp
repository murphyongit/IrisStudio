#pragma once

namespace IWXMVM::T5::MemoryWatch
{
    // Harness-only (built with /p:T5Harness=true): who holds the game's 32-bit address space and its heap.
    // While on, each heap block of at least a size and each reservation of address space is noted with the
    // code that asked for it (the return addresses found on the stack, past the heap's and the runtime's own), until
    // it is freed. Hooks ntdll's RtlAllocateHeap, RtlReAllocateHeap, RtlFreeHeap, NtAllocateVirtualMemory and
    // NtFreeVirtualMemory; what was taken before it went on isn't known.
    bool Start(std::size_t minimumBytes, std::string* error);
    void Stop();
    bool IsOn();
    // remembers the moment: a report since it lists what was taken after it and is still held, and what was held
    // before it and has been freed since
    void Mark();
    // what is held, by the code that took it, the most first
    std::vector<std::string> Report(bool sinceMark, std::size_t rows);
    // the heap blocks each caller held at most at once since the mark (or the start), the most first: the heap keeps
    // the address space of its peaks
    std::vector<std::string> ReportPeaks(std::size_t rows);

    // The address space by owner: the modules, mapped files, each heap (its regions and large blocks, what is in
    // use in it and what is free inside it), reservations by the code that made them while the watch was on, the
    // largest of the rest, and the free space in pieces
    std::vector<std::string> DescribeAddressSpace(std::size_t rows);

    struct HeapUse
    {
        HANDLE heap = nullptr;
        bool walked = false;
        std::uint64_t busy = 0;         // in use
        std::uint64_t free = 0;         // committed, not in use (the heap's own caching and holes)
        std::uint64_t uncommitted = 0;  // reserved by the heap, not committed
        std::uint64_t large = 0;        // of busy, in blocks of their own (512 KB and up, given back when freed)
        std::uint32_t blocks = 0;       // busy entries
    };
    // walks one heap (while the walk holds its lock, other threads wait to allocate)
    HeapUse WalkHeap(HANDLE heap);
}  // namespace IWXMVM::T5::MemoryWatch
