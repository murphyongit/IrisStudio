#pragma once
#include "StdInclude.hpp"
#include "Signatures.hpp"

namespace IWXMVM::T5
{
    using T5Addresses = Signatures::T5Addresses;

    inline const T5Addresses& GetGameAddresses()
    {
        static T5Addresses addresses;
        return addresses;
    }
}  // namespace IWXMVM::T5
