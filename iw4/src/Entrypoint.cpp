#include "StdInclude.hpp"
#include "Mod.hpp"

#include "IW4Interface.hpp"

using namespace IWXMVM;
using namespace IW4;

IW4Interface gameInterface = IW4Interface();

namespace
{
    // The mod is in the game from its first moment (iw4m.exe loads ddraw.dll, the loader, which loads this), long
    // before there is anything to hook: it waits for the game's Direct3D device, then a little for the menus.
    DWORD WINAPI Start(LPVOID)
    {
        const auto device = reinterpret_cast<IDirect3DDevice9* volatile*>(Addresses::d3d9Device);
        while (*device == nullptr)
            ::Sleep(250);

        ::Sleep(4000);

        Mod::Initialize(&gameInterface);
        return 0;
    }
}  // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, [[maybe_unused]] LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        ::DisableThreadLibraryCalls(hinstDLL);
        if (const auto thread = ::CreateThread(nullptr, 0, Start, nullptr, 0, nullptr))
            ::CloseHandle(thread);
    }
    return TRUE;
}
