#include "Injector.hpp"

#include <Psapi.h>
#include <shellapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <format>
#include <thread>
#include <utility>
#include <vector>

namespace Injector
{
    namespace
    {
        // These three patterns are copied from t5/src/Signatures.hpp and have to be kept in sync with it.
        // d3d9DevicePointer: the operand of a mov ecx, [dx.device]
        constexpr char DEVICE_POINTER_PATTERN[] = "8B 0D ?? ?? ?? ?? 68 ?? ?? ?? ?? 51 C6 05 ?? ?? ?? ?? 00 C7 05";
        constexpr std::size_t DEVICE_POINTER_OPERAND_OFFSET = 2;
        // clientUIActives_keyCatchers: the operand of an and [keyCatchers], 1
        constexpr char KEY_CATCHERS_PATTERN[] = "83 25 ?? ?? ?? ?? 01 55 E8";
        constexpr std::size_t KEY_CATCHERS_OPERAND_OFFSET = 2;
        constexpr std::int32_t KEYCATCH_UI = 0x10;
        // Com_TimeScaleMsec only matches the multiplayer executable, so it tells multiplayer and singleplayer apart
        constexpr char MULTIPLAYER_PATTERN[] = "83 EC 10 53 56 8B F0 A1 ?? ?? ?? ?? 8B 40 18 57 8B FE 85 C0 74 04";

        constexpr auto POLL_INTERVAL = std::chrono::milliseconds(250);

        // -1 is a wildcard
        std::vector<int> ParsePattern(const char* pattern)
        {
            std::vector<int> bytes;
            for (const char* c = pattern; *c;)
            {
                if (*c == ' ')
                {
                    ++c;
                }
                else if (*c == '?')
                {
                    bytes.push_back(-1);
                    c += 2;
                }
                else
                {
                    bytes.push_back(std::stoi(std::string(c, 2), nullptr, 16));
                    c += 2;
                }
            }
            return bytes;
        }

        std::optional<std::size_t> FindPattern(const std::vector<std::uint8_t>& memory, const std::vector<int>& pattern)
        {
            if (memory.size() < pattern.size())
                return std::nullopt;

            for (std::size_t i = 0; i <= memory.size() - pattern.size(); ++i)
            {
                std::size_t j = 0;
                while (j < pattern.size() && (pattern[j] == -1 || pattern[j] == memory[i + j]))
                    ++j;

                if (j == pattern.size())
                    return i;
            }
            return std::nullopt;
        }

        // Copies a range of another process' memory; pages that can't be read stay zeroed
        std::vector<std::uint8_t> ReadRemoteMemory(HANDLE process, std::uintptr_t address, std::size_t size)
        {
            constexpr std::size_t CHUNK_SIZE = 0x10000;

            std::vector<std::uint8_t> memory(size);
            for (std::size_t offset = 0; offset < size; offset += CHUNK_SIZE)
            {
                const auto chunkSize = std::min(CHUNK_SIZE, size - offset);
                SIZE_T bytesRead = 0;
                ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address + offset), memory.data() + offset,
                                  chunkSize, &bytesRead);
            }
            return memory;
        }

        std::wstring ToLower(std::wstring string)
        {
            for (auto& c : string)
                c = static_cast<wchar_t>(std::towlower(c));
            return string;
        }

        std::string Narrow(const std::wstring& text)
        {
            const auto size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                                  nullptr, nullptr);
            std::string narrow(size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow.data(), size, nullptr,
                                nullptr);
            return narrow;
        }

        // where the DLL is loaded in the game: its base and size
        std::optional<std::pair<std::uintptr_t, std::size_t>> FindModule(DWORD processId,
                                                                         const std::filesystem::path& dllPath)
        {
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, processId);
            if (snapshot == INVALID_HANDLE_VALUE)
                return std::nullopt;

            const auto dllName = ToLower(dllPath.filename().wstring());
            std::optional<std::pair<std::uintptr_t, std::size_t>> found;

            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Module32FirstW(snapshot, &entry); ok && !found; ok = Module32NextW(snapshot, &entry))
            {
                if (ToLower(entry.szModule) == dllName)
                    found = {reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), entry.modBaseSize};
            }

            CloseHandle(snapshot);
            return found;
        }

        // whether a thread of the game started in the DLL, as the mod's own does until File > Exit
        bool RunsThreadOf(DWORD processId, std::pair<std::uintptr_t, std::size_t> module)
        {
            typedef LONG(NTAPI * NtQueryInformationThread_t)(HANDLE thread, ULONG infoClass, void* info,
                                                             ULONG infoLength, ULONG* returnLength);
            constexpr ULONG THREAD_QUERY_SET_WIN32_START_ADDRESS = 9;
            const auto NtQueryInformationThread = reinterpret_cast<NtQueryInformationThread_t>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (!NtQueryInformationThread || snapshot == INVALID_HANDLE_VALUE)
                return true;

            bool runs = false;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Thread32First(snapshot, &entry); ok && !runs; ok = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != processId)
                    continue;
                HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                std::uintptr_t start = 0;
                if (thread && NtQueryInformationThread(thread, THREAD_QUERY_SET_WIN32_START_ADDRESS, &start,
                                                       sizeof(start), nullptr) >= 0)
                    runs = start >= module.first && start < module.first + module.second;
                if (thread)
                    CloseHandle(thread);
            }
            CloseHandle(snapshot);
            return runs;
        }

        // Whether the process was started with a -lan argument, which is how Plutonium is started in LAN mode. When
        // Plutonium runs online its command line can hold the account's token, so it is only searched for the flag and
        // never printed.
        std::optional<bool> HasLanArgument(HANDLE process)
        {
            // what NtQueryInformationProcess gives for ProcessCommandLineInformation: a UNICODE_STRING followed by the
            // text
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
    }  // namespace

    HWND FindGameWindow(DWORD processId)
    {
        for (HWND window = FindWindowExW(nullptr, nullptr, GAME_WINDOW_CLASS, nullptr); window;
             window = FindWindowExW(nullptr, window, GAME_WINDOW_CLASS, nullptr))
        {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (processId == 0 || owner == processId)
                return window;
        }
        return nullptr;
    }

    Game::Game(Game&& other) noexcept
        : process(std::exchange(other.process, nullptr)),
          processId(other.processId),
          imagePath(std::move(other.imagePath))
    {
    }

    Game& Game::operator=(Game&& other) noexcept
    {
        if (this != &other)
        {
            if (process)
                CloseHandle(process);
            process = std::exchange(other.process, nullptr);
            processId = other.processId;
            imagePath = std::move(other.imagePath);
        }
        return *this;
    }

    Game::~Game()
    {
        if (process)
            CloseHandle(process);
    }

    bool Game::HasExited() const
    {
        return WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    }

    std::optional<Game> Game::Open(HWND window, std::string& error)
    {
        Game game;
        GetWindowThreadProcessId(window, &game.processId);
        game.process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                       PROCESS_VM_READ | PROCESS_VM_WRITE | SYNCHRONIZE,
                                   FALSE, game.processId);
        if (!game.process)
        {
            const auto lastError = GetLastError();
            error = std::format("Could not open the game process {} (error {}).{}", game.processId, lastError,
                                lastError == ERROR_ACCESS_DENIED ? " If the game runs as administrator, run the injector "
                                                                   "as administrator too."
                                                                 : "");
            return std::nullopt;
        }

        wchar_t imagePath[MAX_PATH]{};
        DWORD imagePathSize = MAX_PATH;
        QueryFullProcessImageNameW(game.process, 0, imagePath, &imagePathSize);
        game.imagePath = imagePath;
        const auto imageName = ToLower(std::filesystem::path(imagePath).filename().wstring());

        if (imageName == L"blackops.exe")
        {
            error = "This is the singleplayer/zombies executable, which IWXMVM doesn't support. Start BlackOpsMP.exe "
                    "instead.";
            return std::nullopt;
        }

        if (imageName == PLUTONIUM_IMAGE_NAME)
        {
            const auto lan = HasLanArgument(game.process);
            if (!lan)
            {
                error = "Could not check whether Plutonium was started in LAN mode, so IWXMVM was not injected. "
                        "Plutonium bans players who load DLLs into the game while it runs online.";
                return std::nullopt;
            }
            if (!*lan)
            {
                error = "WARNING: Plutonium is running online. Plutonium bans players who load DLLs into the game "
                        "while it runs online, so IWXMVM was not injected. Close the game and start Plutonium in LAN "
                        "mode, with -lan:\n"
                        "  plutonium-bootstrapper-win32.exe t5mp \"<game folder>\" -lan -name <name>";
                return std::nullopt;
            }
        }
        return game;
    }

    bool CheckBuild(const Game& game, Addresses& addresses, std::string& error)
    {
        HMODULE mainModule = nullptr;
        DWORD bytesNeeded = 0;
        MODULEINFO moduleInfo{};
        if (!EnumProcessModules(game.Process(), &mainModule, sizeof(mainModule), &bytesNeeded) ||
            !GetModuleInformation(game.Process(), mainModule, &moduleInfo, sizeof(moduleInfo)))
        {
            error = std::format("Could not read the game's modules (error {})", GetLastError());
            return false;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(moduleInfo.lpBaseOfDll);
        const auto image = ReadRemoteMemory(game.Process(), base, moduleInfo.SizeOfImage);

        if (!FindPattern(image, ParsePattern(MULTIPLAYER_PATTERN)))
        {
            error = "This doesn't look like a supported Black Ops multiplayer executable. Singleplayer and zombies "
                    "aren't supported, and the game may have been updated.";
            return false;
        }

        const auto devicePointerPattern = FindPattern(image, ParsePattern(DEVICE_POINTER_PATTERN));
        if (!devicePointerPattern)
        {
            error = "Could not find the game's D3D9 device pointer";
            return false;
        }

        std::memcpy(&addresses.device, image.data() + *devicePointerPattern + DEVICE_POINTER_OPERAND_OFFSET,
                    sizeof(addresses.device));

        // without it, injecting waits the longest for the menu
        if (const auto keyCatchersPattern = FindPattern(image, ParsePattern(KEY_CATCHERS_PATTERN)))
            std::memcpy(&addresses.keyCatchers, image.data() + *keyCatchersPattern + KEY_CATCHERS_OPERAND_OFFSET,
                        sizeof(addresses.keyCatchers));
        return true;
    }

    bool WaitForDevice(const Game& game, const Addresses& addresses, const std::function<bool()>& keepWaiting,
                       std::string& error)
    {
        std::uint32_t device = 0;
        while (!device)
        {
            if (game.HasExited())
            {
                error = "The game closed";
                return false;
            }
            if (keepWaiting && !keepWaiting())
            {
                error = "Stopped waiting for the game";
                return false;
            }

            ReadProcessMemory(game.Process(), reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(addresses.device)),
                              &device, sizeof(device), nullptr);
            if (!device)
                std::this_thread::sleep_for(POLL_INTERVAL);
        }
        return true;
    }

    MenuWait WaitForMenu(const Game& game, const Addresses& addresses, std::chrono::steady_clock::time_point started,
                         const std::function<bool()>& keepWaiting, std::string& error)
    {
        const auto until =
            std::max(std::chrono::steady_clock::now() + MENU_AFTER_DEVICE, started + MENU_AFTER_START);
        while (std::chrono::steady_clock::now() < until)
        {
            if (game.HasExited())
            {
                error = "The game closed";
                return MenuWait::Failed;
            }
            if (keepWaiting && !keepWaiting())
            {
                error = "Stopped waiting for the game";
                return MenuWait::Failed;
            }

            std::int32_t keyCatchers = 0;
            if (addresses.keyCatchers &&
                ReadProcessMemory(game.Process(),
                                  reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(addresses.keyCatchers)),
                                  &keyCatchers, sizeof(keyCatchers), nullptr) &&
                (keyCatchers & KEYCATCH_UI))
                return MenuWait::Shown;
            std::this_thread::sleep_for(POLL_INTERVAL);
        }
        return MenuWait::TimedOut;
    }

    bool IsModuleLoaded(DWORD processId, const std::filesystem::path& dllPath)
    {
        return FindModule(processId, dllPath).has_value();
    }

    bool Inject(const Game& game, const std::filesystem::path& dllPath, std::string& error)
    {
        const auto process = game.Process();
        const auto path = dllPath.wstring();
        const auto size = (path.size() + 1) * sizeof(wchar_t);

        void* remotePath = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remotePath)
        {
            error = std::format("Could not allocate memory in the game (error {})", GetLastError());
            return false;
        }

        if (!WriteProcessMemory(process, remotePath, path.c_str(), size, nullptr))
        {
            error = std::format("Could not write to the game's memory (error {})", GetLastError());
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
            return false;
        }

        // kernel32 is mapped at the same address in every 32-bit process of this session
        const auto loadLibrary =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

        HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
        if (!thread)
        {
            error = std::format("Could not start a thread in the game (error {})", GetLastError());
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
            return false;
        }

        const bool finished = WaitForSingleObject(thread, 30'000) == WAIT_OBJECT_0;
        DWORD moduleHandle = 0;
        GetExitCodeThread(thread, &moduleHandle);
        CloseHandle(thread);

        // the game may still be reading the path if LoadLibraryW hasn't returned yet
        if (finished)
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

        if (!finished)
        {
            error = "LoadLibraryW did not return within 30 seconds";
            return false;
        }

        if (moduleHandle == 0)
        {
            error = "The game failed to load the DLL. Make sure the Visual C++ 2015-2022 x86 redistributable is "
                    "installed.";
            return false;
        }

        return true;
    }

    // Takes the DLL out of the game with the loader's LdrUnloadDll (which Plutonium leaves alone), once for each
    // reference until it is gone; only once File > Exit has ended the mod's thread
    bool Unload(const Game& game, const std::filesystem::path& dllPath, std::string& message)
    {
        const auto name = Narrow(dllPath.filename().wstring());
        const auto module = FindModule(game.ProcessId(), dllPath);
        if (!module)
        {
            message = name + " isn't loaded in the game";
            return true;
        }
        if (RunsThreadOf(game.ProcessId(), *module))
        {
            message = "IWXMVM is still running in the game: unload it with File > Exit first";
            return false;
        }

        // ntdll is mapped at the same address in every 32-bit process of this session
        const auto ldrUnloadDll =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrUnloadDll"));
        constexpr int MAX_REFERENCES = 32;
        for (int references = 1; references <= MAX_REFERENCES; ++references)
        {
            HANDLE thread = CreateRemoteThread(game.Process(), nullptr, 0, ldrUnloadDll,
                                               reinterpret_cast<void*>(module->first), 0, nullptr);
            if (!thread)
            {
                message = std::format("Could not start a thread in the game (error {})", GetLastError());
                return false;
            }
            WaitForSingleObject(thread, 30'000);
            CloseHandle(thread);
            if (!IsModuleLoaded(game.ProcessId(), dllPath))
            {
                message = std::format("Unloaded {} ({} references)", name, references);
                return true;
            }
        }
        message = std::format("{} is still loaded after {} unloads", name, MAX_REFERENCES);
        return false;
    }
}  // namespace Injector
