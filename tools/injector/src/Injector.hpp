#pragma once

// The injector's work, shared by injector.exe (main.cpp) and the launcher (launcher/src/Launch.cpp): finding Call of
// Duty: Black Ops multiplayer (Steam BlackOpsMP.exe or Plutonium T5), checking that it is the supported multiplayer
// build and, for Plutonium, that it was started in LAN mode, waiting for its D3D9 device and its menu, and loading
// T5iris.dll into it with LoadLibraryW (or taking it out again).
//
// Only use this offline. Never join VAC-secured or Plutonium online servers with the mod loaded. Plutonium bans players
// who load DLLs into the game while it runs online, so Open refuses Plutonium unless it was started in LAN mode.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

static_assert(sizeof(void*) == 4, "The game is 32-bit, so the injector has to be built for Win32");

namespace Injector
{
    inline constexpr wchar_t GAME_WINDOW_CLASS[] = L"CoDBlackOps";
    inline constexpr wchar_t PLUTONIUM_IMAGE_NAME[] = L"plutonium-bootstrapper-win32.exe";

    // the game's window; of a process when one is given
    HWND FindGameWindow(DWORD processId = 0);

    // The game's process, opened to load the DLL into it
    class Game
    {
       public:
        Game() = default;
        Game(const Game&) = delete;
        Game& operator=(const Game&) = delete;
        Game(Game&& other) noexcept;
        Game& operator=(Game&& other) noexcept;
        ~Game();

        HANDLE Process() const
        {
            return process;
        }
        DWORD ProcessId() const
        {
            return processId;
        }
        const std::wstring& ImagePath() const
        {
            return imagePath;
        }
        bool HasExited() const;

        // The process of the game's window, checked: not singleplayer's BlackOps.exe, Plutonium only in LAN mode. None,
        // with why in error, when it can't be opened or is one of those.
        static std::optional<Game> Open(HWND window, std::string& error);

       private:
        HANDLE process = nullptr;
        DWORD processId = 0;
        std::wstring imagePath;
    };

    // Where CheckBuild found the game's D3D9 device pointer and its key catchers (0 when not found)
    struct Addresses
    {
        std::uint32_t device = 0;
        std::uint32_t keyCatchers = 0;
    };

    // Whether the executable is a supported multiplayer build, and its addresses; false, with why in error, when it
    // isn't
    bool CheckBuild(const Game& game, Addresses& addresses, std::string& error);

    // Waits until the game has created its D3D9 device, as long as keepWaiting says so (checked every 250 ms); false,
    // with why in error, when the game closed or keepWaiting said to stop
    bool WaitForDevice(const Game& game, const Addresses& addresses, const std::function<bool()>& keepWaiting,
                       std::string& error);

    // Injecting before the menu is up can miss ReShade: it swaps its own device in about 0.4 s after the game creates
    // one. The menu's UI takes the keys about 4 s after the device (2026-10-28, Plutonium with ReShade), 2 s before
    // the menu fades in. Without that, injecting waits at most this long after the game started and after its device,
    // the fixed wait used before.
    inline constexpr auto MENU_AFTER_START = std::chrono::seconds(25);
    inline constexpr auto MENU_AFTER_DEVICE = std::chrono::seconds(5);

    enum class MenuWait
    {
        Shown,     // the menu's UI took the keys
        TimedOut,  // the longest wait passed first
        Failed,    // the game closed or keepWaiting said to stop; error says which
    };

    // Waits, after WaitForDevice, until the game's menu is up, at most until MENU_AFTER_START after started and
    // MENU_AFTER_DEVICE after now, as long as keepWaiting says so
    MenuWait WaitForMenu(const Game& game, const Addresses& addresses, std::chrono::steady_clock::time_point started,
                         const std::function<bool()>& keepWaiting, std::string& error);

    bool IsModuleLoaded(DWORD processId, const std::filesystem::path& dllPath);

    // LoadLibraryW of the DLL in the game; false, with why in error, when it didn't load
    bool Inject(const Game& game, const std::filesystem::path& dllPath, std::string& error);

    // Takes the DLL out of the game after File > Exit (Plutonium keeps it loaded); message says what happened, false
    // when it didn't work
    bool Unload(const Game& game, const std::filesystem::path& dllPath, std::string& message);
}  // namespace Injector
