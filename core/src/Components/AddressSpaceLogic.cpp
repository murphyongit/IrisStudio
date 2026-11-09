#include "StdInclude.hpp"
#include "AddressSpaceLogic.hpp"

namespace IWXMVM::Components::AddressSpaceLogic
{
    bool IsShort(const AddressSpace& space, std::uint64_t freeFloor)
    {
        return space.free < freeFloor || space.largest < freeFloor / 2;
    }

    std::string Describe(const AddressSpace& space)
    {
        return std::format("{} MB free, the largest block {} MB", space.free >> 20, space.largest >> 20);
    }
}  // namespace IWXMVM::Components::AddressSpaceLogic
