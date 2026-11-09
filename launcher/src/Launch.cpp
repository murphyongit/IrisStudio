#include "Launch.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>

#include "Injector.hpp"
#include "Platform.hpp"
#include "Setup/Folders.hpp"
#include "Setup/SetupLogic.hpp"
#include "../res/resource.h"

namespace Launcher
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // how long the game gets to show its window
        constexpr auto WINDOW_TIMEOUT = std::chrono::seconds(60);
    }  // namespace

    struct LaunchJob::Work
    {
        Options options;
        Settings settings;
        Detection detection;
        std::mutex mutex;
        LaunchStatus status;
        std::atomic<bool> stop = false;
        std::ofstream log;
        std::thread thread;

        void Log(std::string_view line)
        {
            log << line << '\n';
            log.flush();
        }

        void Stage(LaunchStage stage)
        {
            std::lock_guard lock(mutex);
            status.stage = stage;
        }

        void Fail(const std::string& error)
        {
            Log("ERROR " + error);
            std::lock_guard lock(mutex);
            status.stage = LaunchStage::Failed;
            status.error = error;
        }

        // sleeps a while unless asked to stop; false when asked
        bool Wait(std::chrono::milliseconds time)
        {
            const auto until = Clock::now() + time;
            while (Clock::now() < until)
            {
                if (stop)
                    return false;
                Sleep(100);
            }
            return !stop;
        }

        // the embedded T5iris.dll in the launcher's folder, written when it isn't the same; empty on a failure
        std::filesystem::path WriteDll();
        void Run();
    };

    std::filesystem::path LaunchJob::Work::WriteDll()
    {
        const auto embedded = Platform::Resource(IDR_T5_DLL);
        if (embedded.empty())
        {
            Fail("The launcher has no T5iris.dll in it");
            return {};
        }
        const auto path = options.home / IWXMVM::Setup::T5_DLL;
        const auto existing = Platform::Sha256File(path);
        if (existing && *existing == Platform::Sha256(embedded))
            return path;
        if (!Platform::WriteFile(path, embedded))
        {
            Fail("T5iris.dll couldn't be written to " + Platform::Utf8(options.home) +
                 (existing ? ": is a game still running with the old one?" : ""));
            return {};
        }
        Log("wrote " + Platform::Utf8(path));
        return path;
    }

    void LaunchJob::Work::Run()
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        Log(std::format("Iris Studio launch, {:04}-{:02}-{:02} {:02}:{:02}:{:02}", time.wYear, time.wMonth, time.wDay,
                        time.wHour, time.wMinute, time.wSecond));
        const auto dll = WriteDll();
        if (dll.empty())
            return;

        // a game already running is used as it is (the checks below refuse it if Plutonium runs online)
        DWORD startedId = 0;
        HANDLE startedProcess = nullptr;
        auto started = Clock::now();
        HWND window = Injector::FindGameWindow();
        if (window)
        {
            Log("Black Ops is running already");
            started -= Injector::MENU_AFTER_START;
        }
        else
        {
            // Plutonium in LAN, as tools/t5-harness/launch.ps1 starts it; never online
            const auto player = IWXMVM::Setup::PlayerName(settings.playerName);
            std::wstring command = IWXMVM::Setup::QuoteArgument(detection.bootstrapper.wstring()) + L" t5mp " +
                                   IWXMVM::Setup::QuoteArgument(detection.gameFolder.wstring()) + L" -lan -name " +
                                   IWXMVM::Setup::QuoteArgument(Platform::FromUtf8(player).wstring());
            // the Mod row's pick, as the folder has it; one gone starts no mod
            for (const auto& mod : detection.mods)
                if (!settings.gameMod.empty() && _stricmp(mod.name.c_str(), settings.gameMod.c_str()) == 0)
                    command += L" +set fs_game " + IWXMVM::Setup::QuoteArgument(
                                                       Platform::FromUtf8(IWXMVM::Components::DemoModLogic::GetFsGame(
                                                                              mod.name))
                                                           .wstring());
            Log("starting " + Platform::Utf8(std::filesystem::path(command)));
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                options.plutonium.c_str(), &startup, &process))
            {
                Fail(std::format("Plutonium couldn't be started (error {})", GetLastError()));
                return;
            }
            CloseHandle(process.hThread);
            startedId = process.dwProcessId;
            startedProcess = process.hProcess;
            started = Clock::now();

            Stage(LaunchStage::WaitingForWindow);
            while (!(window = Injector::FindGameWindow(startedId)) && !(window = Injector::FindGameWindow()))
            {
                if (Clock::now() - started > WINDOW_TIMEOUT)
                {
                    const bool closed = WaitForSingleObject(startedProcess, 0) == WAIT_OBJECT_0;
                    CloseHandle(startedProcess);
                    Fail(closed ? "Plutonium closed before T5 showed its window: start it once from its own launcher"
                                : "T5 didn't show its window within a minute");
                    return;
                }
                if (!Wait(std::chrono::milliseconds(250)))
                {
                    CloseHandle(startedProcess);
                    return;
                }
            }
            CloseHandle(startedProcess);
        }

        std::string error;
        auto game = Injector::Game::Open(window, error);
        if (!game)
        {
            Fail(error);
            return;
        }
        Log("found the game window in " + Platform::Utf8(std::filesystem::path(game->ImagePath())) +
            std::format(" (process {})", game->ProcessId()));
        Injector::Addresses addresses;
        if (!Injector::CheckBuild(*game, addresses, error))
        {
            Fail(error);
            return;
        }

        Stage(LaunchStage::WaitingForMenu);
        const auto keepWaiting = [this] { return !stop.load(); };
        if (!Injector::WaitForDevice(*game, addresses, keepWaiting, error))
        {
            if (!stop)
                Fail(error);
            return;
        }
        Log("the game has its D3D9 device");
        switch (Injector::WaitForMenu(*game, addresses, started, keepWaiting, error))
        {
            case Injector::MenuWait::Shown:
                Log("the game shows its menu");
                break;
            case Injector::MenuWait::TimedOut:
                Log("no menu seen; loading the mod anyway");
                break;
            case Injector::MenuWait::Failed:
                if (!stop)
                    Fail("T5 closed before its menu");
                return;
        }

        if (Injector::IsModuleLoaded(game->ProcessId(), dll))
        {
            Log("T5iris.dll is loaded in the game already");
        }
        else
        {
            Stage(LaunchStage::Loading);
            if (!Injector::Inject(*game, dll, error))
            {
                Fail(error);
                return;
            }
            Log("injected " + Platform::Utf8(dll));
        }
        Stage(LaunchStage::Running);

        // until the game closes; closing the launcher leaves it running
        while (!game->HasExited())
            if (!Wait(std::chrono::milliseconds(500)))
                return;
        Log("the game closed");
        Stage(LaunchStage::Closed);
    }

    LaunchJob::LaunchJob(const Options& options, const Settings& settings, const Detection& detection)
        : work(std::make_shared<Work>())
    {
        work->options = options;
        work->settings = settings;
        work->detection = detection;
        std::error_code error;
        std::filesystem::create_directories(LogFile().parent_path(), error);
        work->log.open(LogFile(), std::ios::trunc);
        work->thread = std::thread([work = work] { work->Run(); });
    }

    LaunchJob::~LaunchJob()
    {
        work->stop = true;
        if (work->thread.joinable())
            work->thread.join();
    }

    LaunchStatus LaunchJob::Status() const
    {
        std::lock_guard lock(work->mutex);
        return work->status;
    }

    std::filesystem::path LaunchJob::LogFile() const
    {
        return work->options.home / "logs" / "launch.log";
    }
}  // namespace Launcher
