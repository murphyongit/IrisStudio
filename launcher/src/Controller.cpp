#include "Controller.hpp"

#include <Windows.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <thread>

#include "Beta.hpp"
#include "Detect.hpp"
#include "Launch.hpp"
#include "Net.hpp"
#include "Platform.hpp"
#include "Setup.hpp"
#include "Setup/Folders.hpp"
#include "Version.hpp"

namespace Launcher
{
    namespace Setup = IWXMVM::Setup;
    namespace Beta = IWXMVM::Beta;

    namespace
    {
        // the repository whose latest release the update check asks for
        constexpr wchar_t LATEST_RELEASE_URL[] =
            L"https://api.github.com/repos/yerbisonline/IrisStudio/releases/latest";

        const char* const ELLIPSIS = "\xE2\x80\xA6";

        std::string Drive(const std::filesystem::path& path)
        {
            return Platform::Utf8(path.root_name());
        }

        // an error as the home view's line: a sentence's start
        std::string Sentence(std::string text)
        {
            if (!text.empty())
                text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
            return text;
        }

        // "OpenAssetTools, FFmpeg"
        std::string List(const std::vector<std::string>& items)
        {
            std::string text;
            for (const auto& item : items)
                text += (text.empty() ? "" : ", ") + item;
            return text;
        }

        // a game folder's name, and whether it is Steam's copy: the one to tell apart from a copy of the user's own
        std::string FolderName(const std::filesystem::path& folder)
        {
            auto lower = Platform::Utf8(folder);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            return Platform::Utf8(folder.filename()) +
                   (lower.find("\\steamapps\\common\\") != std::string::npos ? " (Steam)" : "");
        }
    }  // namespace

    struct Controller::State
    {
        Options options;
        Settings* settings = nullptr;
        std::filesystem::path detectedFor;  // the folder of the settings when the last detection started

        std::future<Detection> detecting;
        std::optional<Detection> detection;
        bool detectAgain = false;  // once the one running is done

        std::unique_ptr<SetupJob> setup;
        SetupStatus setupStatus;
        std::string setupError;  // of the last setup, until the next one
        std::filesystem::path setupLog;

        std::unique_ptr<LaunchJob> launch;
        LaunchStatus launchStatus;
        std::string launchError;
        std::filesystem::path launchLog;

        // the update check's answer, on a thread of its own that the launcher doesn't wait for when it closes
        struct UpdateCheck
        {
            std::mutex mutex;
            std::string url;  // the page of a newer release
            std::atomic<bool> stop = false;
        };
        std::shared_ptr<UpdateCheck> updateCheck = std::make_shared<UpdateCheck>();

        bool summaryOpen = false;  // Set up T5iris's summary, waiting for Accept or Cancel
        bool rebuild = false;      // the setup it sums up builds everything again
        std::string notice;        // a problem of the last thing the user did (a folder that isn't Black Ops')

        // a beta build's key: beta.key in the launcher's folder, checked at the start, on
        // Paste key and before each launch
        std::optional<std::string> thisPc;
        Beta::Verdict keyVerdict = Beta::Verdict::NoKey;
        Beta::Key key;             // the key of beta.key, when it is one
        std::string pasteProblem;  // why the last key pasted doesn't work, until one does
        bool keyPasted = false;    // a key just worked: the home view says whose it is until the next button

        bool KeyWorks() const
        {
            return !BETA || (BETA_TEST_KEY && options.noKeyCheck) || keyVerdict == Beta::Verdict::Valid;
        }

        void CheckKey()
        {
            thisPc = Beta::ThisPcCode();
            const auto text = Platform::ReadFile(options.home / Beta::KEY_FILE).value_or("");
            Beta::Key found;
            keyVerdict = Beta::Check(text, thisPc.value_or(""), Beta::Today(), BETA_PUBLIC_KEY, &found);
            key = found;
        }

        // Paste key: the clipboard's key (--beta-key's in tests), kept only when it works, so a key that worked stays
        // when one that doesn't is pasted
        void PasteKey()
        {
            const auto text = !options.betaKey.empty() ? options.betaKey : Platform::ClipboardText().value_or("");
            if (!Beta::HasKeyPrefix(text))
            {
                pasteProblem = Beta::NO_KEY_ON_CLIPBOARD;
                return;
            }
            Beta::Key found;
            const auto verdict = Beta::Check(text, thisPc.value_or(""), Beta::Today(), BETA_PUBLIC_KEY, &found);
            if (verdict != Beta::Verdict::Valid)
            {
                pasteProblem = Beta::KeyScreenLine(verdict, found);
                return;
            }
            if (!Platform::WriteFile(options.home / Beta::KEY_FILE, found.text + "\n"))
            {
                pasteProblem = "The key couldn't be saved";
                return;
            }
            keyVerdict = Beta::Verdict::Valid;
            key = found;
            pasteProblem.clear();
            keyPasted = true;
        }

        void Detect()
        {
            if (detecting.valid())
            {
                detectAgain = true;
                return;
            }
            detectedFor = settings->gameFolder;
            detecting = std::async(std::launch::async, [options = options, settings = *settings] {
                return Launcher::Detect(options, settings);
            });
        }

        // what a setup does now: OpenAssetTools when the zones need it and it isn't here, FFmpeg when the mod has
        // none, the zones and the pack when they aren't built (all of them for a rebuild)
        SetupJob::Plan Plan() const
        {
            SetupJob::Plan plan;
            if (!detection || !detection->GameFound())
                return plan;
            plan.rebuild = rebuild;
            plan.zones = rebuild || !detection->ZonesReady();
            plan.pack = rebuild || !detection->PackReady();
            plan.download = plan.zones && detection->linker.empty();
            plan.ffmpeg = detection->ffmpeg.empty();
            return plan;
        }

        bool NeedsSetup() const
        {
            const auto plan = Plan();
            return plan.zones || plan.pack || plan.ffmpeg;
        }

        // the summary's numbers for the plan
        Setup::SetupEstimate Estimate(const SetupJob::Plan& plan) const
        {
            const auto& d = *detection;
            Setup::SetupNeeds needs;
            needs.openAssetTools = plan.download;
            needs.ffmpeg = plan.ffmpeg;
            if (plan.zones)
                needs.zones = plan.rebuild || d.zonesStale ? d.zonesWanted : d.zonesWanted - d.zonesPresent;
            if (plan.pack)
                needs.animations = d.packWanted;
            return Setup::Estimate(needs);
        }

        // Accept: the setup, if there is room for it
        void Accept()
        {
            summaryOpen = false;
            if (!detection || !detection->GameFound())
                return;
            const auto plan = Plan();
            const auto estimate = Estimate(plan);
            if (detection->freeBytes < estimate.spaceBytes)
            {
                notice = std::format("Needs {} on {}, {} is free", Setup::FormatSize(estimate.spaceBytes),
                                     Drive(options.storage), Setup::FormatSize(detection->freeBytes));
                return;
            }
            setupError.clear();
            launchError.clear();
            notice.clear();
            setupStatus = {};
            setup = std::make_unique<SetupJob>(options, *detection, plan);
            setupLog = setup->LogFile();
            rebuild = false;
        }
    };

    Controller::Controller(const Options& options, Settings& settings)
        : state(std::make_unique<State>())
    {
        state->options = options;
        state->settings = &settings;
        if (BETA)
            state->CheckKey();
        state->Detect();
        // the update check, when Settings has it on: GitHub's latest release, never a download. A beta build has
        // none: its repository is private, and new builds go to testers by hand.
        if (!BETA && settings.checkUpdates && !options.noUpdateCheck && options.shot.empty())
            std::thread([check = state->updateCheck, log = options.home / "logs" / "update.log"] {
                std::string error;
                const auto answer =
                    Net::Get(LATEST_RELEASE_URL, 1 << 20, check->stop, error, {}, L"application/vnd.github+json");
                const auto release = answer ? Setup::ReadLatestRelease(std::string_view(
                                                  reinterpret_cast<const char*>(answer->data()), answer->size()))
                                            : std::nullopt;
                const bool newer = release && Setup::IsNewerVersion(release->tag, IRIS_STUDIO_VERSION_STRING);
                // what it found, for when "Update available" doesn't show
                Platform::WriteFile(log, std::format("releases/latest: {}\n",
                                                     !answer    ? error
                                                     : !release ? std::string("no release")
                                                                : release->tag + (newer ? ", newer" : ", not newer")));
                if (newer)
                {
                    std::lock_guard lock(check->mutex);
                    check->url = release->url;
                }
            }).detach();
    }

    Controller::~Controller()
    {
        state->updateCheck->stop = true;
    }

    std::string Controller::UpdateUrl() const
    {
        std::lock_guard lock(state->updateCheck->mutex);
        return state->updateCheck->url;
    }

    void Controller::Update()
    {
        auto& s = *state;
        if (s.detecting.valid() && s.detecting.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            s.detection = s.detecting.get();
            // a folder found through Steam is kept, so the next start finds it at once (the summary shows it, with
            // Change, and Settings can change it later)
            if (s.detection->gameFrom != GameFrom::Settings && s.detection->GameFound())
            {
                s.settings->gameFolder = s.detection->gameFolder;
                s.detectedFor = s.settings->gameFolder;
                s.settings->Save(s.options.home / "launcher.json");
            }
            if (std::exchange(s.detectAgain, false))
                s.Detect();
        }

        if (s.setup)
        {
            s.setupStatus = s.setup->Status();
            if (s.setupStatus.finished)
            {
                if (!s.setupStatus.cancelled && !s.setupStatus.error.empty())
                    s.setupError = s.setupStatus.error;
                s.setup.reset();
                s.Detect();
            }
        }

        if (s.launch)
        {
            s.launchStatus = s.launch->Status();
            if (s.launchStatus.stage == LaunchStage::Failed || s.launchStatus.stage == LaunchStage::Closed)
            {
                if (s.launchStatus.stage == LaunchStage::Failed)
                    s.launchError = s.launchStatus.error;
                s.launch.reset();
                s.Detect();
            }
        }
    }

    bool Controller::Busy() const
    {
        return state->setup || state->launch || state->detecting.valid();
    }

    void Controller::Activated()
    {
        if (!state->setup && !state->launch)
            state->Detect();
    }

    void Controller::SettingsChanged()
    {
        if (state->settings->gameFolder != state->detectedFor)
            state->Detect();
    }

    std::string Controller::GameFolderText() const
    {
        if (state->detection && state->detection->GameFound())
            return Platform::Utf8(state->detection->gameFolder);
        return Platform::Utf8(state->settings->gameFolder);
    }

    bool Controller::Detected() const
    {
        return state->detection.has_value() && !state->detecting.valid();
    }

    bool Controller::CanRebuild() const
    {
        return state->detection && state->detection->GameFound() && !state->setup && !state->launch;
    }

    std::pair<std::string, bool> Controller::BetaKeyRow() const
    {
        const auto& s = *state;
        if (!s.pasteProblem.empty())
            return {s.pasteProblem, true};
        if (s.keyVerdict == Beta::Verdict::Valid)
            return {Beta::KeyText(s.key), false};
        if (s.keyVerdict == Beta::Verdict::NoKey)
            return {"None", false};
        return {Beta::KeyScreenLine(s.keyVerdict, s.key), true};
    }

    void Controller::Do(Action action)
    {
        auto& s = *state;
        if (action != Action::PasteKey && action != Action::CopyPcCode)
        {
            s.keyPasted = false;
            s.pasteProblem.clear();
        }
        switch (action)
        {
            case Action::PasteKey:
                if (BETA)
                    s.PasteKey();
                break;
            case Action::CopyPcCode:
                if (s.thisPc)
                    Platform::SetClipboardText(Beta::FormatPcCode(*s.thisPc));
                break;
            case Action::SetUp:
            case Action::Rebuild:
                if (s.detection && s.detection->GameFound() && !s.setup && !s.launch)
                {
                    s.rebuild = action == Action::Rebuild;
                    s.summaryOpen = true;
                }
                break;
            case Action::Accept:
                if (!s.setup && !s.launch)
                    s.Accept();
                break;
            case Action::Cancel:
                if (s.setup)
                    s.setup->Cancel();
                s.summaryOpen = false;
                s.rebuild = false;
                break;
            case Action::Launch:
                // the key again, in case its last day passed while the launcher was open
                if (BETA)
                    s.CheckKey();
                if (s.detection && !s.launch && !s.setup && s.KeyWorks())
                {
                    s.launchError.clear();
                    s.notice.clear();
                    s.launchStatus = {};
                    s.launch = std::make_unique<LaunchJob>(s.options, *s.settings, *s.detection);
                    s.launchLog = s.launch->LogFile();
                }
                break;
            case Action::BrowseGame:
            {
                const auto start = s.detection && s.detection->GameFound() ? s.detection->gameFolder
                                                                            : std::filesystem::path("C:\\");
                const auto picked = Platform::PickFolder(L"The Black Ops folder (with BlackOpsMP.exe)", start);
                if (!picked)
                    break;
                if (const auto folder = GameFolderOf(*picked))
                {
                    s.notice.clear();
                    s.settings->gameFolder = *folder;
                    s.settings->Save(s.options.home / "launcher.json");
                    SettingsChanged();
                }
                else
                {
                    s.notice = "That folder has no BlackOpsMP.exe";
                }
                break;
            }
            case Action::GetPlutonium:
                Platform::OpenUrl(L"https://plutonium.pw/");
                break;
            case Action::OpenPlutonium:
                Platform::OpenFile(s.options.plutonium / "bin" / "plutonium-launcher-win32.exe");
                break;
            case Action::OpenLog:
                if (!s.setupLog.empty() && !s.setupError.empty())
                    Platform::OpenFile(s.setupLog);
                else if (!s.launchLog.empty())
                    Platform::OpenFile(s.launchLog);
                break;
            case Action::OpenModFolder:
                Platform::OpenFolder(s.options.storage);
                break;
            case Action::RemoveFiles:
                RemoveFiles();
                break;
            default:
                break;
        }
    }

    void Controller::RemoveFiles()
    {
        auto& s = *state;
        if (s.setup || s.launch)
            return;
        std::error_code error;
        // the zones of the catalog, the pack and the setup's record; the user's scenes, keyframes and config stay
        try
        {
            for (const auto& zone : LoadCatalogs().zones)
                std::filesystem::remove(s.options.storage / "zones" / Platform::FromUtf8(zone.name + ".ff"), error);
        }
        catch (const std::exception&)
        {
        }
        std::filesystem::remove(s.options.storage / "animations.pack", error);
        std::filesystem::remove(s.options.storage / "setup.json", error);
        // the launcher's own: the DLL, FFmpeg and the tools it downloaded (not launcher.json, which it still uses)
        std::filesystem::remove(s.options.home / IWXMVM::Setup::T5_DLL, error);
        std::filesystem::remove(s.options.home / IWXMVM::Setup::OLD_T5_DLL, error);
        std::filesystem::remove(s.options.home / "ffmpeg.exe", error);
        std::filesystem::remove_all(s.options.home / "tools", error);
        s.Detect();
    }

    Home Controller::BuildHome() const
    {
        const auto& s = *state;
        Home home;
        // a problem: its line and the one button that fixes it
        const auto problem = [&home](std::string line, std::string button, Action action) {
            home.line = std::move(line);
            home.lineBad = true;
            home.buttonLabel = std::move(button);
            home.action = action;
        };
        const auto quiet = [&home](std::string line, std::string label, ButtonIcon icon) {
            home.line = std::move(line);
            home.button = ButtonStyle::Quiet;
            home.buttonLabel = std::move(label);
            home.icon = icon;
            home.enabled = false;
        };

        // a beta build without a key that works: the PC code to send, and Paste key, before anything else
        if (!s.KeyWorks())
        {
            home.keyScreen = true;
            home.pcCode = s.thisPc ? Beta::FormatPcCode(*s.thisPc) : "";
            home.line = !s.pasteProblem.empty() ? s.pasteProblem
                        : !s.thisPc             ? Beta::KeyScreenLine(Beta::Verdict::NoPcCode, s.key)
                                                : Beta::KeyScreenLine(s.keyVerdict, s.key);
            home.lineBad = !s.pasteProblem.empty() || !s.thisPc || s.keyVerdict != Beta::Verdict::NoKey;
            home.buttonLabel = "Paste key";
            home.action = Action::PasteKey;
            home.icon = ButtonIcon::Paste;
            return home;
        }

        if (!s.detection)
        {
            quiet("", std::string("Checking") + ELLIPSIS, ButtonIcon::Busy);
            return home;
        }
        const auto& d = *s.detection;

        if (s.launch)
        {
            const auto stage = s.launchStatus.stage;
            if (stage == LaunchStage::Running)
                quiet("Press F1 in the game for T5iris", "Running", ButtonIcon::Done);
            else
                quiet(stage == LaunchStage::Loading ? "Loading T5iris" : "Starting Plutonium in LAN mode",
                      std::string("Starting") + ELLIPSIS, ButtonIcon::Busy);
            return home;
        }

        if (s.setup)
        {
            const auto& status = s.setupStatus;
            home.progress = true;
            home.progressFraction = status.fraction;
            switch (status.stage)
            {
                case SetupStage::Download:
                case SetupStage::FFmpeg:
                    home.progressLabel =
                        status.stage == SetupStage::Download ? "Getting OpenAssetTools" : "Getting FFmpeg";
                    if (status.downloadSize > 0)
                        home.progressCount = std::format("{} / {}", Setup::FormatSize(status.downloaded),
                                                         Setup::FormatSize(status.downloadSize));
                    break;
                case SetupStage::Zones:
                    home.progressLabel = "Building character zones";
                    home.progressCount = std::format("{} / {}", status.done, status.total);
                    break;
                default:
                    home.progressLabel = "Writing the animation pack";
                    home.progressCount = std::format("{} / {}", status.done, status.total);
                    break;
            }
            home.button = ButtonStyle::Quiet;
            home.buttonLabel = "Cancel";
            home.action = Action::Cancel;
            return home;
        }

        if (s.summaryOpen && d.GameFound())
        {
            const auto plan = s.Plan();
            const auto estimate = s.Estimate(plan);
            std::vector<std::string> downloads, builds;
            if (plan.download)
                downloads.push_back("OpenAssetTools");
            if (plan.ffmpeg)
                downloads.push_back("FFmpeg");
            if (plan.zones)
                builds.push_back("Character zones");
            if (plan.pack)
                builds.push_back(builds.empty() ? "Animations" : "animations");
            home.summary = true;
            home.gameFolderPath = Platform::Utf8(d.gameFolder);
            home.gameFolder = FolderName(d.gameFolder);
            home.downloads = downloads.empty() ? "None" : List(downloads);
            home.downloadsSize = downloads.empty() ? "-" : Setup::FormatSize(estimate.downloadBytes);
            home.builds = builds.empty() ? "None" : List(builds);
            home.buildsSize = builds.empty() ? "-" : Setup::FormatSize(estimate.buildBytes);
            home.spaceNeeded = Setup::FormatSize(estimate.spaceBytes);
            home.duration = Setup::Duration(estimate.seconds);
            return home;
        }

        if (!d.catalogError.empty())
        {
            problem("The launcher is damaged: download it again", "Set up T5iris", Action::None);
            home.enabled = false;
            return home;
        }
        if (!d.GameFound())
        {
            problem(!s.notice.empty()    ? s.notice
                    : d.savedFolderGone ? "The Black Ops folder moved"
                                        : "Black Ops wasn't found",
                    "Choose folder", Action::BrowseGame);
            return home;
        }
        if (d.plutonium == PlutoniumState::Missing)
        {
            problem("Plutonium wasn't found", "Get Plutonium", Action::GetPlutonium);
            return home;
        }
        if (d.plutonium == PlutoniumState::NotUpdated)
        {
            problem("Log in to Plutonium once", "Open Plutonium", Action::OpenPlutonium);
            return home;
        }

        if (!s.setupError.empty())
        {
            problem(Sentence(s.setupError), "Set up T5iris", Action::SetUp);
            home.link = "Log";
            home.linkAction = Action::OpenLog;
            return home;
        }
        if (s.NeedsSetup())
        {
            home.buttonLabel = "Set up T5iris";
            home.action = Action::SetUp;
            if (!s.notice.empty())
                home.line = s.notice, home.lineBad = true;
            else if (s.keyPasted)
                home.line = Beta::ValidLine(s.key), home.lineGood = true;
            else if (d.zonesStale && d.zonesPresent > 0)
                home.line = d.newCharacters ? "New characters to set up" : "Black Ops was updated";
            else
                home.line = "Black Ops and Plutonium found";
            return home;
        }

        home.buttonLabel = "Launch T5iris";
        home.action = Action::Launch;
        home.icon = ButtonIcon::Play;
        // the game's mod: a row when the mods folders have one; a mod picked that is
        // gone starts no mod, and the line says so
        const auto& gameMod = s.settings->gameMod;
        const bool modGone = !gameMod.empty() && !IWXMVM::Components::DemoModLogic::HasMod(d.mods, gameMod);
        home.modRow = !d.mods.empty();
        home.gameMod = modGone ? "" : gameMod;
        for (const auto& mod : d.mods)
            home.mods.emplace_back(mod.name, mod.folder == d.gameFolder / "mods" ? "Game folder" : "Plutonium");
        if (!s.launchError.empty())
        {
            home.line = Sentence(s.launchError);
            home.lineBad = true;
            home.link = "Log";
            home.linkAction = Action::OpenLog;
        }
        else if (!s.notice.empty())
        {
            home.line = s.notice;
            home.lineBad = true;
        }
        else if (s.keyPasted)
        {
            home.line = Beta::ValidLine(s.key);
            home.lineGood = true;
        }
        else if (modGone)
        {
            home.line = gameMod + " wasn't found";
            home.lineBad = true;
        }
        return home;
    }
}  // namespace Launcher
