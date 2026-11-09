#pragma once

#include "Beta/BetaKeys.hpp"

// Whether this is a beta build (/p:IrisBeta=true, or test for the test key pair): the key
// screen until a key works, the beta key's row in Settings in place of the update check, "beta" in the title bar. The
// default build has none of it.
namespace Launcher
{
#ifdef IRIS_BETA
    inline constexpr bool BETA = true;
#else
    inline constexpr bool BETA = false;
#endif

#ifdef IRIS_BETA_TEST_KEY
    inline constexpr bool BETA_TEST_KEY = true;
    inline constexpr const IWXMVM::Beta::PublicKey& BETA_PUBLIC_KEY = IWXMVM::Beta::TEST_PUBLIC_KEY;
#else
    inline constexpr bool BETA_TEST_KEY = false;
    inline constexpr const IWXMVM::Beta::PublicKey& BETA_PUBLIC_KEY = IWXMVM::Beta::PUBLIC_KEY;
#ifdef IRIS_BETA
    static_assert(IWXMVM::Beta::IsSet(IWXMVM::Beta::PUBLIC_KEY),
                  "a beta build needs the user's public key: tools/beta-keys init");
#endif
#endif
}  // namespace Launcher
