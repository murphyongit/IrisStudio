#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace IWXMVM::Components::ThreadStopLogic
{
    // Whether a stopped thread is caught where a change to the game's code or data would catch it halfway,
    // for T5's pool growth: where it is, or a return address into the code on its stack.

    inline constexpr std::size_t BYTES_BEFORE = 7;  // what FollowsCall looks at

    // Whether code ending just before an address is a call, as a return address's is. A word on a stack that doesn't
    // follow a call is no frame: it's data, or the address of a call that something kept. `before` holds the
    // BYTES_BEFORE bytes before the address.
    bool FollowsCall(std::span<const std::uint8_t, BYTES_BEFORE> before);

    struct Match
    {
        std::uint32_t address = 0;
        std::int32_t depth = -1;  // the word of the stack, -1 for the instruction pointer
    };

    // The instruction pointer if it's in the code, else the first word of the whole stack that's a return address into
    // it. Not only near the stack pointer: a zone's load runs deep, with its first frame far above the last. Runs with
    // the game's threads stopped, so nothing here may allocate: hence plain functions.
    using AddressTest = bool (*)(std::uint32_t address);
    std::optional<Match> FindInCode(std::uint32_t eip, std::span<const std::uint32_t> stack, AddressTest inCode,
                                    AddressTest followsCall);
}  // namespace IWXMVM::Components::ThreadStopLogic
