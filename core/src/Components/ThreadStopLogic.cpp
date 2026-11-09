#include "StdInclude.hpp"
#include "ThreadStopLogic.hpp"

namespace IWXMVM::Components::ThreadStopLogic
{
    bool FollowsCall(std::span<const std::uint8_t, BYTES_BEFORE> before)
    {
        // E8 rel32
        if (before[BYTES_BEFORE - 5] == 0xE8)
            return true;
        // FF /2, with a register or memory operand of 0, 1, 4 or 5 more bytes
        const auto isCall = [&](std::size_t length) {
            return before[BYTES_BEFORE - length] == 0xFF && (before[BYTES_BEFORE - length + 1] & 0x38) == 0x10;
        };
        return isCall(2) || isCall(3) || isCall(6) || isCall(7);
    }

    std::optional<Match> FindInCode(std::uint32_t eip, std::span<const std::uint32_t> stack, AddressTest inCode,
                                    AddressTest followsCall)
    {
        if (inCode(eip))
            return Match{eip, -1};
        for (std::size_t i = 0; i < stack.size(); ++i)
            if (inCode(stack[i]) && followsCall(stack[i]))
                return Match{stack[i], static_cast<std::int32_t>(i)};
        return std::nullopt;
    }
}  // namespace IWXMVM::Components::ThreadStopLogic
