#include "Setup.hpp"

#include <Windows.h>

#include <chrono>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <unordered_map>

#include "Components/AnimationPackLogic.hpp"
#include "Net.hpp"
#include "Platform.hpp"
#include "Setup/PackWriter.hpp"
#include "Setup/Zip.hpp"
#include "Utilities/Deflate.hpp"
#include "Utilities/Inflate.hpp"

namespace Launcher
{
    namespace Setup = IWXMVM::Setup;
    namespace Writer = IWXMVM::Setup::PackWriter;

    namespace
    {
        using Clock = std::chrono::steady_clock;

        // what each part takes, for the progress and the time left (measured on 2026-10-27, see the plan)
        constexpr double DOWNLOAD_SECONDS = 5.0;
        constexpr double FFMPEG_SECONDS = 15.0;
        constexpr double SECONDS_A_ZONE = 0.45;
        constexpr double PACK_SECONDS = 40.0;

        constexpr std::size_t OAT_ZIP_LIMIT = 64ull << 20;
        constexpr std::size_t ZONE_LIMIT = 1ull << 30;

        std::string Now()
        {
            SYSTEMTIME time{};
            GetLocalTime(&time);
            return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", time.wYear, time.wMonth, time.wDay, time.wHour,
                               time.wMinute, time.wSecond);
        }

        std::optional<std::vector<std::uint8_t>> ReadBytes(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return std::nullopt;
            const auto size = static_cast<std::size_t>(file.tellg());
            std::vector<std::uint8_t> bytes(size);
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
                return std::nullopt;
            return bytes;
        }
    }  // namespace

    struct SetupJob::Work
    {
        Options options;
        Detection detection;
        Plan plan;
        mutable std::mutex mutex;
        SetupStatus status;
        std::atomic<bool> cancelled = false;
        std::mutex logMutex;
        std::ofstream log;
        Clock::time_point started = Clock::now();

        // the plan's seconds: all of it, and those done when each part starts
        double plannedSeconds = 0.0;
        double ffmpegStart = 0.0, zonesStart = 0.0, packStart = 0.0;

        void Log(std::string_view line)
        {
            std::lock_guard lock(logMutex);
            log << line << '\n';
            log.flush();
        }

        // the progress, given the plan's seconds done
        void Progress(SetupStage stage, std::size_t done, std::size_t total, double secondsDone)
        {
            std::lock_guard lock(mutex);
            status.stage = stage;
            status.done = done;
            status.total = total;
            status.fraction = plannedSeconds > 0.0 ? static_cast<float>(std::min(1.0, secondsDone / plannedSeconds))
                                                   : 0.0f;
            // the plan's rate, corrected by how fast it has gone so far once a little of it is done
            const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
            const double left = std::max(0.0, plannedSeconds - secondsDone);
            status.secondsLeft = secondsDone > plannedSeconds * 0.05 && secondsDone > 0.0
                                     ? left * std::clamp(elapsed / secondsDone, 0.5, 3.0)
                                     : left;
        }

        void Fail(const std::string& error)
        {
            Log("ERROR " + error);
            std::lock_guard lock(mutex);
            if (status.error.empty())
                status.error = error;
        }

        bool Failed() const
        {
            std::lock_guard lock(mutex);
            return !status.error.empty();
        }

        bool Download();
        bool DownloadFFmpeg();
        bool BuildZones();
        // the Change Map menu's pictures, copied out of the game's zones with the Unlinker
        bool CopyPictures();
        // the weapons' textures only Rezurrection's files have, into one file the game searches
        bool WriteWeaponTextures();
        bool WritePack();
        // runs the Linker with its output in the log; false when it couldn't start or was cancelled
        bool RunLinker(const std::wstring& commandLine, const std::filesystem::path& folder);
    };

    SetupJob::SetupJob(const Options& options, const Detection& detection, Plan plan)
        : work(std::make_unique<Work>())
    {
        work->options = options;
        work->detection = detection;
        work->plan = plan;
        std::error_code error;
        std::filesystem::create_directories(LogFile().parent_path(), error);
        work->log.open(LogFile(), std::ios::trunc);
        thread = std::thread([this] { Run(); });
    }

    SetupJob::~SetupJob()
    {
        Cancel();
        if (thread.joinable())
            thread.join();
    }

    void SetupJob::Cancel()
    {
        work->cancelled = true;
    }

    SetupStatus SetupJob::Status() const
    {
        std::lock_guard lock(work->mutex);
        return work->status;
    }

    std::filesystem::path SetupJob::LogFile() const
    {
        return work->options.home / "logs" / "setup.log";
    }

    void SetupJob::Run()
    {
        auto& w = *work;
        w.Log(std::format("Iris Studio setup, {}", Now()));
        w.Log("game: " + Platform::Utf8(w.detection.gameFolder));
        w.Log("into: " + Platform::Utf8(w.options.storage));

        // the plan's seconds, for the progress
        std::size_t zonesToBuild = 0;
        try
        {
            if (w.plan.zones)
                for (const auto& zone : LoadCatalogs().zones)
                    if (Setup::ChooseLevel(w.detection.gameZones, zone))
                        ++zonesToBuild;
        }
        catch (const std::exception& e)
        {
            w.Fail(e.what());
        }
        w.ffmpegStart = w.plan.download ? DOWNLOAD_SECONDS : 0.0;
        w.zonesStart = w.ffmpegStart + (w.plan.ffmpeg ? FFMPEG_SECONDS : 0.0);
        w.packStart = w.zonesStart + zonesToBuild * SECONDS_A_ZONE;
        w.plannedSeconds = w.packStart + (w.plan.pack ? PACK_SECONDS : 0.0);

        try
        {
            bool ok = !w.Failed();
            if (ok && w.plan.download)
                ok = w.Download();
            if (ok && !w.cancelled && w.plan.ffmpeg)
                ok = w.DownloadFFmpeg();
            if (ok && !w.cancelled && w.plan.zones)
                ok = w.BuildZones();
            if (ok && !w.cancelled && w.plan.zones)
                ok = w.CopyPictures();
            if (ok && !w.cancelled && w.plan.zones)
                ok = w.WriteWeaponTextures();
            if (ok && !w.cancelled && w.plan.pack)
                ok = w.WritePack();

            if (ok && !w.cancelled)
            {
                // what this setup was built from, so a game update or a new catalog shows as out of date
                const auto& catalogs = LoadCatalogs();
                SetupRecord record;
                record.fingerprint = w.detection.fingerprint;
                record.characters = catalogs.charactersHash;
                record.animations = catalogs.animationsHash;
                record.gameFolder = Platform::Utf8(w.detection.gameFolder);
                // the zone files the character zones were built from: a copy of the game that has them the same
                // doesn't build them again
                std::vector<std::string> levels;
                for (const auto& zone : catalogs.zones)
                    if (const auto level = Setup::ChooseLevel(w.detection.gameZones, zone);
                        level && std::find(levels.begin(), levels.end(), *level) == levels.end())
                        levels.push_back(*level);
                record.zoneFiles = Setup::FilesBuiltFrom(w.detection.gameZones, levels, w.detection.zoneFiles);
                {
                    std::lock_guard lock(w.mutex);
                    record.zones = w.status.zonesBuilt;
                    record.animationsWritten = w.status.animationsWritten;
                }
                if (!record.Save(w.options.storage / "setup.json"))
                    w.Fail("setup.json couldn't be written in the mod's folder");
            }
        }
        catch (const std::exception& e)
        {
            w.Fail(e.what());
        }

        const double seconds = std::chrono::duration<double>(Clock::now() - w.started).count();
        w.Log(std::format("{} after {:.1f} s", w.cancelled ? "cancelled" : w.Failed() ? "failed" : "done", seconds));
        std::lock_guard lock(w.mutex);
        w.status.cancelled = w.cancelled;
        w.status.finished = true;
        w.status.stage = SetupStage::Done;
    }

    int RunSetup(const Options& options)
    {
        const auto settings = Settings::Load(options.home / "launcher.json");
        const auto detection = Detect(options, settings);
        if (!detection.GameFound() || !detection.catalogError.empty())
            return 2;
        SetupJob::Plan plan;
        plan.rebuild = options.rebuild;
        plan.zones = options.rebuild || !detection.ZonesReady();
        plan.pack = options.rebuild || !detection.PackReady();
        plan.download = plan.zones && detection.linker.empty() && (options.download || !options.oatZip.empty());
        plan.ffmpeg = detection.ffmpeg.empty() && (options.download || !options.ffmpegZip.empty());
        if (!plan.zones && !plan.pack && !plan.ffmpeg)
            return 0;
        SetupJob job(options, detection, plan);
        const auto started = Clock::now();
        while (true)
        {
            const auto status = job.Status();
            if (status.finished)
                return status.error.empty() && !status.cancelled ? 0 : 1;
            if (options.cancelAfter > 0.0f &&
                std::chrono::duration<double>(Clock::now() - started).count() > options.cancelAfter)
                job.Cancel();
            Sleep(200);
        }
    }

    bool SetupJob::Work::Download()
    {
        Progress(SetupStage::Download, 0, 0, 0.0);
        std::optional<std::vector<std::uint8_t>> zip;
        std::string error;
        if (!options.oatZip.empty())
        {
            // a zip on disk instead (tests): only the Linker's own hash is checked below
            Log("OpenAssetTools from " + Platform::Utf8(options.oatZip));
            zip = ReadBytes(options.oatZip);
            if (!zip)
                error = "it couldn't be read";
        }
        else
        {
            Log("downloading " + std::string(OAT_ZIP_URL));
            const std::wstring url(OAT_ZIP_URL.begin(), OAT_ZIP_URL.end());
            zip = Net::Get(url, OAT_ZIP_LIMIT, cancelled, error, [this](std::uint64_t done, std::uint64_t total) {
                {
                    std::lock_guard lock(mutex);
                    status.downloaded = done;
                    status.downloadSize = total;
                }
                if (total > 0)
                    Progress(SetupStage::Download, 0, 0, DOWNLOAD_SECONDS * done / total);
            });
            if (zip && Platform::Hex(Platform::Sha256(*zip)) != OAT_ZIP_SHA256)
            {
                Fail("oat-windows.zip isn't the file expected (its SHA-256 differs); nothing of it was used");
                return false;
            }
        }
        if (!zip)
        {
            if (!cancelled)
                Fail("OpenAssetTools couldn't be downloaded: " + error);
            return false;
        }
        const auto entries = Setup::Zip::Read(*zip, OAT_ZIP_LIMIT);
        if (!entries)
        {
            Fail("oat-windows.zip couldn't be unpacked");
            return false;
        }
        // unpacked next to where it goes, then moved there whole
        const auto folder = options.home / "tools" / ("oat-" + std::string(OAT_VERSION));
        auto unpacking = folder;
        unpacking += ".part";
        std::error_code fileError;
        std::filesystem::remove_all(unpacking, fileError);
        for (const auto& entry : *entries)
        {
            if (!Setup::Zip::IsSafeName(entry.name))
            {
                Fail("oat-windows.zip has a file outside its folder: " + entry.name);
                return false;
            }
            if (entry.name.ends_with('/'))
                continue;
            if (!Platform::WriteFile(unpacking / Platform::FromUtf8(entry.name), entry.data))
            {
                Fail("OpenAssetTools couldn't be written to " + Platform::Utf8(folder));
                return false;
            }
        }
        const auto hash = Platform::Sha256File(unpacking / "Linker.exe");
        if (!hash || Platform::Hex(*hash) != LINKER_SHA256)
        {
            std::filesystem::remove_all(unpacking, fileError);
            Fail("the Linker in oat-windows.zip isn't the one expected");
            return false;
        }
        std::filesystem::remove_all(folder, fileError);
        std::filesystem::rename(unpacking, folder, fileError);
        if (fileError)
        {
            Fail("OpenAssetTools couldn't be put in " + Platform::Utf8(folder));
            return false;
        }
        detection.linker = folder / "Linker.exe";
        Log(std::format("OpenAssetTools {} in {}", OAT_VERSION, Platform::Utf8(folder)));
        Progress(SetupStage::Download, 0, 0, DOWNLOAD_SECONDS);
        return true;
    }

    bool SetupJob::Work::DownloadFFmpeg()
    {
        Progress(SetupStage::FFmpeg, 0, 0, ffmpegStart);
        std::optional<std::vector<std::uint8_t>> zip;
        std::string error;
        if (!options.ffmpegZip.empty())
        {
            // a zip on disk instead (tests): its SHA-256 isn't checked
            Log("FFmpeg from " + Platform::Utf8(options.ffmpegZip));
            zip = ReadBytes(options.ffmpegZip);
            if (!zip)
                error = "it couldn't be read";
        }
        else
        {
            Log("downloading " + std::string(FFMPEG_ZIP_URL));
            const std::wstring url(FFMPEG_ZIP_URL.begin(), FFMPEG_ZIP_URL.end());
            zip = Net::Get(url, FFMPEG_ZIP_BYTES + (1 << 20), cancelled, error,
                           [this](std::uint64_t done, std::uint64_t total) {
                               {
                                   std::lock_guard lock(mutex);
                                   status.downloaded = done;
                                   status.downloadSize = total;
                               }
                               if (total > 0)
                                   Progress(SetupStage::FFmpeg, 0, 0, ffmpegStart + FFMPEG_SECONDS * done / total);
                           });
            if (zip && Platform::Hex(Platform::Sha256(*zip)) != FFMPEG_ZIP_SHA256)
            {
                Fail("FFmpeg's zip isn't the file expected (its SHA-256 differs); nothing of it was used");
                return false;
            }
        }
        if (!zip)
        {
            if (!cancelled)
                Fail("FFmpeg couldn't be downloaded: " + error);
            return false;
        }
        const auto entries = Setup::Zip::Read(*zip, 512ull << 20,
                                              [](std::string_view name) { return name.ends_with("/bin/ffmpeg.exe"); });
        zip.reset();
        if (!entries || entries->size() != 1)
        {
            Fail("FFmpeg's zip couldn't be unpacked");
            return false;
        }
        const auto path = options.home / "ffmpeg.exe";
        if (!Platform::WriteFile(path, entries->front().data))
        {
            Fail("ffmpeg.exe couldn't be written to " + Platform::Utf8(options.home));
            return false;
        }
        Log(std::format("ffmpeg.exe ({:.1f} MB) in {}", entries->front().data.size() / 1e6, Platform::Utf8(path)));
        Progress(SetupStage::FFmpeg, 0, 0, zonesStart);
        return true;
    }

    bool SetupJob::Work::RunLinker(const std::wstring& commandLine, const std::filesystem::path& folder)
    {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inherit, 0))
            return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = write;
        startup.hStdError = write;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        std::wstring command = commandLine;
        const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                            folder.c_str(), &startup, &process);
        CloseHandle(write);
        if (!started)
        {
            CloseHandle(read);
            Fail(std::format("the Linker couldn't be started (error {})", GetLastError()));
            return false;
        }

        std::string output;
        const auto drain = [&] {
            DWORD available = 0;
            while (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available > 0)
            {
                char buffer[4096];
                DWORD got = 0;
                if (!ReadFile(read, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || got == 0)
                    break;
                output.append(buffer, got);
            }
        };
        bool stopped = false;
        while (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT)
        {
            drain();
            if (cancelled)
            {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 5000);
                stopped = true;
                break;
            }
        }
        drain();
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(read);
        Log(output);
        Log(std::format("(Linker exit {})", exitCode));
        return !stopped;
    }

    bool SetupJob::Work::BuildZones()
    {
        const auto& catalogs = LoadCatalogs();
        if (detection.linker.empty())
        {
            Fail("OpenAssetTools' Linker isn't there");
            return false;
        }

        // the zones to build, by level in the order zones.py links them
        std::map<std::string, std::vector<const Setup::CharacterZone*>> byLevel;
        std::size_t skipped = 0;
        const auto zonesFolder = options.storage / "zones";
        std::error_code error;
        for (const auto& zone : catalogs.zones)
        {
            // (a weapon's from the first of its levels the game has)
            const auto level = Setup::ChooseLevel(detection.gameZones, zone);
            if (!level)
            {
                ++skipped;
                continue;
            }
            const bool built = std::filesystem::file_size(zonesFolder / Platform::FromUtf8(zone.name + ".ff"), error) > 0 &&
                               !error;
            if (plan.rebuild || detection.zonesStale || !built)
                byLevel[*level].push_back(&zone);
        }
        std::size_t total = 0;
        for (const auto& [_, zones] : byLevel)
            total += zones.size();
        Log(std::format("zones: {} to build in {} levels, {} of levels this game hasn't", total, byLevel.size(),
                        skipped));
        if (total == 0)
            return true;

        // the Linker's work folder: the definitions, its output
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const auto folder = std::filesystem::path(temp) / std::format(L"IrisStudio-setup-{}", GetCurrentProcessId());
        std::filesystem::remove_all(folder, error);
        std::filesystem::create_directories(folder / "zone_source", error);
        std::filesystem::create_directories(folder / "zone_out", error);
        std::filesystem::create_directories(zonesFolder, error);

        std::size_t done = 0, built = 0, failed = 0;
        Progress(SetupStage::Zones, 0, total, zonesStart);
        for (const auto& [level, zones] : byLevel)
        {
            if (cancelled)
                break;
            const auto started = Clock::now();
            std::wstring command = Setup::QuoteArgument(detection.linker.wstring()) + L" --no-color --base-folder " +
                                   Setup::QuoteArgument(folder.wstring()) + L" --output-folder " +
                                   Setup::QuoteArgument((folder / "zone_out").wstring());
            // (kept: a range-for over the optional's contents would outlive it)
            const auto loads = Setup::LevelLoads(detection.gameZones, level).value_or(std::vector<std::filesystem::path>{});
            for (const auto& load : loads)
                command += L" --load " + Setup::QuoteArgument((detection.gameFolder / load).make_preferred().wstring());
            for (const auto* zone : zones)
            {
                const auto definition = Setup::ZoneDefinition(zone->name, zone->models, zone->assets);
                Platform::WriteFile(folder / "zone_source" / Platform::FromUtf8(zone->name + ".zone"), definition);
                std::filesystem::remove(folder / "zone_out" / Platform::FromUtf8(zone->name + ".ff"), error);
                command += L" " + Setup::QuoteArgument(Platform::FromUtf8(zone->name).wstring());
            }
            Log(std::format("--- {}: {} zones", level, zones.size()));
            Log(Platform::Utf8(std::filesystem::path(command)));
            if (!RunLinker(command, detection.linker.parent_path()))
                break;

            std::size_t levelBuilt = 0;
            for (const auto* zone : zones)
            {
                const auto name = Platform::FromUtf8(zone->name + ".ff");
                const auto output = folder / "zone_out" / name;
                if (std::filesystem::is_regular_file(output, error) &&
                    CopyFileW(output.c_str(), (zonesFolder / name).c_str(), FALSE))
                {
                    ++levelBuilt;
                }
                else
                {
                    Log("   not built: " + zone->name);
                    ++failed;
                }
            }
            built += levelBuilt;
            done += zones.size();
            Log(std::format("{}: {}/{} zones in {:.1f} s", level, levelBuilt, zones.size(),
                            std::chrono::duration<double>(Clock::now() - started).count()));
            {
                std::lock_guard lock(mutex);
                status.zonesBuilt = built;
                status.zonesFailed = failed;
            }
            Progress(SetupStage::Zones, done, total, zonesStart + done * SECONDS_A_ZONE);
        }
        std::filesystem::remove_all(folder, error);
        Log(std::format("{} of {} character zones written to {}", built, total, Platform::Utf8(zonesFolder)));
        if (failed > 0 && !cancelled)
        {
            Fail(std::format("{} of {} character zones weren't built", failed, total));
            return false;
        }
        return true;
    }

    bool SetupJob::Work::CopyPictures()
    {
        const auto& catalogs = LoadCatalogs();
        const auto picturesFolder = options.storage / std::string(Setup::MENU_PICTURES_FOLDER);
        std::error_code error;
        std::map<std::string, std::vector<std::string>> byZone;
        for (const auto& picture : catalogs.pictures)
            if (detection.gameZones.HasCommon(picture.zone) &&
                (plan.rebuild || detection.zonesStale ||
                 std::filesystem::file_size(picturesFolder / Platform::FromUtf8(picture.image + ".dds"), error) == 0 ||
                 error))
                byZone[picture.zone].push_back(picture.image);
        if (byZone.empty())
            return true;
        const auto unlinker = detection.linker.parent_path() / "Unlinker.exe";
        if (!std::filesystem::is_regular_file(unlinker, error))
        {
            Fail("OpenAssetTools' Unlinker isn't there");
            return false;
        }
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const auto folder = std::filesystem::path(temp) / std::format(L"IrisStudio-pictures-{}", GetCurrentProcessId());
        std::filesystem::create_directories(picturesFolder, error);
        std::size_t copied = 0, wanted = 0;
        for (const auto& [zone, images] : byZone)
        {
            if (cancelled)
                break;
            wanted += images.size();
            std::filesystem::remove_all(folder, error);
            const auto zoneFile = detection.gameFolder / "zone" / "Common" / Platform::FromUtf8(zone + ".ff");
            const std::wstring command = Setup::QuoteArgument(unlinker.wstring()) +
                                         L" --no-color --include-assets image --image-format DDS -o " +
                                         Setup::QuoteArgument((folder / "out").wstring()) + L" " +
                                         Setup::QuoteArgument(zoneFile.wstring());
            Log(std::format("--- the Change Map menu's pictures of {}", zone));
            if (!RunLinker(command, unlinker.parent_path()))
                break;
            for (const auto& image : images)
            {
                const auto name = Platform::FromUtf8(image + ".dds");
                if (CopyFileW((folder / "out" / "images" / name).c_str(), (picturesFolder / name).c_str(), FALSE))
                    ++copied;
                else
                    Log("   not copied: " + image);
            }
        }
        std::filesystem::remove_all(folder, error);
        Log(std::format("{} of {} of the Change Map menu's pictures written to {}", copied, wanted,
                        Platform::Utf8(picturesFolder)));
        if (copied < wanted && !cancelled)
        {
            Fail(std::format("{} of {} of the Change Map menu's pictures weren't copied", wanted - copied, wanted));
            return false;
        }
        return true;
    }

    bool SetupJob::Work::WriteWeaponTextures()
    {
        const auto& textures = LoadCatalogs().weaponTextures;
        const auto& target = detection.weaponTexturesFile;
        if (target.empty() || (detection.weaponTexturesPresent && !plan.rebuild && !detection.zonesStale))
            return true;
        // (each file read whole, one at a time: the largest is 160 MB)
        const auto lower = [](std::string_view text) {
            std::string lowered(text);
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lowered;
        };
        std::set<std::string> wanted;
        for (const auto& image : textures.images)
            wanted.insert(lower("images/" + image + ".iwi"));
        std::vector<Setup::Zip::Entry> entries;
        for (const auto name : Setup::REZURRECTION_IWDS)
        {
            if (cancelled || wanted.empty())
                break;
            const auto archive = Platform::ReadFile(detection.gameFolder / "main" / std::string(name));
            if (!archive)
                continue;
            const std::span bytes(reinterpret_cast<const std::uint8_t*>(archive->data()), archive->size());
            auto found = Setup::Zip::Read(bytes, 64 << 20, [&](std::string_view entry) {
                return wanted.contains(lower(entry));
            });
            if (!found)
            {
                Log(std::format("{} doesn't read as a zip", name));
                continue;
            }
            for (auto& entry : *found)
            {
                wanted.erase(lower(entry.name));
                entries.push_back(std::move(entry));
            }
        }
        if (cancelled)
            return true;
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        const auto bytes = Setup::Zip::WriteStored(entries);
        const auto part = std::filesystem::path(target).concat(L".part");
        if (!Platform::WriteFile(part, std::span<const std::uint8_t>(bytes)) ||
            !MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            Fail("The weapons' textures couldn't be written to " + Platform::Utf8(target));
            return false;
        }
        Log(std::format("{} of {} weapon textures written to {} ({})", entries.size(), textures.images.size(),
                        Platform::Utf8(target), Setup::FormatSize(bytes.size())));
        return true;
    }

    bool SetupJob::Work::WritePack()
    {
        const auto& catalog = LoadCatalogs().animations;
        const auto zonesFolder = detection.gameFolder / "zone" / "Common";

        // the catalog's animations by their name in a zone ("name (zone)" is name too)
        std::unordered_map<std::string, std::vector<std::size_t>> byName;
        for (std::size_t i = 0; i < catalog.animations.size(); ++i)
        {
            const auto& key = catalog.animations[i].key;
            byName[key.substr(0, key.find(' '))].push_back(i);
        }
        // the zones to read: the catalog's that the game has, the biggest first so the threads end together
        struct ZoneFile
        {
            std::string name;
            std::filesystem::path path;
            std::uintmax_t size = 0;
        };
        std::vector<ZoneFile> zones;
        std::error_code error;
        for (const auto& zone : catalog.zones)
        {
            for (const auto& file : detection.gameZones.common)
            {
                if (_stricmp(file.c_str(), (zone + ".ff").c_str()) != 0)
                    continue;
                const auto path = zonesFolder / Platform::FromUtf8(file);
                zones.push_back({zone, path, std::filesystem::file_size(path, error)});
                break;
            }
        }
        std::sort(zones.begin(), zones.end(), [](const ZoneFile& a, const ZoneFile& b) { return a.size > b.size; });
        Log(std::format("pack: {} animations from {} zones", catalog.animations.size(), zones.size()));

        // each entry found is deflated at once and put in a file next to the pack, which is put together after
        const auto packPath = options.storage / "animations.pack";
        auto spillPath = packPath, partPath = packPath;
        spillPath += ".entries";
        partPath += ".part";
        std::filesystem::create_directories(options.storage, error);
        std::ofstream spill(spillPath, std::ios::binary | std::ios::trunc);
        if (!spill)
        {
            Fail("the pack couldn't be written in the mod's folder");
            return false;
        }
        struct Found
        {
            std::uint64_t offset = 0;
            std::uint32_t size = 0;
            std::uint32_t inflated = 0;
            bool claimed = false;
            bool written = false;
        };
        std::vector<Found> found(catalog.animations.size());
        std::mutex foundMutex;
        std::atomic<std::size_t> next = 0, zonesDone = 0;
        std::atomic<bool> writeFailed = false;

        const auto started = Clock::now();
        Progress(SetupStage::Pack, 0, zones.size(), packStart);
        const auto worker = [&] {
            for (std::size_t i; (i = next++) < zones.size() && !cancelled;)
            {
                std::optional<std::vector<std::uint8_t>> zone;
                {
                    const auto file = ReadBytes(zones[i].path);
                    if (file)
                        zone = Writer::InflateFastfile(*file, ZONE_LIMIT);
                }
                if (!zone)
                {
                    Log("   not a zone that reads: " + zones[i].name);
                }
                else
                {
                    Writer::ForEachAnimation(*zone, [&](std::string_view name, std::span<const std::uint8_t> entry) {
                        const auto it = byName.find(std::string(name));
                        if (it == byName.end())
                            return;
                        const auto hash = Writer::EntryHash(entry);
                        for (const auto index : it->second)
                        {
                            if (catalog.animations[index].hash != hash)
                                continue;
                            {
                                std::lock_guard lock(foundMutex);
                                if (found[index].claimed)
                                    return;
                                found[index].claimed = true;
                            }
                            const auto packed = IWXMVM::Deflate::Zlib(entry);
                            std::lock_guard lock(foundMutex);
                            auto& slot = found[index];
                            slot.offset = static_cast<std::uint64_t>(spill.tellp());
                            slot.size = static_cast<std::uint32_t>(packed.size());
                            slot.inflated = static_cast<std::uint32_t>(entry.size());
                            if (!spill.write(reinterpret_cast<const char*>(packed.data()),
                                             static_cast<std::streamsize>(packed.size())))
                                writeFailed = true;
                            slot.written = true;
                            return;
                        }
                    });
                }
                const auto doneNow = ++zonesDone;
                Progress(SetupStage::Pack, doneNow, zones.size(),
                         packStart + PACK_SECONDS * 0.9 * doneNow / std::max<std::size_t>(1, zones.size()));
            }
        };
        const auto threadCount = std::clamp<unsigned>(std::thread::hardware_concurrency(), 1, 3);
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < threadCount; ++t)
            threads.emplace_back(worker);
        for (auto& thread : threads)
            thread.join();
        spill.close();
        if (cancelled || writeFailed)
        {
            std::filesystem::remove(spillPath, error);
            if (writeFailed)
                Fail("the pack couldn't be written (is the disk full?)");
            return false;
        }

        // the pack: the header and index, then the entries, in the catalog's order
        std::vector<Writer::IndexRecord> records;
        std::vector<const Found*> order;
        std::size_t missing = 0;
        for (std::size_t i = 0; i < catalog.animations.size(); ++i)
        {
            if (!found[i].written)
            {
                ++missing;
                continue;
            }
            records.push_back({catalog.animations[i].key, found[i].size, found[i].inflated, catalog.animations[i].hash});
            order.push_back(&found[i]);
        }
        {
            std::ifstream entries(spillPath, std::ios::binary);
            std::ofstream pack(partPath, std::ios::binary | std::ios::trunc);
            const auto header = Writer::Header(records);
            pack.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
            std::vector<char> buffer;
            for (const auto* entry : order)
            {
                buffer.resize(entry->size);
                entries.seekg(static_cast<std::streamoff>(entry->offset));
                entries.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                pack.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            }
            if (!entries || !pack)
            {
                pack.close();
                entries.close();
                std::filesystem::remove(partPath, error);
                std::filesystem::remove(spillPath, error);
                Fail("the pack couldn't be written (is the disk full?)");
                return false;
            }
        }
        std::filesystem::remove(spillPath, error);

        // every entry read back against its hash, as animations.py checks its pack
        {
            const auto pack = ReadBytes(partPath);
            std::ifstream stream(partPath, std::ios::binary);
            std::size_t bad = 0;
            try
            {
                const auto index = IWXMVM::AnimationPackLogic::ReadIndex(stream);
                if (!pack || index.size() != records.size())
                    ++bad;
                for (const auto& record : records)
                {
                    const auto it = index.find(record.key);
                    if (it == index.end() || !pack || std::uint64_t{it->second.offset} + it->second.size > pack->size())
                    {
                        ++bad;
                        continue;
                    }
                    const auto inflated = IWXMVM::Inflate::Zlib(
                        std::span(*pack).subspan(it->second.offset, it->second.size), it->second.inflated);
                    if (!inflated || Writer::EntryHash(*inflated) != record.hash ||
                        !IWXMVM::AnimationPackLogic::ReadEntry(*inflated))
                        ++bad;
                }
            }
            catch (const std::exception&)
            {
                ++bad;
            }
            if (bad > 0)
            {
                std::filesystem::remove(partPath, error);
                Fail(std::format("the pack didn't read back ({} entries)", bad));
                return false;
            }
        }
        if (!MoveFileExW(partPath.c_str(), packPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(partPath, error);
            Fail("animations.pack couldn't be replaced: is the game running with it?");
            return false;
        }

        std::uint64_t inflated = 0;
        for (const auto& record : records)
            inflated += record.inflated;
        Log(std::format("{} animations, {:.1f} MB, packed to {:.1f} MB in {} ({} of the catalog's not in this game's "
                        "zones) in {:.1f} s",
                        records.size(), inflated / 1e6, std::filesystem::file_size(packPath, error) / 1e6,
                        Platform::Utf8(packPath), missing,
                        std::chrono::duration<double>(Clock::now() - started).count()));
        {
            std::lock_guard lock(mutex);
            status.animationsWritten = records.size();
            status.animationsMissing = missing;
        }
        Progress(SetupStage::Pack, zones.size(), zones.size(), plannedSeconds);
        return true;
    }
}  // namespace Launcher
