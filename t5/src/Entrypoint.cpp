#include "StdInclude.hpp"
#include "Mod.hpp"

#include "T5Interface.hpp"

#include <shellapi.h>

#ifdef IRIS_BETA
#include "Beta/BetaKey.hpp"
#include "Beta/BetaKeys.hpp"
#include "Version.hpp"
#endif

using namespace IWXMVM;
using namespace T5;

T5Interface gameInterface = T5Interface();

namespace
{
    // Plutonium bans players who load DLLs into the game while it runs online, so the mod only starts when Plutonium
    // was started in LAN mode, with -lan. The injector already refuses online Plutonium; this covers other ways of
    // loading the DLL, though by then the DLL is in the game.
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

#ifdef IRIS_BETA
#ifdef IRIS_BETA_TEST_KEY
    constexpr const Beta::PublicKey& BETA_PUBLIC_KEY = Beta::TEST_PUBLIC_KEY;
#else
    constexpr const Beta::PublicKey& BETA_PUBLIC_KEY = Beta::PUBLIC_KEY;
    static_assert(Beta::IsSet(Beta::PUBLIC_KEY), "a beta build needs the user's public key: tools/beta-keys init");
#endif

    // The beta's key, checked here as well as in the launcher, since T5iris.dll is a
    // plain file in the launcher's folder and can be loaded without it. False, after saying why, when the mod
    // mustn't start.
    bool CheckBetaKey(HMODULE module)
    {
        wchar_t dllPath[MAX_PATH]{};
        GetModuleFileNameW(module, dllPath, MAX_PATH);
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        const std::filesystem::path localAppData = local ? local : L"";

        const auto text =
            Beta::ReadKeyFile(Beta::KeyFilePlaces(std::filesystem::path(dllPath).parent_path(), localAppData));
        const auto thisPc = Beta::ThisPcCode().value_or("");
        Beta::Key key;
        const auto verdict = Beta::Check(text, thisPc, Beta::Today(), BETA_PUBLIC_KEY, &key);
        if (verdict == Beta::Verdict::Valid)
        {
#ifdef IRIS_BETA_TEST_KEY
            key.testKey = true;
#endif
            Beta::SetActive(key);
            return true;
        }

        const auto message = Beta::ModMessage(verdict, ModName(Types::Game::T5), key, thisPc);
        std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0),
                          L'\0');
        MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), wide.data(),
                            static_cast<int>(wide.size()));
        MessageBoxW(nullptr, wide.c_str(), L"T5iris", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        return false;
    }
#endif

    DWORD WINAPI Start(LPVOID module)
    {
        if (IsOnlinePlutonium())
        {
            MessageBoxW(nullptr,
                        L"Plutonium is running online. Plutonium bans players who load DLLs into the game while it "
                        L"runs online, so T5iris won't start. Close the game and start Plutonium in LAN mode, with "
                        L"-lan.",
                        L"T5iris", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
            FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
        }

#ifdef IRIS_BETA
        // before anything is hooked, so a refused start leaves the game as it was
        if (!CheckBetaKey(static_cast<HMODULE>(module)))
            FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
#endif

        Mod::Initialize(&gameInterface);
        return 0;
    }
}  // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        // the command line is checked on the new thread, as DllMain must not load other DLLs
        ::CreateThread(nullptr, 0, Start, hinstDLL, 0, nullptr);
    }
    return TRUE;
}
