// Injector for T5iris, the Black Ops (T5) build of the mod.
//
// Waits for Call of Duty: Black Ops multiplayer (Steam BlackOpsMP.exe or Plutonium T5 multiplayer) to open its
// window and create its D3D9 device (and, when it starts after the injector, to show its menu), checks that it is the
// supported multiplayer build, and then loads T5iris.dll into it with LoadLibraryW. The work is Injector.hpp's, which
// the launcher (launcher/) uses too.
//
// Usage: injector.exe [--unload] [path\to\T5iris.dll]
// Without a path it uses t5\bin\Win32\<configuration>\T5iris.dll of the solution it was built in, or a T5iris.dll
// next to the injector. --unload takes the DLL out of the game after File > Exit, as Plutonium keeps it loaded (it
// hooks FreeLibraryAndExitThread), so it can be injected again without restarting the game.
//
// Only use this offline. Never join VAC-secured or Plutonium online servers with the mod loaded. Plutonium bans players
// who load DLLs into the game while it runs online, so the injector refuses Plutonium unless it was started in LAN mode.

#include "Injector.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace
{
    std::optional<std::filesystem::path> FindDefaultDll()
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        // the injector is built to tools/bin/Win32/<configuration>
        const auto exeDirectory = std::filesystem::path(exePath).parent_path();
        const auto configuration = exeDirectory.filename();
        const auto solutionDirectory = exeDirectory.parent_path().parent_path().parent_path().parent_path();

        for (const auto& candidate : {solutionDirectory / "t5" / "bin" / "Win32" / configuration / "T5iris.dll",
                                      exeDirectory / "T5iris.dll"})
        {
            if (std::filesystem::is_regular_file(candidate))
                return std::filesystem::canonical(candidate);
        }
        return std::nullopt;
    }

    int Run(int argc, wchar_t** argv)
    {
        const bool unload = argc >= 2 && std::wstring(argv[1]) == L"--unload";
        if (unload)
        {
            --argc;
            ++argv;
        }
        std::filesystem::path dllPath;
        if (argc >= 2)
        {
            dllPath = argv[1];
            if (!std::filesystem::is_regular_file(dllPath))
            {
                std::printf("DLL not found: %ls\n", dllPath.c_str());
                return 1;
            }
            dllPath = std::filesystem::canonical(dllPath);
        }
        else if (auto defaultDll = FindDefaultDll())
        {
            dllPath = *defaultDll;
        }
        else
        {
            std::printf(
                "Could not find T5iris.dll. Build the t5 project first, or pass the DLL path as an argument.\n");
            return 1;
        }

        std::printf("DLL: %ls\n", dllPath.c_str());
        std::printf("Waiting for Call of Duty: Black Ops multiplayer to start...\n");

        // a game started after the injector gets until its menu shows (see Injector::WaitForMenu)
        std::optional<std::chrono::steady_clock::time_point> started;
        HWND window = nullptr;
        while (!(window = Injector::FindGameWindow()))
        {
            started = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        std::string error;
        auto game = Injector::Game::Open(window, error);
        if (!game)
        {
            std::printf("%s\n", error.c_str());
            return 1;
        }
        std::printf("Found the game window in %ls (process %lu)\n", game->ImagePath().c_str(), game->ProcessId());

        Injector::Addresses addresses;
        if (!Injector::CheckBuild(*game, addresses, error))
        {
            std::printf("%s\n", error.c_str());
            return 1;
        }

        std::printf("Waiting for the game to create its D3D9 device...\n");
        if (!Injector::WaitForDevice(*game, addresses, {}, error))
        {
            std::printf("%s\n", error.c_str());
            return 1;
        }

        if (unload)
        {
            std::string message;
            const bool unloaded = Injector::Unload(*game, dllPath, message);
            std::printf("%s\n", message.c_str());
            return unloaded ? 0 : 1;
        }

        if (Injector::IsModuleLoaded(game->ProcessId(), dllPath))
        {
            std::printf("%ls is already loaded in the game\n", dllPath.filename().c_str());
            return 0;
        }

        if (started)
        {
            std::printf("Waiting for the game's menu...\n");
            if (Injector::WaitForMenu(*game, addresses, *started, {}, error) == Injector::MenuWait::Failed)
            {
                std::printf("%s\n", error.c_str());
                return 1;
            }
        }

        if (!Injector::Inject(*game, dllPath, error))
        {
            std::printf("%s\n", error.c_str());
            return 1;
        }
        std::printf("Injected %ls\n", dllPath.filename().c_str());
        return 0;
    }
}  // namespace

int wmain(int argc, wchar_t** argv)
{
    // show progress right away, even when the output is redirected
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const int result = Run(argc, argv);

    // keep the window open when the injector was started by double-clicking it
    DWORD consoleProcesses[2];
    if (GetConsoleProcessList(consoleProcesses, 2) == 1)
    {
        std::printf("\nPress Enter to close this window\n");
        std::getchar();
    }

    return result;
}
