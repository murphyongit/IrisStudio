#include "StdInclude.hpp"
#include "Mod.hpp"

#include "IW5Interface.hpp"

#include <shellapi.h>

using namespace IWXMVM;
using namespace IW5;

IW5Interface gameInterface = IW5Interface();

namespace
{
    // Plutonium bans players who load DLLs into the game while it runs online, so the mod only starts when Plutonium
    // was started in LAN mode, with -lan. IW5iris.exe already refuses online Plutonium; this covers other ways of
    // loading the DLL, though by then the DLL is in the game. (The same guard as T5iris's.)
    bool IsOnlinePlutonium()
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (_wcsicmp(std::filesystem::path(exePath).filename().c_str(), L"plutonium-bootstrapper-win32.exe") != 0)
            return false;

        int argumentCount = 0;
        const auto arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
        if (!arguments)
            return true;

        bool lan = false;
        for (int i = 1; i < argumentCount && !lan; ++i)
            lan = _wcsicmp(arguments[i], L"-lan") == 0;

        LocalFree(arguments);
        return !lan;
    }

    DWORD WINAPI Start(LPVOID module)
    {
        if (IsOnlinePlutonium())
        {
            MessageBoxW(nullptr,
                        L"Plutonium is running online. Plutonium bans players who load DLLs into the game while it "
                        L"runs online, so IW5iris won't start. Close the game and start it with IW5iris.exe, which "
                        L"starts Plutonium in LAN mode.",
                        L"IW5iris", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
            FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
        }

        Mod::Initialize(&gameInterface);
        return 0;
    }
}  // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, [[maybe_unused]] LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        // the command line is checked on the new thread, as DllMain must not load other DLLs
        if (const auto thread = ::CreateThread(nullptr, 0, Start, hinstDLL, 0, nullptr))
            ::CloseHandle(thread);
    }
    return TRUE;
}
