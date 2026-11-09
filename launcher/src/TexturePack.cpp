#include "TexturePack.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <ranges>
#include <set>
#include <condition_variable>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "nlohmann/json.hpp"

#include "Detect.hpp"
#include "Net.hpp"
#include "Platform.hpp"
#include "Setup/PackWriter.hpp"
#include "Setup/SetupLogic.hpp"
#include "Setup/TextureBuild.hpp"
#include "Setup/Zip.hpp"
#include "Utilities/Inflate.hpp"

namespace Launcher
{
    namespace TL = IWXMVM::Components::TextureLogic;
    namespace TB = IWXMVM::Setup::TextureBuild;
    using Clock = std::chrono::steady_clock;

    namespace
    {
        // The tools a build downloads the first time (AI1), pinned and checked by SHA-256 as OpenAssetTools is:
        // Real-ESRGAN's ncnn-vulkan runner (BSD-3; its exe and the OpenMP runtime it needs are kept), the 4xNomos8kSC
        // model in ncnn's form (CC-BY 4.0, Helaman; upscayl/custom-models at a fixed commit), and Microsoft's texconv
        // (MIT)
        struct Tool
        {
            const char* name;  // as the summary lists it
            const wchar_t* url;
            const char* sha256;  // of the download
            std::uint64_t bytes;
        };
        constexpr Tool ESRGAN_ZIP{"Real-ESRGAN",
                                  L"https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/"
                                  L"realesrgan-ncnn-vulkan-20220424-windows.zip",
                                  "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d", 45474481};
        constexpr const char* ESRGAN_EXE_SHA256 = "07e49f7cbb4ede01ae4dd4c399d3a7e5846e3d2085c3128eff881e55cb7b1a0c";
        constexpr Tool MODEL_BIN{"Nomos8kSC",
                                 L"https://raw.githubusercontent.com/upscayl/custom-models/"
                                 L"e8b59db9b4ccfc38ab9e335c2929d0ac110017c5/models/4xNomos8kSC.bin",
                                 "da16e3880d87b177b7c6b659bbd880f8a101b868eb9ebc08d69eaa6d3edc4517", 33424520};
        constexpr Tool MODEL_PARAM{"Nomos8kSC",
                                   L"https://raw.githubusercontent.com/upscayl/custom-models/"
                                   L"e8b59db9b4ccfc38ab9e335c2929d0ac110017c5/models/4xNomos8kSC.param",
                                   "4576ed5c2fc5fa250d3c3d585ef02248f26abdfc1867088078f501fe71e5d61e", 108039};
        constexpr Tool TEXCONV{"texconv", L"https://github.com/microsoft/DirectXTex/releases/download/may2026/texconv.exe",
                               "dcfdec10244e02cf5037fba089c55fb7e1326b1c8181742d77d15fa5cb5eef06", 966480};

        // compressions running beside the model (each holds a texture at its new size: the launcher is 32-bit)
        constexpr std::size_t COMPRESSIONS = 3;
        constexpr std::size_t ZONE_LIMIT = 1ull << 30;
        constexpr const char* CACHE_FILE = "texture-cache.json";

        std::string Lower(std::string_view text)
        {
            std::string lower(text);
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower;
        }

        std::optional<std::vector<std::uint8_t>> ReadBytes(const std::filesystem::path& path,
                                                           std::size_t limit = SIZE_MAX)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return std::nullopt;
            const auto size = static_cast<std::size_t>(file.tellg());
            if (size > limit)
                return std::nullopt;
            std::vector<std::uint8_t> bytes(size);
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
                return std::nullopt;
            return bytes;
        }

        template <typename T>
        T Get(const std::vector<std::uint8_t>& bytes, std::size_t at)
        {
            T value{};
            std::memcpy(&value, bytes.data() + at, sizeof(T));
            return value;
        }

        // ---- The iwds: their images, read a file at a time without loading the whole archive

        struct IwdEntry
        {
            std::filesystem::path iwd;
            std::uint32_t offset = 0;  // of its local header
            std::uint32_t compressed = 0, size = 0, crc = 0;
            std::uint16_t method = 0;
        };

        // every images/<name>.iwi of the iwds of the folders, the first folder winning and in a folder the iwd named
        // last (the game's search order)
        std::unordered_map<std::string, IwdEntry> IndexIwds(const std::vector<std::filesystem::path>& folders)
        {
            std::unordered_map<std::string, IwdEntry> found;
            for (const auto& folder : folders)
            {
                std::vector<std::filesystem::path> iwds;
                std::error_code error;
                for (const auto& file : std::filesystem::directory_iterator(folder, error))
                    if (Lower(file.path().extension().string()) == ".iwd")
                        iwds.push_back(file.path());
                std::ranges::sort(iwds, std::greater<>());
                for (const auto& iwd : iwds)
                {
                    std::ifstream file(iwd, std::ios::binary | std::ios::ate);
                    const auto size = static_cast<std::uint64_t>(file.tellg());
                    // the end of central directory record, within its comment's reach of the end
                    const auto tail = static_cast<std::size_t>(std::min<std::uint64_t>(size, 65536 + 22));
                    std::vector<std::uint8_t> end(tail);
                    file.seekg(static_cast<std::streamoff>(size - tail));
                    file.read(reinterpret_cast<char*>(end.data()), static_cast<std::streamsize>(tail));
                    std::optional<std::size_t> record;
                    for (std::size_t back = 22; back <= tail && !record; ++back)
                        if (Get<std::uint32_t>(end, tail - back) == 0x06054b50)
                            record = tail - back;
                    if (!record)
                        continue;
                    const auto directorySize = Get<std::uint32_t>(end, *record + 12);
                    const auto directoryOffset = Get<std::uint32_t>(end, *record + 16);
                    std::vector<std::uint8_t> directory(directorySize);
                    file.seekg(directoryOffset);
                    if (!file.read(reinterpret_cast<char*>(directory.data()), directorySize))
                        continue;
                    for (std::size_t at = 0; at + 46 <= directory.size() && Get<std::uint32_t>(directory, at) == 0x02014b50;)
                    {
                        const auto nameLength = Get<std::uint16_t>(directory, at + 28);
                        const auto extraLength = Get<std::uint16_t>(directory, at + 30);
                        const auto commentLength = Get<std::uint16_t>(directory, at + 32);
                        if (at + 46 + nameLength > directory.size())
                            break;
                        auto name = Lower(std::string_view(reinterpret_cast<const char*>(directory.data()) + at + 46,
                                                           nameLength));
                        std::ranges::replace(name, '\\', '/');
                        if (name.starts_with("images/") && name.ends_with(".iwi"))
                        {
                            name = name.substr(7, name.size() - 11);
                            if (!found.contains(name))
                                found[name] = {iwd, Get<std::uint32_t>(directory, at + 42),
                                               Get<std::uint32_t>(directory, at + 20), Get<std::uint32_t>(directory, at + 24),
                                               Get<std::uint32_t>(directory, at + 16), Get<std::uint16_t>(directory, at + 10)};
                        }
                        at += 46 + nameLength + extraLength + commentLength;
                    }
                }
            }
            return found;
        }

        // an entry's bytes as stored (at most limit of them), none when they can't be read
        std::optional<std::vector<std::uint8_t>> ReadStored(const IwdEntry& entry, std::size_t limit)
        {
            std::ifstream file(entry.iwd, std::ios::binary);
            std::array<std::uint8_t, 30> local{};
            file.seekg(entry.offset);
            if (!file.read(reinterpret_cast<char*>(local.data()), local.size()) ||
                std::memcmp(local.data(), "PK\x03\x04", 4) != 0)
                return std::nullopt;
            std::uint16_t nameLength, extraLength;
            std::memcpy(&nameLength, local.data() + 26, 2);
            std::memcpy(&extraLength, local.data() + 28, 2);
            file.seekg(entry.offset + 30 + nameLength + extraLength);
            std::vector<std::uint8_t> data(std::min<std::size_t>(entry.compressed, limit));
            if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
                return std::nullopt;
            return data;
        }

        std::optional<std::vector<std::uint8_t>> ReadEntry(const IwdEntry& entry)
        {
            auto data = ReadStored(entry, SIZE_MAX);
            if (!data)
                return std::nullopt;
            if (entry.method == 0)
                return data->size() == entry.size ? data : std::nullopt;
            return IWXMVM::Inflate::Raw(*data, entry.size);
        }

        // the first bytes of an entry, without inflating the rest (an .iwi has its smallest levels first)
        std::optional<std::vector<std::uint8_t>> ReadStart(const IwdEntry& entry, std::size_t count)
        {
            // (a deflated stream is a little longer than what it holds at the worst)
            const auto data = ReadStored(entry, count + count / 8 + 4096);
            if (!data)
                return std::nullopt;
            if (entry.method == 0)
                return data->size() >= count ? data : std::nullopt;
            return IWXMVM::Inflate::RawPrefix(*data, count);
        }

        std::optional<TL::IwiHeader> ReadHeader(const IwdEntry& entry)
        {
            const auto data = ReadStored(entry, 8192);
            if (!data)
                return std::nullopt;
            if (entry.method == 0)
                return TL::ParseIwiHeader(*data);
            const auto start = IWXMVM::Inflate::RawPrefix(*data, TL::IWI_HEADER_SIZE);
            return start ? TL::ParseIwiHeader(*start) : std::nullopt;
        }

        // ---- A zone's images: its image records ({..., name: -1}, 4 bytes, the name, 0) whose names the iwds have
        // (tools/t5-textures/zone.py: exactly OpenAssetTools' list of the textures a pack upscales)
        std::vector<std::string> ReadZoneImages(const std::filesystem::path& zone,
                                                const std::unordered_map<std::string, IwdEntry>& iwds)
        {
            const auto file = ReadBytes(zone, ZONE_LIMIT);
            if (!file)
                return {};
            const auto data = IWXMVM::Setup::PackWriter::InflateFastfile(*file, ZONE_LIMIT);
            if (!data)
                return {};
            std::set<std::string> names;
            for (std::size_t at = 0; at + 9 < data->size(); ++at)
            {
                if (Get<std::uint32_t>(*data, at) != 0xFFFFFFFF)
                    continue;
                auto end = at + 8;
                while (end < data->size() && end - at < 8 + 128 && (*data)[end] >= 0x21 && (*data)[end] <= 0x7E)
                    ++end;
                if (end == at + 8 || end >= data->size() || (*data)[end] != 0)
                    continue;
                auto name = Lower(std::string_view(reinterpret_cast<const char*>(data->data()) + at + 8, end - at - 8));
                if (iwds.contains(name))
                    names.insert(std::move(name));
            }
            return {names.begin(), names.end()};
        }

        // ---- PNG through Windows' imaging (the model reads and writes PNG)

        IWICImagingFactory* Wic()
        {
            thread_local IWICImagingFactory* factory = [] {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                IWICImagingFactory* made = nullptr;
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&made));
                return made;
            }();
            return factory;
        }

        // RGB (opaque) of an image as a PNG
        bool WritePng(const std::filesystem::path& path, const TB::Image& image)
        {
            const auto factory = Wic();
            if (!factory)
                return false;
            std::vector<std::uint8_t> bgr(std::size_t{image.width} * image.height * 3);
            for (std::size_t i = 0, j = 0; i < image.rgba.size(); i += 4, j += 3)
            {
                bgr[j] = image.rgba[i + 2];
                bgr[j + 1] = image.rgba[i + 1];
                bgr[j + 2] = image.rgba[i];
            }
            IWICStream* stream = nullptr;
            IWICBitmapEncoder* encoder = nullptr;
            IWICBitmapFrameEncode* frame = nullptr;
            bool written = false;
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(factory->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
                SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                SUCCEEDED(frame->SetSize(image.width, image.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
                format == GUID_WICPixelFormat24bppBGR &&
                SUCCEEDED(frame->WritePixels(image.height, image.width * 3, static_cast<UINT>(bgr.size()), bgr.data())) &&
                SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()))
                written = true;
            for (IUnknown* object : {static_cast<IUnknown*>(frame), static_cast<IUnknown*>(encoder),
                                     static_cast<IUnknown*>(stream)})
                if (object)
                    object->Release();
            return written;
        }

        std::optional<TB::Image> ReadPng(const std::filesystem::path& path)
        {
            const auto factory = Wic();
            if (!factory)
                return std::nullopt;
            IWICBitmapDecoder* decoder = nullptr;
            IWICBitmapFrameDecode* frame = nullptr;
            IWICFormatConverter* converter = nullptr;
            std::optional<TB::Image> image;
            UINT width = 0, height = 0;
            if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnDemand, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) &&
                SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                                0.0, WICBitmapPaletteTypeCustom)))
            {
                auto read = TB::MakeImage(width, height);
                if (SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(read.rgba.size()),
                                                    read.rgba.data())))
                    image = std::move(read);
            }
            for (IUnknown* object : {static_cast<IUnknown*>(converter), static_cast<IUnknown*>(frame),
                                     static_cast<IUnknown*>(decoder)})
                if (object)
                    object->Release();
            return image;
        }

        // a tool run without a window, its output dropped; false when it couldn't start, failed or was stopped
        // (its output into a file when one is given)
        bool RunHidden(const std::wstring& commandLine, const std::filesystem::path& folder,
                       const std::atomic<bool>& cancel, const std::filesystem::path& output = {})
        {
            SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
            const HANDLE nul = output.empty()
                                   ? CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr)
                                   : CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                                                 CREATE_ALWAYS, 0, nullptr);
            STARTUPINFOW startup{sizeof(startup)};
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = startup.hStdOutput = startup.hStdError = nul;
            PROCESS_INFORMATION process{};
            std::wstring command = commandLine;
            const bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                                CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                                                folder.c_str(), &startup, &process);
            CloseHandle(nul);
            if (!started)
                return false;
            bool stopped = false;
            while (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT)
                if (cancel)
                {
                    TerminateProcess(process.hProcess, 1);
                    WaitForSingleObject(process.hProcess, 5000);
                    stopped = true;
                    break;
                }
            DWORD exitCode = 1;
            GetExitCodeProcess(process.hProcess, &exitCode);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return !stopped && exitCode == 0;
        }

        std::wstring Quote(const std::filesystem::path& path)
        {
            return L"\"" + path.wstring() + L"\"";
        }

        // the dropdown a zone is listed in
        std::string GetGroup(std::string_view zone)
        {
            const auto kind = TL::GetPartKind(zone);
            return !kind || *kind == TL::PartKind::Guns   ? std::string()
                   : *kind == TL::PartKind::Multiplayer   ? std::string("Maps")
                   : *kind == TL::PartKind::Campaign      ? std::string("Campaign")
                                                          : std::string("Zombies");
        }

        std::string Drive(const std::filesystem::path& path)
        {
            const auto root = path.root_name().string();
            return root.empty() ? "the drive" : root;
        }

        std::optional<TL::PackIndex> LoadIndex(const std::filesystem::path& folder)
        {
            const auto text = Platform::ReadFile(folder / TL::INDEX_FILE);
            if (!text)
                return std::nullopt;
            try
            {
                return TL::IndexFromJson(nlohmann::json::parse(*text));
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        bool SaveIndex(const std::filesystem::path& folder, const TL::PackIndex& index)
        {
            return Platform::WriteFile(folder / TL::INDEX_FILE, TL::ToJson(index).dump(1));
        }
    }  // namespace

    // ---- The work

    struct TexturePack::Work
    {
        Options options;
        std::filesystem::path gameFolder;
        std::filesystem::path root, built, tools, workFolder;

        mutable std::mutex mutex;
        std::thread thread;
        std::mutex logMutex;
        std::ofstream log;
        std::size_t limit = 0;  // textures a map at most (tests)

        void Log(std::string_view text)
        {
            std::lock_guard lock(logMutex);
            if (!log.is_open())
            {
                std::error_code error;
                std::filesystem::create_directories(options.home / "logs", error);
                log.open(options.home / "logs" / "textures.log", std::ios::app);
            }
            SYSTEMTIME time{};
            GetLocalTime(&time);
            log << std::format("{:02}:{:02}:{:02} {}\n", time.wHour, time.wMinute, time.wSecond, text);
            log.flush();
        }
        std::atomic<bool> cancel = false;
        std::atomic<bool> running = false;

        // what reading the game found (the job's until it is done, then read only)
        std::unordered_map<std::string, IwdEntry> iwds;
        std::unordered_map<std::string, TL::Texture> textures;
        // what a check found wrong in the pack (made again by a repair), how long that takes, and how many the model
        // got wrong twice
        std::set<std::string> wrongImages;
        std::string wrongTime;
        std::atomic<std::size_t> modelWrong = 0;
        std::atomic<bool> checking = false;
        bool repairing = false;
        struct Part
        {
            std::string zone;
            std::vector<std::string> images;
            std::uint64_t bytes2 = 0, bytes4 = 0;
        };
        std::vector<Part> parts;
        bool read = false;

        // under the mutex
        TexturePage page = TexturePage::Reading;
        std::string line;
        bool lineBad = false;
        float fraction = 0.0f;
        std::set<std::string> ticked;
        std::uint32_t scale = 4;
        TL::PackIndex index;
        std::vector<TL::PackIndex> imported;
        TL::Use use;
        std::string step, count, then;
        // an import's summary
        std::filesystem::path importFolder;
        std::string importName;
        std::vector<std::pair<std::filesystem::path, std::string>> importFits;  // file, image
        TL::ImportSummary importSummary;

        void Start(std::function<void()> job)
        {
            if (running)
                return;
            if (thread.joinable())
                thread.join();
            cancel = false;
            running = true;
            thread = std::thread([this, job = std::move(job)] {
                job();
                running = false;
            });
        }

        void SetLine(std::string text, bool bad = false)
        {
            if (bad)
                Log(text);
            std::lock_guard lock(mutex);
            line = std::move(text);
            lineBad = bad;
        }

        void LoadPacks()
        {
            auto loaded = LoadIndex(built).value_or(TL::PackIndex{std::string(), false, std::string(TL::MODEL_CREDIT)});
            std::vector<TL::PackIndex> others;
            std::error_code error;
            for (const auto& folder : std::filesystem::directory_iterator(root / TL::IMPORTED_FOLDER, error))
                if (auto pack = LoadIndex(folder.path()); pack && pack->imported)
                    others.push_back(std::move(*pack));
            TL::Use loadedUse;
            if (const auto text = Platform::ReadFile(root / TL::USE_FILE))
                try
                {
                    loadedUse = TL::UseFromJson(nlohmann::json::parse(*text));
                }
                catch (const std::exception&)
                {
                }
            std::lock_guard lock(mutex);
            index = std::move(loaded);
            imported = std::move(others);
            use = std::move(loadedUse);
        }

        bool HasPack() const
        {
            return !index.parts.empty() || !imported.empty();
        }

        void SaveUse()
        {
            // (written with its folder)
            Platform::WriteFile(root / TL::USE_FILE, TL::ToJson(use).dump(1));
        }

        // Reading the game: the iwds' images, each map's zone's images and their headers, kept in the launcher's folder
        // for the next time (by the files' sizes and times)
        void ReadGame()
        {
            SetLine("Reading the game's textures");
            iwds = IndexIwds({options.plutonium / "storage" / "t5" / "main", gameFolder / "main"});
            nlohmann::json cache;
            if (const auto text = Platform::ReadFile(options.home / CACHE_FILE))
                try
                {
                    cache = nlohmann::json::parse(*text);
                }
                catch (const std::exception&)
                {
                }
            if (!cache.is_object())
                cache = nlohmann::json::object();
            auto& zonesCache = cache["zones"];
            auto& headersCache = cache["headers"];
            if (!zonesCache.is_object())
                zonesCache = nlohmann::json::object();
            if (!headersCache.is_object())
                headersCache = nlohmann::json::object();

            // the maps (and the guns' zone) this game has
            std::vector<std::filesystem::path> zones;
            std::error_code error;
            for (const auto& file : std::filesystem::directory_iterator(gameFolder / "zone" / "Common", error))
            {
                const auto zone = Lower(file.path().stem().string());
                if (Lower(file.path().extension().string()) == ".ff" && TL::GetPartKind(zone))
                    zones.push_back(file.path());
            }
            std::ranges::sort(zones, [](const auto& a, const auto& b) {
                return TL::ListsBefore(Lower(a.stem().string()), Lower(b.stem().string()));
            });
            std::vector<Part> found;
            for (std::size_t i = 0; i < zones.size() && !cancel; ++i)
            {
                const auto zone = Lower(zones[i].stem().string());
                {
                    std::lock_guard lock(mutex);
                    line = std::format("Reading the game's maps: {} / {}", i + 1, zones.size());
                    fraction = 0.8f * i / zones.size();
                }
                Part part{zone};
                // a campaign or Zombies level's patch zone has some of its images
                for (const auto& file : {zones[i], zones[i].parent_path() / (zone + "_patch.ff")})
                {
                    if (!std::filesystem::exists(file, error))
                        continue;
                    const auto key = Lower(file.stem().string());
                    const auto size = std::filesystem::file_size(file, error);
                    const auto time = std::filesystem::last_write_time(file, error).time_since_epoch().count();
                    auto& entry = zonesCache[key];
                    if (!entry.is_object() || entry.value("size", std::uint64_t{0}) != size ||
                        entry.value("time", std::int64_t{0}) != time)
                        entry = {{"size", size}, {"time", time}, {"images", ReadZoneImages(file, iwds)}};
                    for (const auto& image : entry["images"])
                        part.images.push_back(image.get<std::string>());
                }
                std::ranges::sort(part.images);
                part.images.erase(std::unique(part.images.begin(), part.images.end()), part.images.end());
                found.push_back(std::move(part));
            }

            // the textures' headers
            std::set<std::string> wanted;
            for (const auto& part : found)
                wanted.insert(part.images.begin(), part.images.end());
            std::size_t done = 0;
            for (const auto& name : wanted)
            {
                if (cancel)
                    break;
                if (++done % 200 == 0)
                {
                    std::lock_guard lock(mutex);
                    line = std::format("Reading the game's textures: {} / {}", done, wanted.size());
                    fraction = 0.8f + 0.2f * done / wanted.size();
                }
                const auto& entry = iwds.at(name);
                auto& cached = headersCache[name];
                TL::IwiHeader header;
                if (cached.is_object() && cached.value("size", 0u) == entry.size && cached.value("crc", 0u) == entry.crc)
                {
                    header.format = cached.value("format", std::uint8_t{0});
                    header.flags = cached.value("flags", std::uint8_t{0});
                    header.width = cached.value("width", std::uint16_t{0});
                    header.height = cached.value("height", std::uint16_t{0});
                    header.depth = cached.value("depth", std::uint16_t{1});
                }
                else if (const auto read = ReadHeader(entry))
                {
                    header = *read;
                    cached = {{"size", entry.size},     {"crc", entry.crc},       {"format", header.format},
                              {"flags", header.flags},  {"width", header.width}, {"height", header.height},
                              {"depth", header.depth}};
                }
                else
                    continue;
                textures[name] = {header, Platform::Utf8(entry.iwd.filename()), entry.size, entry.crc};
            }
            if (!cancel)
                Platform::WriteFile(options.home / CACHE_FILE, cache.dump());

            for (auto& part : found)
            {
                part.bytes2 = TL::MakePlan({part.images}, textures, 2, {}).bytes;
                part.bytes4 = TL::MakePlan({part.images}, textures, 4, {}).bytes;
            }
            parts = std::move(found);
            read = true;
            Log(std::format("read {} maps, {} textures, {} images in the iwds", parts.size(), textures.size(),
                            iwds.size()));
            LoadPacks();
            {
                std::lock_guard lock(mutex);
                line.clear();
                page = HasPack() ? TexturePage::Done : TexturePage::Summary;
                if (ticked.empty())
                    for (const auto zone : {std::string("mp_nuked"), std::string(TL::GUNS_ZONE)})
                        if (std::ranges::find(parts, zone, &Part::zone) != parts.end() && !index.parts.contains(zone))
                            ticked.insert(zone);
            }
            // a pack of an older build is checked by itself (AR1); those without a window ask for it
            if (options.buildTextures.empty() && options.importTextures.empty() && !options.checkTextures &&
                !options.repairTextures)
                CheckPack(false);
        }

        std::unordered_map<std::string, TL::Built> BuiltImages() const
        {
            std::unordered_map<std::string, TL::Built> done;
            for (const auto& [name, image] : index.images)
                done[name] = {image.scale, image.source.size, image.source.crc, wrongImages.contains(name)};
            return done;
        }

        TL::Plan PlanOf(const std::set<std::string>& zones, std::uint32_t planScale) const
        {
            std::vector<std::vector<std::string>> images;
            for (const auto& part : parts)
                if (zones.contains(part.zone))
                    images.push_back(part.images);
            return TL::MakePlan(images, textures, planScale, BuiltImages());
        }

        // the tools missing, with their size
        std::vector<const Tool*> MissingTools() const
        {
            std::vector<const Tool*> missing;
            if (!std::filesystem::exists(tools / "realesrgan-ncnn-vulkan.exe"))
                missing.push_back(&ESRGAN_ZIP);
            if (!std::filesystem::exists(tools / "models" / "4xNomos8kSC.bin") ||
                !std::filesystem::exists(tools / "models" / "4xNomos8kSC.param"))
            {
                missing.push_back(&MODEL_BIN);
                missing.push_back(&MODEL_PARAM);
            }
            if (!std::filesystem::exists(tools / "texconv.exe"))
                missing.push_back(&TEXCONV);
            return missing;
        }

        // the tools downloaded and checked; false (with the line saying why) when one couldn't be
        bool GetTools()
        {
            for (const auto tool : MissingTools())
            {
                {
                    std::lock_guard lock(mutex);
                    step = std::format("Downloading {}", tool->name);
                    count.clear();
                }
                std::string error;
                const auto bytes = Net::Get(tool->url, static_cast<std::size_t>(tool->bytes) + (1 << 20), cancel, error,
                                            [&](std::uint64_t done, std::uint64_t total) {
                                                std::lock_guard lock(mutex);
                                                count = std::format("{} / {}", IWXMVM::Setup::FormatSize(done),
                                                                    IWXMVM::Setup::FormatSize(total ? total : tool->bytes));
                                            });
                if (!bytes)
                {
                    if (!cancel)
                        SetLine(std::format("{} couldn't be downloaded: {}", tool->name, error), true);
                    return false;
                }
                if (Platform::Hex(Platform::Sha256(*bytes)) != tool->sha256)
                {
                    SetLine(std::format("{} isn't the file expected (its SHA-256 differs); nothing of it was used",
                                        tool->name),
                            true);
                    return false;
                }
                bool written = true;
                if (tool == &ESRGAN_ZIP)
                {
                    const auto entries = IWXMVM::Setup::Zip::Read(*bytes, 64ull << 20, [](std::string_view name) {
                        return name == "realesrgan-ncnn-vulkan.exe" || name == "vcomp140.dll";
                    });
                    written = entries && entries->size() == 2;
                    for (const auto& entry : entries.value_or(std::vector<IWXMVM::Setup::Zip::Entry>{}))
                    {
                        if (entry.name == "realesrgan-ncnn-vulkan.exe" &&
                            Platform::Hex(Platform::Sha256(entry.data)) != ESRGAN_EXE_SHA256)
                            written = false;
                        written = written && Platform::WriteFile(tools / Platform::FromUtf8(entry.name), entry.data);
                    }
                }
                else
                {
                    const auto file = tool == &TEXCONV     ? tools / "texconv.exe"
                                      : tool == &MODEL_BIN ? tools / "models" / "4xNomos8kSC.bin"
                                                           : tools / "models" / "4xNomos8kSC.param";
                    written = Platform::WriteFile(file, *bytes);
                }
                if (!written)
                {
                    SetLine(std::format("{} couldn't be put in {}", tool->name, Platform::Utf8(tools)), true);
                    return false;
                }
            }
            return true;
        }

        // What the model is given: an RGB normal for a normal map, opaque, the largest halved first so their 4x stays
        // at 4,096
        static TB::Image MakeModelInput(TL::Kind kind, const TB::Image& source)
        {
            auto input = kind == TL::Kind::Normal ? TB::ToNormal(source) : TB::Opaque(source);
            for (auto s = TL::GetScale(source.width, source.height, 4); s < 4; s *= 2)
                input = TB::Halve(input, false);
            return input;
        }

        std::wstring ModelCommand(const std::filesystem::path& in, const std::filesystem::path& out) const
        {
            return std::format(L"{} -i {} -o {} -n 4xNomos8kSC -s 4 -m {} -t 256 -f png",
                               Quote(tools / "realesrgan-ncnn-vulkan.exe"), Quote(in), Quote(out),
                               Quote(tools / "models"));
        }

        // A texture through the model alone: a run of the model can give back black, whole or from a tile on (46 normal
        // maps of one pack, a card busy or lost for a moment)
        std::optional<TB::Image> RunModelAlone(const TB::Image& input)
        {
            const auto in = workFolder / "alone-in", out = workFolder / "alone-out";
            std::error_code error;
            for (const auto& path : {in, out})
            {
                std::filesystem::remove_all(path, error);
                std::filesystem::create_directories(path, error);
            }
            const auto pad = TB::GetPad(input.width, input.height);
            if (!WritePng(in / "0000.png", TB::WrapPad(input, pad)) || !RunHidden(ModelCommand(in, out), tools, cancel))
                return std::nullopt;
            const auto read = ReadPng(out / "0000.png");
            if (!read || read->width != (input.width + pad * 2) * 4)
                return std::nullopt;
            return TB::Crop(*read, pad * 4, pad * 4, input.width * 4, input.height * 4);
        }

        // What the model gave back kept only when it is the same picture: wrong, the texture goes through it alone, and
        // when that is wrong too it is resized plainly; none then
        std::optional<TB::Image> CheckModelOutput(const std::string& name, TL::Kind kind, const TB::Image& source,
                                                  TB::Image output)
        {
            const auto input = MakeModelInput(kind, source);
            if (!TB::IsWrong(kind, TB::Compare(input, output)))
                return output;
            Log(name + ": the model gave back something else, tried again alone");
            auto again = RunModelAlone(input);
            if (again && !TB::IsWrong(kind, TB::Compare(input, *again)))
                return again;
            Log(name + ": the model gave back something else twice, resized plainly");
            ++modelWrong;
            return std::nullopt;
        }

        // A texture made: the game's decoded, through the model (its 4x, given) or resized, at the pack's scale,
        // compressed by texconv into the pack. What it took, none when it failed.
        struct Made
        {
            std::string name;
            std::uint32_t scale = 0, width = 0, height = 0;
            std::uint8_t format = 0;
            std::uint64_t bytes = 0;
        };
        std::optional<Made> Finish(const std::string& name, const TL::Texture& texture, const TB::Image& source,
                                   std::optional<TB::Image> modelled, std::uint32_t packScale, std::size_t slot)
        {
            const auto& header = texture.header;
            const auto kind = TL::GetKind(name, header.format);
            const bool normal = kind == TL::Kind::Normal;
            const auto finalScale = TL::GetScale(header.width, header.height, packScale);
            const auto width = header.width * finalScale, height = header.height * finalScale;
            TB::Image result;
            if (modelled)
            {
                // the model's RGB (a normal map's as an RGB normal), halved down to the scale
                result = normal ? TB::FromNormal(*modelled) : TB::Opaque(std::move(*modelled));
                while (result.width > width)
                    result = TB::Halve(result, normal);
            }
            else
                result = normal ? TB::FromNormal(TB::Resize(TB::ToNormal(source), width, height))
                                : TB::Resize(source, width, height);
            if (result.width != width || result.height != height)
            {
                Log(std::format("{}: {} x {} came out, not {} x {}", name, result.width, result.height, width, height));
                return std::nullopt;
            }
            // alpha resized plainly; a DXT1 without it kept opaque
            if (!normal)
                result = TB::HasAlpha(source) ? TB::WithAlpha(std::move(result), TB::Resize(source, width, height))
                                              : TB::Opaque(std::move(result));

            // (texconv names its output after its input: into a folder of its own)
            const auto folder = workFolder / std::format("c{}", slot);
            std::error_code error;
            std::filesystem::create_directories(folder / "out", error);
            const auto input = folder / "t.dds";
            const std::vector<TL::Level> levels{{result.width, result.height, result.rgba}};
            bool written = false;
            {
                // (straight into the work folder: a temporary file renamed was now and then held by a scan)
                const auto dds = TL::WriteDds(0, levels);
                std::ofstream stream(input, std::ios::binary | std::ios::trunc);
                written = static_cast<bool>(
                    stream.write(reinterpret_cast<const char*>(dds.data()), static_cast<std::streamsize>(dds.size())));
            }
            if (!written)
            {
                Log(name + ": couldn't be written for texconv");
                return std::nullopt;
            }
            result = {};
            const auto format = header.format == TL::FORMAT_DXT1   ? L"BC1_UNORM"
                                : header.format == TL::FORMAT_DXT3 ? L"BC2_UNORM"
                                                                   : L"BC3_UNORM";
            // (a few tries: a file just written is now and then held by a scan as texconv opens it)
            bool compressed = false;
            for (int attempt = 0; attempt < 3 && !compressed && !cancel; ++attempt)
            {
                if (attempt > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                // (-sepalpha: without it texconv's levels lose the colour under low alpha, which a weapon's or wet
                // ground's mask leaves on show)
                compressed = RunHidden(std::format(L"{} -nologo -y -dx9 -sepalpha -m 0 -f {} -o {} {}",
                                                   Quote(tools / "texconv.exe"), format, Quote(folder / "out"),
                                                   Quote(input)),
                                       folder, cancel, folder / "texconv.txt");
            }
            if (!compressed)
            {
                if (!cancel)
                {
                    // its last line says why
                    auto said = Platform::ReadFile(folder / "texconv.txt").value_or("");
                    while (!said.empty() && (said.back() == '\n' || said.back() == '\r'))
                        said.pop_back();
                    Log(std::format("{}: texconv failed ({})", name, said.substr(said.rfind('\n') + 1)));
                }
                return std::nullopt;
            }
            // from the game's size down a spec map's levels are the game's own, a colour map's alpha too
            if (!KeepGameLevels(name, kind, header.format, folder / "out" / "t.dds"))
            {
                Log(name + ": the game's levels couldn't be kept");
                return std::nullopt;
            }
            const auto file = built / Platform::FromUtf8(name + ".dds");
            std::filesystem::remove(input, error);
            std::filesystem::rename(folder / "out" / "t.dds", file, error);
            if (error)
            {
                Log(name + ": couldn't be put in the pack");
                return std::nullopt;
            }
            const auto bytes = std::filesystem::file_size(file, error);
            return Made{name, finalScale, width, height, header.format, bytes};
        }

        // whether an image takes levels or alpha from the game's file
        static bool KeepsGameLevels(TL::Kind kind, std::uint8_t format)
        {
            return kind == TL::Kind::Spec || (kind == TL::Kind::Colour && format != TL::FORMAT_DXT1);
        }

        // The game's levels of a spec map, or the alpha of a colour map, put in a pack's file. (A texture of the game
        // with no levels of its own keeps those texconv made below its size.) A few tries, as for texconv: a file just
        // written is now and then held by a scan.
        bool KeepGameLevels(const std::string& name, TL::Kind kind, std::uint8_t format,
                            const std::filesystem::path& file)
        {
            if (!KeepsGameLevels(kind, format))
                return true;
            const auto game = ReadEntry(iwds.at(name));
            const auto iwi = game ? TL::ParseIwi(*game) : std::nullopt;
            if (!iwi)
            {
                Log(name + ": the game's file couldn't be read");
                return false;
            }
            for (int attempt = 0; attempt < 8 && !cancel; ++attempt)
            {
                if (attempt > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const auto made = ReadBytes(file);
                if (!made)
                    continue;
                const auto kept =
                    kind == TL::Kind::Spec ? TL::KeepGameLevels(*made, *iwi) : TL::KeepGameAlpha(*made, *iwi);
                if (!kept)
                {
                    Log(name + ": its levels aren't the game's sizes or format");
                    return false;
                }
                if (Platform::WriteFile(file, *kept))
                    return true;
            }
            return false;
        }

        static std::optional<std::vector<std::uint8_t>> ReadBytes(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return std::nullopt;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                return std::nullopt;
            return bytes;
        }

        // Building the parts ticked, map by map; Stop keeps what is made, Build again carries on
        void Build(std::vector<std::string> zones, std::uint32_t packScale)
        {
            const auto started = Clock::now();
            std::error_code error;
            std::filesystem::create_directories(built, error);
            std::filesystem::create_directories(tools / "models", error);
            std::filesystem::remove_all(workFolder, error);
            std::filesystem::create_directories(workFolder, error);
            if (!GetTools())
            {
                std::lock_guard lock(mutex);
                page = TexturePage::Summary;
                return;
            }
            auto total = PlanOf({zones.begin(), zones.end()}, packScale);
            // (a repair makes only what the check found wrong, not what a pack never had)
            const auto onlyWrong = [this](TL::Plan& plan) {
                if (repairing)
                    std::erase_if(plan.images, [this](const std::string& name) { return !wrongImages.contains(name); });
            };
            onlyWrong(total);
            std::size_t doneAll = 0;
            double modelSeconds = 0.0;
            {
                std::lock_guard lock(mutex);
                index.model = std::string(TL::MODEL_CREDIT);
            }
            for (std::size_t z = 0; z < zones.size() && !cancel; ++z)
            {
                const auto& zone = zones[z];
                const auto part = std::ranges::find(parts, zone, &Part::zone);
                if (part == parts.end())
                    continue;
                auto plan = TL::MakePlan({part->images}, textures, packScale, BuiltImages());
                onlyWrong(plan);
                if (limit && plan.images.size() > limit)
                    plan.images.resize(limit);
                Log(std::format("{} at {}x: {} textures to build, {} built before, {} shared", zone, packScale,
                                plan.images.size(), plan.built, plan.shared));
                {
                    // (a map built at 4x and again at 2x keeps its 4x textures)
                    std::lock_guard lock(mutex);
                    auto& entry = index.parts[zone];
                    entry.scale = std::max(entry.scale, packScale);
                }
                // in batches: the model's textures of a batch in one run, then each finished on the processor while the
                // next batch runs on the card
                std::deque<std::future<std::optional<Made>>> finishing;
                std::size_t donePart = 0;
                const auto collect = [&](bool all) {
                    while (!finishing.empty() && (all || finishing.size() >= COMPRESSIONS))
                    {
                        const auto made = finishing.front().get();
                        finishing.pop_front();
                        ++donePart;
                        ++doneAll;
                        if (made)
                        {
                            std::lock_guard lock(mutex);
                            const auto& texture = textures.at(made->name);
                            auto& image = index.images[made->name];
                            image.scale = made->scale;
                            image.format = made->format;
                            image.width = made->width;
                            image.height = made->height;
                            image.bytes = made->bytes;
                            image.source = {made->scale, texture.size, texture.crc};
                            image.build = TL::BUILD_VERSION;
                            wrongImages.erase(made->name);
                            if (std::ranges::find(image.parts, zone) == image.parts.end())
                                image.parts.push_back(zone);
                        }
                        const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                        const auto left = doneAll ? elapsed / doneAll * (total.images.size() - doneAll)
                                                  : TL::EstimateSeconds(total, packScale);
                        std::lock_guard lock(mutex);
                        step = repairing ? std::string("Repairing")
                                         : std::format("Upscaling {}", TL::GetPartName(zone));
                        count = repairing ? std::format("{} / {}", IWXMVM::Setup::FormatCount(doneAll),
                                                        IWXMVM::Setup::FormatCount(total.images.size()))
                                          : std::format("{} / {}", donePart, plan.images.size());
                        fraction = total.images.empty() ? 1.0f : float(doneAll) / total.images.size();
                        // (short: "2 more maps · about 36 minutes left")
                        const auto more = repairing ? 0 : zones.size() - z - 1;
                        then = more ? std::format("{} more {} \xC2\xB7 {} left", more, more == 1 ? "map" : "maps",
                                                  TL::FormatDuration(left))
                                    : std::format("{} left", TL::FormatDuration(left));
                        then[0] = static_cast<char>(std::toupper(then[0]));
                    }
                };
                if (!repairing)
                {
                    std::lock_guard lock(mutex);
                    step = std::format("Upscaling {}", TL::GetPartName(zone));
                    count = std::format("0 / {}", plan.images.size());
                }
                std::size_t slot = 0;
                // A batch: its textures decoded and the model's inputs written (wrap-padded, a normal map as an RGB
                // normal, the largest halved first so their 4x stays at 4,096), into a folder of its own of two used in
                // turn, so the card runs the next batch's model while the processor finishes the last
                struct Batch
                {
                    std::filesystem::path in, out;
                    std::vector<std::pair<std::string, TB::Image>> sources;
                    std::vector<std::tuple<std::size_t, std::uint32_t, std::uint32_t, std::uint32_t>> inputs;
                    std::future<bool> model;
                };
                const auto prepare = [&](std::size_t first, int folder) {
                    auto batch = std::make_unique<Batch>();
                    batch->in = workFolder / std::format("in{}", folder);
                    batch->out = workFolder / std::format("out{}", folder);
                    for (const auto& path : {batch->in, batch->out})
                    {
                        std::filesystem::remove_all(path, error);
                        std::filesystem::create_directories(path, error);
                    }
                    const auto last = std::min(plan.images.size(), first + TL::MODEL_BATCH);
                    for (std::size_t i = first; i < last && !cancel; ++i)
                    {
                        const auto& name = plan.images[i];
                        const auto bytes = ReadEntry(iwds.at(name));
                        const auto iwi = bytes ? TL::ParseIwi(*bytes) : std::nullopt;
                        if (!iwi || iwi->levels.empty())
                        {
                            Log(name + ": couldn't be read from " + Platform::Utf8(iwds.at(name).iwd.filename()));
                            ++donePart;
                            ++doneAll;
                            continue;
                        }
                        const auto& level = iwi->levels.front();
                        auto source = TB::DecodeDxt(iwi->header.format, level.width, level.height, level.data);
                        const auto kind = TL::GetKind(name, iwi->header.format);
                        if (TL::UsesModel(kind))
                        {
                            const auto input = MakeModelInput(kind, source);
                            const auto pad = TB::GetPad(input.width, input.height);
                            const auto at = batch->sources.size();
                            if (WritePng(batch->in / std::format("{:04}.png", at), TB::WrapPad(input, pad)))
                                batch->inputs.emplace_back(at, pad, input.width, input.height);
                        }
                        batch->sources.emplace_back(name, std::move(source));
                    }
                    const auto command = ModelCommand(batch->in, batch->out);
                    const bool none = batch->inputs.empty() || cancel;
                    batch->model = std::async(std::launch::async, [this, command, none] {
                        return none || RunHidden(command, tools, cancel);
                    });
                    return batch;
                };

                auto next = plan.images.empty() ? nullptr : prepare(0, 0);
                for (std::size_t first = 0; next && !cancel; first += TL::MODEL_BATCH)
                {
                    auto batch = std::move(next);
                    const auto modelStarted = Clock::now();
                    const bool modelled = batch->model.get();
                    modelSeconds += std::chrono::duration<double>(Clock::now() - modelStarted).count();
                    if (!modelled && !cancel)
                    {
                        SetLine("The model didn't run: Real-ESRGAN needs a graphics card with Vulkan", true);
                        break;
                    }
                    if (first + TL::MODEL_BATCH < plan.images.size() && !cancel)
                        next = prepare(first + TL::MODEL_BATCH, int(first / TL::MODEL_BATCH + 1) % 2);
                    for (std::size_t s = 0; s < batch->sources.size() && !cancel; ++s)
                    {
                        std::optional<TB::Image> upscaled;
                        const auto& inputs = batch->inputs;
                        if (const auto input = std::ranges::find(inputs, s, [](const auto& t) { return std::get<0>(t); });
                            input != inputs.end())
                        {
                            const auto [index, pad, width, height] = *input;
                            const auto read = ReadPng(batch->out / std::format("{:04}.png", s));
                            if (!read || read->width != (width + pad * 2) * 4)
                            {
                                Log(batch->sources[s].first + ": the model's output is missing or not 4x");
                                ++donePart;
                                ++doneAll;
                                continue;
                            }
                            const auto& [sourceName, sourceImage] = batch->sources[s];
                            upscaled = CheckModelOutput(sourceName,
                                                        TL::GetKind(sourceName, textures.at(sourceName).header.format),
                                                        sourceImage,
                                                        TB::Crop(*read, pad * 4, pad * 4, width * 4, height * 4));
                        }
                        collect(false);
                        const auto name = batch->sources[s].first;
                        finishing.push_back(std::async(std::launch::async,
                                                       [this, name, source = std::move(batch->sources[s].second),
                                                        upscaled = std::move(upscaled), packScale, slot]() mutable {
                                                           return Finish(name, textures.at(name), source,
                                                                         std::move(upscaled), packScale, slot);
                                                       }));
                        slot = (slot + 1) % (COMPRESSIONS * 4);
                    }
                    collect(true);
                    // a map is used once it is done; the index is saved as it goes
                    std::lock_guard lock(mutex);
                    TL::CountParts(index);
                    SaveIndex(built, index);
                }
                // (a batch prepared when stopped: its model is let go)
                if (next)
                    next->model.wait();
                collect(true);
                // the map's textures built before (for another map) are its too
                std::lock_guard lock(mutex);
                for (const auto& name : part->images)
                    if (auto image = index.images.find(name); image != index.images.end() &&
                                                              std::ranges::find(image->second.parts, zone) ==
                                                                  image->second.parts.end())
                        image->second.parts.push_back(zone);
                TL::CountParts(index);
                if (index.parts[zone].images == 0)
                    index.parts.erase(zone);
                SaveIndex(built, index);
            }
            std::filesystem::remove_all(workFolder, error);
            Log(std::format("{} {} textures in {:.0f} s (waiting for the model {:.0f} s, {} resized plainly in its "
                            "place)",
                            cancel ? "stopped after" : "built", doneAll,
                            std::chrono::duration<double>(Clock::now() - started).count(), modelSeconds,
                            modelWrong.load()));
            std::lock_guard lock(mutex);
            page = HasPack() ? TexturePage::Done : TexturePage::Summary;
            if (!cancel && !lineBad && !repairing)
                line = std::format("Built in {}", TL::FormatDuration(
                                                      std::chrono::duration<double>(Clock::now() - started).count())
                                                      .substr(6));
            for (const auto& zone : zones)
                ticked.erase(zone);
        }

        // An image of a pack beside the game's, at the check's level of both; none when they can't be compared (a
        // pack's file without that level)
        struct CheckLevels
        {
            TB::Image game, pack;
        };
        std::optional<CheckLevels> ReadCheckLevels(const std::string& name, const IwdEntry& entry,
                                                   const std::filesystem::path& file)
        {
            const auto start = ReadHeader(entry);
            if (!start || !TL::IsDxt(start->format))
                return std::nullopt;
            const auto& header = *start;
            const auto level = TL::GetCheckLevel(TL::GetKind(name, header.format), header.width, header.height,
                                                 !(header.flags & TL::IWI_NO_MIPS));
            auto width = std::uint32_t{header.width}, height = std::uint32_t{header.height};
            for (std::uint32_t i = 0; i < level; ++i)
            {
                width = std::max(1u, width / 2);
                height = std::max(1u, height / 2);
            }
            const auto through = static_cast<std::size_t>(TL::GetIwiBytesThrough(header, level));
            const auto levelBytes = static_cast<std::size_t>(TL::GetLevelBytes(header.format, width, height));
            const auto bytes = ReadStart(entry, through);
            if (!bytes || bytes->size() < through)
                return std::nullopt;
            const auto levelData = std::span<const std::uint8_t>(*bytes).subspan(through - levelBytes, levelBytes);
            auto game = TB::DecodeDxt(header.format, width, height, levelData);

            std::ifstream stream(file, std::ios::binary);
            std::vector<std::uint8_t> head(TL::DDS_HEADER_SIZE);
            if (!stream.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size())))
                return std::nullopt;
            std::error_code error;
            const auto info = TL::ParseDdsHeader(head, std::filesystem::file_size(file, error));
            const auto place = info ? TL::FindDdsLevel(*info, width, height) : std::nullopt;
            if (!place)
                return std::nullopt;
            std::vector<std::uint8_t> data(place->bytes);
            stream.seekg(static_cast<std::streamoff>(place->offset));
            if (!stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
                return std::nullopt;
            TB::Image pack;
            if (info->format)
                pack = TB::DecodeDxt(info->format, width, height, data);
            else
            {
                pack = TB::MakeImage(width, height);
                for (std::size_t i = 0; i + 3 < data.size() && i + 3 < pack.rgba.size(); i += 4)
                {
                    pack.rgba[i] = data[i + 2];
                    pack.rgba[i + 1] = data[i + 1];
                    pack.rgba[i + 2] = data[i];
                    pack.rgba[i + 3] = data[i + 3];
                }
            }
            return CheckLevels{std::move(game), std::move(pack)};
        }

        std::optional<TB::Difference> CheckImage(const std::string& name, const IwdEntry& entry,
                                                 const std::filesystem::path& file)
        {
            const auto levels = ReadCheckLevels(name, entry, file);
            return levels ? std::optional(TB::Compare(levels->game, levels->pack)) : std::nullopt;
        }

        // Imported packs can't be made again: their broken images are left out, so the game's texture draws (AU1); a
        // pack that only looks different stays as it is
        void CheckImported()
        {
            std::vector<TL::PackIndex> packs;
            {
                std::lock_guard lock(mutex);
                packs = imported;
            }
            for (auto& pack : packs)
            {
                const auto folder = root / TL::IMPORTED_FOLDER / Platform::FromUtf8(pack.name);
                std::vector<std::string> broken;
                bool changed = false;
                for (auto& [name, image] : pack.images)
                {
                    const auto entry = iwds.find(name);
                    if (image.build >= TL::BUILD_VERSION || entry == iwds.end() || cancel)
                        continue;
                    const auto levels =
                        ReadCheckLevels(name, entry->second, folder / Platform::FromUtf8(name + ".dds"));
                    if (levels && TB::IsBroken(levels->game, levels->pack))
                        broken.push_back(name);
                    image.build = TL::BUILD_VERSION;
                    changed = true;
                }
                for (const auto& name : broken)
                    pack.images.erase(name);
                if (changed)
                    SaveIndex(folder, pack);
                if (!broken.empty())
                    Log(std::format("{}: {} wrong images left out", pack.name, broken.size()));
            }
            std::lock_guard lock(mutex);
            imported = std::move(packs);
        }

        // how long a repair of what is wrong takes, by the measured rates
        void SetRepairTime()
        {
            TL::Plan plan;
            std::uint32_t scaleOfPack = 2;
            for (const auto& name : wrongImages)
            {
                const auto texture = textures.find(name);
                const auto image = index.images.find(name);
                if (texture == textures.end() || image == index.images.end())
                    continue;
                const auto& header = texture->second.header;
                scaleOfPack = std::max(scaleOfPack, image->second.scale);
                plan.images.push_back(name);
                plan.bytes += TL::GetPackBytes(header.format, header.width, header.height, image->second.scale);
                const std::uint64_t texels = std::uint64_t{header.width} * header.height;
                (TL::UsesModel(TL::GetKind(name, header.format)) ? plan.modelTexels : plan.plainTexels) += texels;
            }
            wrongTime = TL::FormatDuration(TL::EstimateSeconds(plan, scaleOfPack));
        }

        // The images of older builds checked against the game's, a few threads at a time: the right ones marked so
        // and never looked at again, the wrong ones listed to be made again (the rest of the pack stays as it is)
        void CheckPack(bool logAll)
        {
            const auto started = Clock::now();
            std::vector<std::string> names;
            {
                std::lock_guard lock(mutex);
                for (const auto& [name, image] : index.images)
                    if (image.build < TL::BUILD_VERSION && iwds.contains(name))
                        names.push_back(name);
            }
            if (names.empty() && !logAll && std::ranges::none_of(imported, [](const TL::PackIndex& pack) {
                    return std::ranges::any_of(pack.images | std::views::values, [](const TL::PackImage& image) {
                        return image.build < TL::BUILD_VERSION;
                    });
                }))
                return;
            checking = true;
            std::atomic<std::size_t> next = 0;
            std::vector<std::optional<TB::Difference>> differences(names.size());
            std::vector<std::thread> threads;
            for (unsigned t = 0; t < std::max(2u, std::thread::hardware_concurrency() / 2); ++t)
                threads.emplace_back([&] {
                    for (std::size_t i; (i = next++) < names.size() && !cancel;)
                    {
                        differences[i] = CheckImage(names[i], iwds.at(names[i]),
                                                    built / Platform::FromUtf8(names[i] + ".dds"));
                        if (i % 500 == 0)
                            SetLine(std::format("Checking {} / {}", IWXMVM::Setup::FormatCount(i),
                                                IWXMVM::Setup::FormatCount(names.size())));
                    }
                });
            for (auto& thread : threads)
                thread.join();
            // right colour maps take the game's alpha now, a matter of bytes: nothing else of them changes
            std::atomic<std::size_t> nextKept = 0;
            threads.clear();
            for (unsigned t = 0; t < std::max(2u, std::thread::hardware_concurrency() / 2) && !logAll; ++t)
                threads.emplace_back([&] {
                    for (std::size_t i; (i = nextKept++) < names.size() && !cancel;)
                    {
                        const auto format = index.images.at(names[i]).format;
                        const auto kind = TL::GetKind(names[i], format);
                        if (kind == TL::Kind::Colour && differences[i] && !TB::IsWrong(kind, *differences[i]) &&
                            !KeepGameLevels(names[i], kind, format, built / Platform::FromUtf8(names[i] + ".dds")))
                            differences[i].reset();
                        if (i % 500 == 0)
                            SetLine(std::format("Checking {} / {}", IWXMVM::Setup::FormatCount(i),
                                                IWXMVM::Setup::FormatCount(names.size())));
                    }
                });
            for (auto& thread : threads)
                thread.join();
            std::map<std::string, std::size_t> wrongKinds;
            std::size_t right = 0, unknown = 0;
            std::vector<std::string> lines;
            {
                std::lock_guard lock(mutex);
                wrongImages.clear();
                for (std::size_t i = 0; i < names.size(); ++i)
                {
                    if (!differences[i])
                    {
                        ++unknown;
                        continue;
                    }
                    const auto kind = TL::GetKind(names[i], index.images.at(names[i]).format);
                    const bool wrong = TB::IsWrong(kind, *differences[i]);
                    if (wrong)
                    {
                        wrongImages.insert(names[i]);
                        ++wrongKinds[std::string(TL::GetName(kind))];
                    }
                    else
                    {
                        index.images.at(names[i]).build = TL::BUILD_VERSION;
                        ++right;
                    }
                    if (wrong || logAll)
                        lines.push_back(std::format("{}: {} {}, colour {:.1f}, alpha {:.1f}, under alpha {:.1f}",
                                                    wrong ? "wrong" : "right", names[i], TL::GetName(kind),
                                                    differences[i]->colour, differences[i]->alpha,
                                                    differences[i]->underAlpha));
                }
                if (!cancel && !logAll)
                    SaveIndex(built, index);
                SetRepairTime();
            }
            if (!logAll)
                CheckImported();
            SetLine("");
            checking = false;
            std::string kinds;
            for (const auto& [kind, count] : wrongKinds)
                kinds += std::format(" {} {}", kind, count);
            Log(std::format("checked {} textures of an older build in {:.0f} s: {} right, {} wrong ({} ), {} not "
                            "compared",
                            names.size(), std::chrono::duration<double>(Clock::now() - started).count(), right,
                            wrongImages.size(), kinds, unknown));
            for (const auto& text : lines)
                Log(text);
        }

        // What a check found made again, part by part at each part's scale: the rest of the pack isn't touched
        void Repair()
        {
            const auto started = Clock::now();
            std::map<std::uint32_t, std::vector<std::string>> byScale;
            {
                std::lock_guard lock(mutex);
                for (const auto& [zone, part] : index.parts)
                    if (std::ranges::find(parts, zone, &Part::zone) != parts.end())
                        byScale[part.scale].push_back(zone);
            }
            repairing = true;
            // (again for what a file held by a scan kept from being finished)
            for (int pass = 0; pass < 3 && !cancel && !wrongImages.empty(); ++pass)
                for (const auto& [partScale, zones] : byScale)
                    if (!cancel)
                        Build(zones, partScale);
            repairing = false;
            std::lock_guard lock(mutex);
            SetRepairTime();
            if (!cancel && !lineBad && wrongImages.empty())
                line = std::format("Repaired in {}", TL::FormatDuration(
                                                         std::chrono::duration<double>(Clock::now() - started).count())
                                                         .substr(6));
        }

        // An imported folder read: each <image>.dds checked (AH2), nothing copied yet
        void ReadImport(std::filesystem::path folder)
        {
            TL::ImportSummary summary;
            std::vector<std::pair<std::filesystem::path, std::string>> fits;
            std::error_code error;
            std::size_t seen = 0;
            for (const auto& file : std::filesystem::recursive_directory_iterator(folder, error))
            {
                if (cancel)
                    break;
                if (!file.is_regular_file() || Lower(file.path().extension().string()) != ".dds")
                    continue;
                const auto name = Lower(file.path().stem().string());
                std::optional<TL::IwiHeader> game;
                if (const auto texture = textures.find(name); texture != textures.end())
                    game = texture->second.header;
                else if (const auto entry = iwds.find(name); entry != iwds.end())
                    game = ReadHeader(entry->second);
                std::ifstream stream(file.path(), std::ios::binary);
                std::vector<std::uint8_t> head(TL::DDS_HEADER_SIZE);
                stream.read(reinterpret_cast<char*>(head.data()), head.size());
                head.resize(static_cast<std::size_t>(stream.gcount()));
                const auto dds = TL::ParseDdsHeader(head, std::filesystem::file_size(file.path(), error));
                auto verdict = TL::CheckImport(dds, game);
                // (AU1: what an old build broke stays out; a pack that only looks different is the pack's)
                if (const auto entry = iwds.find(name); verdict == TL::ImportVerdict::Fits && entry != iwds.end())
                    if (const auto levels = ReadCheckLevels(name, entry->second, file.path());
                        levels && TB::IsBroken(levels->game, levels->pack))
                        verdict = TL::ImportVerdict::Wrong;
                if (verdict == TL::ImportVerdict::Fits)
                {
                    ++summary.fits;
                    summary.bytes += std::filesystem::file_size(file.path(), error);
                    summary.overBuilt += index.images.contains(name);
                    fits.emplace_back(file.path(), name);
                }
                else
                    ++summary.left[verdict];
                if (++seen % 100 == 0)
                    SetLine(std::format("Checking {} files", seen));
            }
            std::lock_guard lock(mutex);
            line.clear();
            importFolder = folder;
            importName = TL::GetImportName(Platform::Utf8(folder.filename()));
            importFits = std::move(fits);
            importSummary = summary;
            page = TexturePage::Import;
        }

        void CopyImport()
        {
            std::filesystem::path folder;
            std::vector<std::pair<std::filesystem::path, std::string>> fits;
            std::string name;
            {
                std::lock_guard lock(mutex);
                fits = importFits;
                name = importName;
            }
            // a name of its own
            for (int i = 2; std::filesystem::exists(root / TL::IMPORTED_FOLDER / Platform::FromUtf8(name)); ++i)
                name = TL::GetImportName(std::format("{} {}", importName, i));
            folder = root / TL::IMPORTED_FOLDER / Platform::FromUtf8(name);
            TL::PackIndex pack{name, true, "imported"};
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            for (std::size_t i = 0; i < fits.size() && !cancel; ++i)
            {
                const auto& [file, image] = fits[i];
                if (i % 25 == 0)
                    SetLine(std::format("Copying {} / {}", i, fits.size()));
                const auto target = folder / Platform::FromUtf8(image + ".dds");
                std::filesystem::copy_file(file, target, std::filesystem::copy_options::overwrite_existing, error);
                if (error)
                    continue;
                std::ifstream stream(target, std::ios::binary);
                std::vector<std::uint8_t> head(TL::DDS_HEADER_SIZE);
                stream.read(reinterpret_cast<char*>(head.data()), head.size());
                const auto bytes = std::filesystem::file_size(target, error);
                const auto dds = TL::ParseDdsHeader(head, bytes);
                const auto texture = textures.find(image);
                std::uint32_t imageScale = 1;
                if (dds && texture != textures.end())
                    imageScale = TL::GetImportScale(*dds, texture->second.header);
                else if (dds)
                    if (const auto entry = iwds.find(image); entry != iwds.end())
                        if (const auto header = ReadHeader(entry->second))
                            imageScale = TL::GetImportScale(*dds, *header);
                pack.images[image] = {imageScale, dds ? dds->format : std::uint8_t{0}, dds ? dds->width : 0u,
                                      dds ? dds->height : 0u, bytes, {}, {}, TL::BUILD_VERSION};
            }
            SaveIndex(folder, pack);
            LoadPacks();
            std::lock_guard lock(mutex);
            line = cancel ? std::string() : std::format("{} imported", name);
            page = TexturePage::Done;
        }
    };

    TexturePack::TexturePack(const Options& options, std::filesystem::path gameFolder)
        : work(std::make_shared<Work>())
    {
        work->options = options;
        work->gameFolder = std::move(gameFolder);
        work->root = options.storage / "textures";
        work->built = work->root / TL::BUILT_FOLDER;
        work->tools = options.home / "tools" / "textures";
        work->workFolder = options.home / "texture-work";
        work->Start([w = work.get()] { w->ReadGame(); });
    }

    TexturePack::~TexturePack()
    {
        work->cancel = true;
        if (work->thread.joinable())
            work->thread.join();
    }

    bool TexturePack::Busy() const
    {
        return work->running;
    }

    TextureView TexturePack::View() const
    {
        const auto& w = *work;
        std::lock_guard lock(w.mutex);
        TextureView view;
        view.page = w.page;
        view.line = w.line;
        view.lineBad = w.lineBad;
        view.fraction = w.fraction;
        if (w.page == TexturePage::Reading || !w.read)
            return view;
        view.scale = w.scale;

        if (w.page == TexturePage::Summary)
        {
            for (const auto& part : w.parts)
            {
                const bool builtPart = w.index.parts.contains(part.zone);
                view.parts.push_back({part.zone, TL::GetPartName(part.zone),
                                      TL::FormatGigabytes(w.scale >= 4 ? part.bytes4 : part.bytes2),
                                      w.ticked.contains(part.zone), builtPart, GetGroup(part.zone)});
            }
            const auto plan = w.PlanOf(w.ticked, w.scale);
            view.model = "4xNomos8kSC (Helaman, CC-BY 4.0)";
            std::uint64_t downloadBytes = 0;
            std::vector<std::string> names;
            for (const auto tool : w.MissingTools())
            {
                downloadBytes += tool->bytes;
                if (std::ranges::find(names, tool->name) == names.end())
                    names.emplace_back(tool->name);
            }
            if (names.empty())
                view.downloads = "None";
            else
            {
                for (std::size_t i = 0; i < names.size(); ++i)
                    view.downloads += (i ? ", " : "") + names[i];
                view.downloads += ", " + IWXMVM::Setup::FormatSize(downloadBytes);
            }
            const auto free = Platform::FreeSpace(w.root);
            view.space = std::format("{} on {}, {} free", TL::FormatGigabytes(plan.bytes), Drive(w.root),
                                     TL::FormatGigabytes(free));
            view.spaceBad = plan.bytes + (1ull << 30) > free;
            view.time = w.ticked.empty()        ? "Nothing ticked"
                        : plan.images.empty()   ? "Built already"
                                                : [&] {
                                                      auto text = TL::FormatDuration(TL::EstimateSeconds(plan, w.scale));
                                                      text[0] = static_cast<char>(std::toupper(text[0]));
                                                      return text;
                                                  }();
            view.canBuild = !plan.images.empty() && !view.spaceBad && !w.running;
            view.canCancel = w.HasPack();
        }
        else if (w.page == TexturePage::Building)
        {
            view.step = w.step;
            view.count = w.count;
            view.then = w.then;
        }
        else if (w.page == TexturePage::Done)
        {
            std::vector<std::string> zones;
            for (const auto& zone : w.index.parts | std::views::keys)
                zones.push_back(zone);
            std::ranges::sort(zones, [](const auto& a, const auto& b) { return TL::ListsBefore(a, b); });
            for (const auto& zone : zones)
            {
                const auto& part = w.index.parts.at(zone);
                view.packs.push_back({zone, TL::GetPartName(zone),
                                      std::format("{}x, {}", part.scale, TL::FormatGigabytes(part.bytes)),
                                      !w.use.partsOff.contains(zone)});
            }
            for (const auto& pack : w.imported)
                view.packs.push_back({"imported:" + pack.name, pack.name + " (imported)",
                                      std::format("{}, {}", IWXMVM::Setup::FormatCount(pack.images.size()),
                                                  TL::FormatGigabytes(TL::GetPackBytes(pack))),
                                      !w.use.importedOff.contains(pack.name)});
            view.groupsOff = w.use.groupsOff;
            // (until its thread has ended, when Repair can start)
            view.checking = w.checking || (w.running && w.page == TexturePage::Done);
            view.wrong = w.checking ? 0 : w.wrongImages.size();
            if (view.wrong > 0)
                view.repairLine = std::format("{} textures wrong \xC2\xB7 {}", IWXMVM::Setup::FormatCount(view.wrong),
                                              w.wrongTime);
        }
        else if (w.page == TexturePage::Import)
        {
            view.folder = Platform::Utf8(w.importFolder);
            view.fits = std::format("{} textures, {}", IWXMVM::Setup::FormatCount(w.importSummary.fits),
                                    TL::FormatGigabytes(w.importSummary.bytes));
            view.leftOut = TL::DescribeLeftOut(w.importSummary);
            view.overBuilt = w.importSummary.overBuilt
                                 ? std::format("{} of the built textures", w.importSummary.overBuilt)
                                 : std::string("None");
            view.space = std::format("{} on {}, {} free", TL::FormatGigabytes(w.importSummary.bytes),
                                     Drive(w.root), TL::FormatGigabytes(Platform::FreeSpace(w.root)));
            view.canImport = w.importSummary.fits > 0 && !w.running;
        }
        return view;
    }

    void TexturePack::Tick(const std::string& zone)
    {
        std::lock_guard lock(work->mutex);
        if (!work->ticked.erase(zone))
            work->ticked.insert(zone);
    }

    void TexturePack::TickGroup(const std::string& group)
    {
        std::lock_guard lock(work->mutex);
        std::vector<std::string> zones;
        for (const auto& part : work->parts)
            if (GetGroup(part.zone) == group)
                zones.push_back(part.zone);
        const bool all = std::ranges::all_of(zones, [&](const auto& zone) { return work->ticked.contains(zone); });
        for (const auto& zone : zones)
            if (all)
                work->ticked.erase(zone);
            else
                work->ticked.insert(zone);
    }

    void TexturePack::SetScale(std::uint32_t scale)
    {
        std::lock_guard lock(work->mutex);
        work->scale = scale;
    }

    void TexturePack::Build()
    {
        std::vector<std::string> zones;
        std::uint32_t scale = 4;
        {
            std::lock_guard lock(work->mutex);
            if (work->running || work->ticked.empty())
                return;
            for (const auto& part : work->parts)
                if (work->ticked.contains(part.zone))
                    zones.push_back(part.zone);
            scale = work->scale;
            work->page = TexturePage::Building;
            work->line.clear();
            work->lineBad = false;
            work->fraction = 0.0f;
            work->step = "Starting";
            work->count.clear();
            work->then.clear();
        }
        work->Start([w = work.get(), zones, scale] { w->Build(zones, scale); });
    }

    void TexturePack::CheckPack(bool logAll)
    {
        work->Start([w = work.get(), logAll] { w->CheckPack(logAll); });
    }

    void TexturePack::Repair()
    {
        {
            std::lock_guard lock(work->mutex);
            if (work->running)
                return;
            work->page = TexturePage::Building;
            work->line.clear();
            work->lineBad = false;
            work->fraction = 0.0f;
            work->step = "Starting";
            work->count.clear();
            work->then.clear();
        }
        work->Start([w = work.get()] { w->Repair(); });
    }

    std::size_t TexturePack::CountWrong() const
    {
        std::lock_guard lock(work->mutex);
        return work->wrongImages.size();
    }

    void TexturePack::SetLimit(std::size_t limit)
    {
        work->limit = limit;
    }

    void TexturePack::Stop()
    {
        work->cancel = true;
    }

    void TexturePack::Cancel()
    {
        std::lock_guard lock(work->mutex);
        if (work->HasPack())
            work->page = TexturePage::Done;
    }

    void TexturePack::AddMaps()
    {
        std::lock_guard lock(work->mutex);
        work->line.clear();
        work->page = TexturePage::Summary;
    }

    void TexturePack::PickImport()
    {
        if (work->running)
            return;
        if (const auto folder = Platform::PickFolder(L"A folder of textures named after the game's images", {}))
            ReadImport(*folder);
    }

    void TexturePack::ReadImport(const std::filesystem::path& folder)
    {
        work->SetLine("Checking the folder");
        work->Start([w = work.get(), folder] { w->ReadImport(folder); });
    }

    void TexturePack::Log(std::string_view text)
    {
        work->Log(text);
    }

    void TexturePack::Import()
    {
        work->Start([w = work.get()] { w->CopyImport(); });
    }

    void TexturePack::CancelImport()
    {
        std::lock_guard lock(work->mutex);
        work->page = work->HasPack() ? TexturePage::Done : TexturePage::Summary;
    }

    void TexturePack::TogglePack(const std::string& id)
    {
        std::lock_guard lock(work->mutex);
        auto& set = id.starts_with("imported:") ? work->use.importedOff : work->use.partsOff;
        const auto key = id.starts_with("imported:") ? id.substr(9) : id;
        if (!set.erase(key))
            set.insert(key);
        work->SaveUse();
    }

    void TexturePack::ToggleGroup(TL::Group group)
    {
        std::lock_guard lock(work->mutex);
        if (!work->use.groupsOff.erase(group))
            work->use.groupsOff.insert(group);
        work->SaveUse();
    }

    void TexturePack::Remove(const std::string& id)
    {
        if (work->running)
            return;
        std::error_code error;
        if (id.starts_with("imported:"))
            std::filesystem::remove_all(work->root / TL::IMPORTED_FOLDER / Platform::FromUtf8(id.substr(9)), error);
        else
        {
            std::lock_guard lock(work->mutex);
            for (const auto& name : TL::RemovePart(work->index, id))
                std::filesystem::remove(work->built / Platform::FromUtf8(name + ".dds"), error);
            SaveIndex(work->built, work->index);
        }
        work->LoadPacks();
        std::lock_guard lock(work->mutex);
        if (!work->HasPack())
            work->page = TexturePage::Summary;
    }

    std::string TexturePack::Describe(const std::filesystem::path& storage)
    {
        std::vector<std::string> names;
        if (const auto index = LoadIndex(storage / "textures" / TL::BUILT_FOLDER))
        {
            std::vector<std::string> zones;
            for (const auto& zone : index->parts | std::views::keys)
                zones.push_back(zone);
            std::ranges::sort(zones, [](const auto& a, const auto& b) { return TL::ListsBefore(a, b); });
            for (const auto& zone : zones)
                names.push_back(std::format("{} {}x", TL::GetPartName(zone), index->parts.at(zone).scale));
        }
        std::error_code error;
        std::size_t imported = 0;
        for (const auto& folder : std::filesystem::directory_iterator(storage / "textures" / TL::IMPORTED_FOLDER, error))
            imported += folder.is_directory();
        if (imported)
            names.push_back(std::format("{} imported", imported));
        if (names.empty())
            return "None built";
        std::string text;
        for (std::size_t i = 0; i < names.size(); ++i)
            text += (i ? ", " : "") + names[i];
        return text;
    }

    TextureView DemoTextureView(const std::string& state)
    {
        // the mockup's numbers: Nuketown and the guns and hands at 4x on one PC
        TextureView view;
        view.scale = 4;
        if (state.starts_with("texture-summary"))
        {
            view.page = TexturePage::Summary;
            view.parts = {{"common_mp", "Guns and hands", "4.0 GB", true, false, ""},
                          {"mp_array", "Array", "3.9 GB", false, false, "Maps"},
                          {"mp_cracked", "Cracked", "3.6 GB", false, false, "Maps"},
                          {"mp_crisis", "Crisis", "3.3 GB", false, false, "Maps"},
                          {"mp_firingrange", "Firing Range", "3.5 GB", false, false, "Maps"},
                          {"mp_nuked", "Nuketown", "3.4 GB", true, false, "Maps"},
                          {"mp_villa", "Villa", "3.2 GB", false, false, "Maps"},
                          {"cuba", "Operation 40", "2.9 GB", false, false, "Campaign"},
                          {"vorkuta", "Vorkuta", "3.1 GB", false, false, "Campaign"},
                          {"zombie_theater", "Kino der Toten", "2.6 GB", false, false, "Zombies"}};
            view.model = "4xNomos8kSC (Helaman, CC-BY 4.0)";
            view.downloads = "Real-ESRGAN, Nomos8kSC, texconv, 76 MB";
            view.space = "7.4 GB on C:, 87.0 GB free";
            view.time = "About 49 minutes";
            view.canBuild = true;
        }
        else if (state == "texture-building")
        {
            view.page = TexturePage::Building;
            view.step = "Upscaling Nuketown";
            view.count = "312 / 922";
            view.fraction = 0.21f;
            view.then = "1 more map \xC2\xB7 about 36 minutes left";
        }
        else if (state == "texture-repairing")
        {
            view.page = TexturePage::Building;
            view.step = "Repairing";
            view.count = "1,204 / 5,366";
            view.fraction = 0.22f;
            view.then = "About 35 minutes left";
        }
        else if (state == "texture-import")
        {
            view.page = TexturePage::Import;
            view.folder = "D:\\packs\\HD Bricks";
            view.fits = "812 textures, 3.1 GB";
            view.leftOut = "14 not the game's names, 2 not DXT";
            view.overBuilt = "96 of the built textures";
            view.space = "3.1 GB on C:, 80.0 GB free";
            view.canImport = true;
        }
        else
        {
            view.page = TexturePage::Done;
            view.packs = {{"common_mp", "Guns and hands", "4x, 4.0 GB", true},
                          {"mp_nuked", "Nuketown", "4x, 3.4 GB", true},
                          {"imported:HD Bricks", "HD Bricks (imported)", "812, 3.1 GB", true}};
            // (texture-done-all: every map built, as on a second PC)
            if (state == "texture-done-all")
            {
                view.packs = {{"common_mp", "Guns and hands", "4x, 4.0 GB", true}};
                for (const auto& [zone, name] : std::initializer_list<std::pair<const char*, const char*>>{
                         {"mp_array", "Array"},           {"mp_cracked", "Cracked"},     {"mp_crisis", "Crisis"},
                         {"mp_firingrange", "Firing Range"}, {"mp_duga", "Grid"},          {"mp_hanoi", "Hanoi"},
                         {"mp_cairo", "Havana"},          {"mp_havoc", "Jungle"},        {"mp_cosmodrome", "Launch"},
                         {"mp_nuked", "Nuketown"},        {"mp_radiation", "Radiation"}, {"mp_mountain", "Summit"},
                         {"mp_villa", "Villa"},           {"mp_russianbase", "WMD"},     {"cuba", "Operation 40"},
                         {"vorkuta", "Vorkuta"},          {"zombie_theater", "Kino der Toten"}})
                    view.packs.push_back({zone, name, "4x, 3.4 GB", true});
            }
            view.groupsOff = {TL::Group::Weapons};
            if (state == "texture-checking")
            {
                view.checking = true;
                view.line = "Checking 8,200 / 16,298";
            }
            else if (state == "texture-repair")
            {
                view.wrong = 5366;
                view.repairLine = "5,366 textures wrong \xC2\xB7 about 45 minutes";
            }
            else if (state == "texture-repaired")
                view.line = "Repaired in 45 minutes";
        }
        return view;
    }
}  // namespace Launcher

namespace Launcher
{
    int RunTextureBuild(const Options& options)
    {
        const auto settings = Settings::Load(options.home / "launcher.json");
        const auto detection = Detect(options, settings);
        if (!detection.GameFound())
            return 2;
        TexturePack pack(options, detection.gameFolder);
        while (pack.Busy() || pack.View().page == TexturePage::Reading)
            Sleep(200);
        if (!options.importTextures.empty())
        {
            pack.ReadImport(options.importTextures);
            while (pack.Busy())
                Sleep(200);
            const auto summary = pack.View();
            pack.Log(std::format("import of {}: fits {}; left out {}; over built {}",
                                 Platform::Utf8(options.importTextures), summary.fits, summary.leftOut,
                                 summary.overBuilt));
            if (!summary.canImport)
                return 1;
            pack.Import();
            do
                Sleep(200);
            while (pack.Busy());
            return pack.View().page == TexturePage::Done ? 0 : 1;
        }
        if (options.checkTextures || options.repairTextures)
        {
            pack.CheckPack(!options.repairTextures);
            do
                Sleep(200);
            while (pack.Busy());
            pack.Log(std::format("{} textures to make again", pack.CountWrong()));
            if (!options.repairTextures || pack.CountWrong() == 0)
                return 0;
            pack.SetLimit(options.textureLimit);
            pack.Repair();
            do
                Sleep(200);
            while (pack.Busy());
            pack.Log(std::format("{} textures still wrong", pack.CountWrong()));
            return pack.View().lineBad || pack.CountWrong() ? 1 : 0;
        }
        auto view = pack.View();
        for (const auto& part : view.parts)
            if (part.ticked)
                pack.Tick(part.zone);
        std::string zones = options.buildTextures + ",";
        for (std::size_t at = 0, next; (next = zones.find(',', at)) != std::string::npos; at = next + 1)
            if (next > at)
                pack.Tick(zones.substr(at, next - at));
        pack.SetScale(options.textureScale);
        pack.SetLimit(options.textureLimit);
        pack.Build();
        do
            Sleep(200);
        while (pack.Busy());
        view = pack.View();
        return view.lineBad ? 1 : 0;
    }
}  // namespace Launcher
