// IW5iris.exe: starts Modern Warfare 3 (2011) multiplayer the way the mod needs it and loads IW5iris.dll into it.
//
// - No game running: Plutonium's MW3 is started in LAN mode (plutonium-bootstrapper-win32.exe iw5mp <game> -lan),
//   never online, and the mod is loaded once its menu is up.
// - A game already running: the mod is loaded into it, unless it is Plutonium running online (Plutonium bans
//   players who load DLLs into the game while it runs online), singleplayer, or the 64-bit Steam game (which
//   needs a mod build of its own).
//
// Settings, all optional, in IW5iris.ini next to this file (one "key=value" a line): game (the game's folder),
// plutonium (Plutonium's folder, else %LOCALAPPDATA%\Plutonium), name (the player's name in LAN mode).
//
// Only use this offline. Never join VAC-secured or Plutonium online servers with the mod loaded.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <shellapi.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

static_assert(sizeof(void*) == 4, "The game this loads into is 32-bit, so this has to be built for Win32");

namespace
{
    constexpr wchar_t GAME_WINDOW_CLASS[] = L"IW5";
    constexpr wchar_t PLUTONIUM_IMAGE_NAME[] = L"plutonium-bootstrapper-win32.exe";
    constexpr wchar_t MOD_DLL[] = L"IW5iris.dll";
    // the menu is up about this long after the game's window shows (ReShade has swapped its device in by then)
    constexpr auto MENU_AFTER_WINDOW = std::chrono::seconds(15);
    constexpr auto WINDOW_WAIT = std::chrono::minutes(5);

    std::wstring ToLower(std::wstring text)
    {
        for (auto& c : text)
            c = static_cast<wchar_t>(towlower(c));
        return text;
    }

    std::wstring Widen(const std::string& text)
    {
        if (text.empty())
            return {};
        std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0),
                          L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                            static_cast<int>(wide.size()));
        return wide;
    }

    std::wstring Quote(const std::wstring& argument)
    {
        return L"\"" + argument + L"\"";
    }

    std::filesystem::path OwnFolder()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(path).parent_path();
    }

    std::map<std::string, std::string> ReadSettings(const std::filesystem::path& file)
    {
        std::map<std::string, std::string> settings;
        std::ifstream stream(file);
        for (std::string line; std::getline(stream, line);)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            const auto equals = line.find('=');
            if (line.empty() || line[0] == '#' || line[0] == ';' || equals == std::string::npos)
                continue;
            settings[line.substr(0, equals)] = line.substr(equals + 1);
        }
        return settings;
    }

    std::wstring ReadRegistryString(HKEY root, const wchar_t* key, const wchar_t* value)
    {
        wchar_t text[MAX_PATH]{};
        DWORD size = sizeof(text) - sizeof(wchar_t);
        if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, text, &size) != ERROR_SUCCESS)
            return {};
        return text;
    }

    // What a folder holds: the 32-bit game Plutonium runs (it has the old sound library, mss32.dll), the 64-bit game
    // Steam installs since its September 2026 update (no mss32.dll: Plutonium refuses it as "an invalid game
    // folder"), or no game at all.
    enum class FolderKind { None, Game32, Game64 };

    FolderKind KindOfFolder(const std::filesystem::path& folder)
    {
        std::error_code error;
        if (folder.empty() || !std::filesystem::is_directory(folder / "main", error) ||
            !std::filesystem::is_directory(folder / "zone", error))
            return FolderKind::None;
        return std::filesystem::is_regular_file(folder / "mss32.dll", error) ? FolderKind::Game32 : FolderKind::Game64;
    }

    struct GameFolder
    {
        std::filesystem::path folder;   // the 32-bit game's, empty when none was found
        std::filesystem::path only64;   // a 64-bit game's that was found instead
    };

    // The 32-bit game's folder: the settings file's, else where Steam's console puts the old version
    // (steamapps\content\app_...\depot_42682, the other depots moved into it), else the game's own folder
    GameFolder FindGameFolder(const std::map<std::string, std::string>& settings)
    {
        std::vector<std::filesystem::path> candidates;
        if (const auto found = settings.find("game"); found != settings.end())
            candidates.emplace_back(Widen(found->second));

        std::vector<std::filesystem::path> installs;
        installs.emplace_back(
            ReadRegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Activision\\Modern Warfare 3", L"InstallPath"));
        installs.emplace_back(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Call of Duty Modern Warfare 3");
        for (const auto& install : installs)
        {
            if (install.empty())
                continue;
            // <Steam>\steamapps\common\<game>  ->  <Steam>\steamapps\content\app_N\depot_42682
            const auto steamapps = install.parent_path().parent_path();
            for (const wchar_t* app : {L"app_42680", L"app_42690", L"app_42682"})
                candidates.push_back(steamapps / "content" / app / "depot_42682");
            candidates.push_back(install / "downgrade");
        }
        for (const auto& install : installs)
            candidates.push_back(install);

        GameFolder result;
        for (const auto& candidate : candidates)
        {
            const auto kind = KindOfFolder(candidate);
            if (kind == FolderKind::Game32)
            {
                result.folder = candidate;
                return result;
            }
            if (kind == FolderKind::Game64 && result.only64.empty())
                result.only64 = candidate;
        }
        return result;
    }

    // Whether the process was started with a -lan argument, which is how Plutonium is started in LAN mode. When
    // Plutonium runs online its command line can hold the account's token, so it is only searched for the flag and
    // never printed. (As tools/injector's.)
    std::optional<bool> HasLanArgument(HANDLE process)
    {
        constexpr ULONG PROCESS_COMMAND_LINE_INFORMATION = 60;
        struct CommandLineString
        {
            USHORT length;
            USHORT maximumLength;
            const wchar_t* buffer;
        };

        typedef LONG(NTAPI * NtQueryInformationProcess_t)(HANDLE process, ULONG infoClass, void* info,
                                                          ULONG infoLength, ULONG* returnLength);
        const auto NtQueryInformationProcess = reinterpret_cast<NtQueryInformationProcess_t>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        if (!NtQueryInformationProcess)
            return std::nullopt;

        ULONG size = 0;
        NtQueryInformationProcess(process, PROCESS_COMMAND_LINE_INFORMATION, nullptr, 0, &size);
        if (size < sizeof(CommandLineString))
            return std::nullopt;

        std::vector<std::uint8_t> info(size);
        if (NtQueryInformationProcess(process, PROCESS_COMMAND_LINE_INFORMATION, info.data(), size, &size) < 0)
            return std::nullopt;

        const auto commandLineString = reinterpret_cast<const CommandLineString*>(info.data());
        const std::wstring commandLine(commandLineString->buffer, commandLineString->length / sizeof(wchar_t));

        int argumentCount = 0;
        const auto arguments = CommandLineToArgvW(commandLine.c_str(), &argumentCount);
        if (!arguments)
            return std::nullopt;

        bool lan = false;
        for (int i = 1; i < argumentCount && !lan; ++i)
            lan = _wcsicmp(arguments[i], L"-lan") == 0;

        LocalFree(arguments);
        return lan;
    }

    bool IsModuleLoaded(DWORD processId, const std::wstring& dllName)
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;
        for (BOOL ok = Module32FirstW(snapshot, &entry); ok && !found; ok = Module32NextW(snapshot, &entry))
            found = ToLower(entry.szModule) == ToLower(dllName);

        CloseHandle(snapshot);
        return found;
    }

    // LoadLibraryW of the DLL in the game. (As tools/injector's.)
    bool Inject(HANDLE process, const std::filesystem::path& dllPath, std::string& error)
    {
        const auto path = dllPath.wstring();
        const auto size = (path.size() + 1) * sizeof(wchar_t);

        void* remotePath = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remotePath)
        {
            error = "Could not allocate memory in the game (error " + std::to_string(GetLastError()) + ")";
            return false;
        }

        if (!WriteProcessMemory(process, remotePath, path.c_str(), size, nullptr))
        {
            error = "Could not write to the game's memory (error " + std::to_string(GetLastError()) + ")";
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
            return false;
        }

        // kernel32 is mapped at the same address in every 32-bit process of this session
        const auto loadLibrary =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        const HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
        if (!thread)
        {
            error = "Could not start a thread in the game (error " + std::to_string(GetLastError()) + ")";
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
            return false;
        }

        const bool finished = WaitForSingleObject(thread, 30'000) == WAIT_OBJECT_0;
        DWORD moduleHandle = 0;
        GetExitCodeThread(thread, &moduleHandle);
        CloseHandle(thread);

        // the game may still be reading the path if LoadLibraryW hasn't returned yet
        if (!finished)
        {
            error = "The game did not finish loading the mod within 30 seconds";
            return false;
        }
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

        if (moduleHandle == 0)
        {
            error = "The game could not load IW5iris.dll. Make sure the Visual C++ 2015-2022 x86 redistributable "
                    "and DirectX 9 (June 2010) are installed.";
            return false;
        }
        return true;
    }

    // Plutonium's MW3 in LAN mode; never online. The process started, or none with why in error.
    HANDLE StartPlutonium(const std::map<std::string, std::string>& settings, std::string& error)
    {
        std::filesystem::path plutonium;
        if (const auto found = settings.find("plutonium"); found != settings.end())
            plutonium = Widen(found->second);
        else if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"))
            plutonium = std::filesystem::path(local) / "Plutonium";

        std::error_code ec;
        const auto bootstrapper = plutonium / "bin" / PLUTONIUM_IMAGE_NAME;
        if (!std::filesystem::is_regular_file(bootstrapper, ec))
        {
            error = "Modern Warfare 3 isn't running, and Plutonium wasn't found to start it with.\n"
                    "Either start the game yourself first and run this again, or install Plutonium (plutonium.pw).";
            return nullptr;
        }
        if (!std::filesystem::is_regular_file(plutonium / "games" / "iw5mp.exe", ec))
        {
            error = "Plutonium is installed but its Modern Warfare 3 isn't set up yet.\n"
                    "Start MW3 once from Plutonium's own launcher, close it, then run this again.";
            return nullptr;
        }

        const auto found = FindGameFolder(settings);
        if (found.folder.empty() && !found.only64.empty())
        {
            error = "The Modern Warfare 3 that Steam has installed is the new 64-bit version (Steam's September 2026 "
                    "update), and Plutonium only runs the older 32-bit one.\n\n"
                    "The 32-bit files can be downloaded with Steam itself (Plutonium's \"MW3 Downgrade and Update "
                    "Prevention Guide\" on forum.plutonium.pw has the steps). Once they are in\n"
                    "  <Steam>\\steamapps\\content\\app_42680\\depot_42682\n"
                    "run this again: it finds them there by itself.";
            return nullptr;
        }
        if (found.folder.empty())
        {
            error = "The game's folder wasn't found. Put a line like\n"
                    "  game=D:\\Games\\Modern Warfare 3\n"
                    "into IW5iris.ini next to this program (the folder with the 32-bit game in it).";
            return nullptr;
        }
        const auto& game = found.folder;

        std::wstring name = L"Player";
        if (const auto found = settings.find("name"); found != settings.end() && !found->second.empty())
            name = Widen(found->second);

        std::wstring command = Quote(bootstrapper.wstring()) + L" iw5mp " + Quote(game.wstring()) + L" -lan -name " +
                               Quote(name);
        std::printf("Starting Plutonium's Modern Warfare 3 in LAN mode (offline)...\n  game folder: %ls\n",
                    game.c_str());

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(bootstrapper.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            plutonium.c_str(), &startup, &process))
        {
            error = "Plutonium couldn't be started (error " + std::to_string(GetLastError()) + ")";
            return nullptr;
        }
        CloseHandle(process.hThread);
        return process.hProcess;
    }

    int Run()
    {
        std::printf("IW5iris for Modern Warfare 3 (2011)\n\n");

        const auto folder = OwnFolder();
        const auto dllPath = folder / MOD_DLL;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(dllPath, ec))
        {
            std::printf("IW5iris.dll isn't next to this program (%ls).\n", folder.c_str());
            return 1;
        }
        const auto settings = ReadSettings(folder / "IW5iris.ini");

        // a game already running is used as it is (the checks below refuse it if it can't take the mod)
        bool startedHere = false;
        HWND window = FindWindowW(GAME_WINDOW_CLASS, nullptr);
        if (!window)
        {
            std::string error;
            const HANDLE started = StartPlutonium(settings, error);
            if (!started)
            {
                std::printf("%s\n", error.c_str());
                return 1;
            }
            startedHere = true;

            std::printf("Waiting for the game's window...\n");
            const auto until = std::chrono::steady_clock::now() + WINDOW_WAIT;
            while (!(window = FindWindowW(GAME_WINDOW_CLASS, nullptr)))
            {
                if (WaitForSingleObject(started, 0) == WAIT_OBJECT_0)
                {
                    std::printf("Plutonium closed before the game showed its window.\n"
                                "Start MW3 once from Plutonium's own launcher (it may need to update), close it, "
                                "then run this again.\n");
                    CloseHandle(started);
                    return 1;
                }
                if (std::chrono::steady_clock::now() > until)
                {
                    std::printf("The game's window didn't show within five minutes.\n");
                    CloseHandle(started);
                    return 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            CloseHandle(started);
        }

        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        const HANDLE process =
            OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                            PROCESS_VM_READ | SYNCHRONIZE,
                        FALSE, processId);
        if (!process)
        {
            std::printf("The game's process couldn't be opened (error %lu). If the game runs as administrator, "
                        "run this as administrator too.\n",
                        GetLastError());
            return 1;
        }

        wchar_t imagePath[MAX_PATH]{};
        DWORD imagePathLength = MAX_PATH;
        QueryFullProcessImageNameW(process, 0, imagePath, &imagePathLength);
        const auto imageName = ToLower(std::filesystem::path(imagePath).filename().wstring());
        std::printf("Found the game: %ls\n", imagePath);

        if (imageName == L"iw5sp.exe")
        {
            std::printf("That is singleplayer. IW5iris is for multiplayer's demos.\n");
            return 1;
        }

        if (imageName == PLUTONIUM_IMAGE_NAME)
        {
            const auto lan = HasLanArgument(process);
            if (!lan || !*lan)
            {
                std::printf("Plutonium is running ONLINE (or its mode couldn't be read). Plutonium bans players who "
                            "load mods into the game while it is online, so nothing was loaded.\n"
                            "Close the game and run this program again: it starts Plutonium in LAN mode itself.\n");
                return 1;
            }
        }
        else
        {
            BOOL wow64 = FALSE;
            SYSTEM_INFO system{};
            GetNativeSystemInfo(&system);
            const bool windows64 = system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
                                   system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64;
            if (windows64 && IsWow64Process(process, &wow64) && !wow64)
            {
                std::printf("This is the 64-bit Modern Warfare 3 (Steam's 2026 version). This build of IW5iris is "
                            "for the 32-bit game (Plutonium, or iw5mp.exe 1.9.461), so nothing was loaded.\n"
                            "Close the game and run this program again to use Plutonium's MW3 in LAN mode.\n");
                return 1;
            }

            std::printf("\nThis is the Steam game, which is VAC-protected. Use the mod only for watching demos, and "
                        "never join an online match with it loaded.\nType Y and press Enter to load the mod, or "
                        "just press Enter to stop: ");
            const int answer = std::getchar();
            if (answer != 'y' && answer != 'Y')
            {
                std::printf("Nothing was loaded.\n");
                return 1;
            }
        }

        if (IsModuleLoaded(processId, MOD_DLL))
        {
            std::printf("IW5iris is already loaded in the game.\n");
            return 0;
        }

        if (startedHere)
        {
            std::printf("Waiting for the game's menu...\n");
            const auto until = std::chrono::steady_clock::now() + MENU_AFTER_WINDOW;
            while (std::chrono::steady_clock::now() < until)
            {
                if (WaitForSingleObject(process, 250) == WAIT_OBJECT_0)
                {
                    std::printf("The game closed.\n");
                    return 1;
                }
            }
        }

        std::string error;
        if (!Inject(process, dllPath, error))
        {
            std::printf("%s\n", error.c_str());
            return 1;
        }

        std::printf("\nIW5iris is loaded. Switch to the game: the editor shows after a few seconds.\n"
                    "You can close this window.\n");
        return 0;
    }
}  // namespace

int wmain()
{
    // show progress right away, even when the output is redirected
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int result = Run();

    // keep the window open when this was started by double-clicking it
    DWORD consoleProcesses[2];
    if (GetConsoleProcessList(consoleProcesses, 2) == 1)
    {
        std::printf("\nPress Enter to close this window\n");
        std::getchar();
    }
    return result;
}
