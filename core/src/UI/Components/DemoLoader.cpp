#include "StdInclude.hpp"
#include "DemoLoader.hpp"

#include "Mod.hpp"
#include "UI/UIManager.hpp"
#include "UI/GettingStarted.hpp"
#include "Utilities/PathUtils.hpp"
#include "Resources.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/Theme.hpp"
#include "Configuration/Configuration.hpp"

namespace IWXMVM::UI
{
    template <bool caseSensitive, typename T>
        requires std::is_same_v<T, std::string_view> || std::is_same_v<T, std::wstring_view>
    bool CompareNaturally(const T lhs, const T rhs)
    {
        auto IsDigit = [](auto c) { return std::iswdigit(c); };

        auto ToUpper = [](auto c) {
            if constexpr (caseSensitive)
                return c;
            else
                return std::towupper(c);
        };

        auto StringToUint = [](const auto* str, auto* output, std::size_t* endPtr = nullptr) {
            try
            {
                *output = std::stoull(str, endPtr);
                return true;
            }
            catch (...)
            {
                return false;
            }
        };

        for (auto lhsItr = lhs.begin(), rhsItr = rhs.begin(); lhsItr != lhs.end() && rhsItr != rhs.end();)
        {
            if (IsDigit(*lhsItr) && IsDigit(*rhsItr))
            {
                std::uint64_t lhsNum = 0;
                std::uint64_t rhsNum = 0;
                std::size_t lhsDigitCount = 0;
                std::size_t rhsDigitCount = 0;

                // when sorting a container, move the 'unparsable' string_view to end of container if string-to-uint
                // throws an exception
                if (!StringToUint(std::addressof(*lhsItr), &lhsNum, &lhsDigitCount))
                    return false;
                if (!StringToUint(std::addressof(*rhsItr), &rhsNum, &rhsDigitCount))
                    return true;

                if (lhsNum != rhsNum)
                    return lhsNum < rhsNum;

                assert(lhsDigitCount == std::distance(lhsItr, std::find_if_not(lhsItr, lhs.end(), IsDigit)));
                assert(rhsDigitCount == std::distance(rhsItr, std::find_if_not(rhsItr, rhs.end(), IsDigit)));

                lhsItr += lhsDigitCount;
                rhsItr += rhsDigitCount;
            }
            else
            {
                if (ToUpper(*lhsItr) != ToUpper(*rhsItr))
                    return *lhsItr < *rhsItr;

                ++lhsItr;
                ++rhsItr;
            }
        }

        return lhs.length() < rhs.length();
    }

    void SortDemoPaths(const auto demos)
    {
        std::sort(demos.begin(), demos.end(), [&](const auto& lhs, const auto& rhs) {
            const std::size_t extLength = Mod::GetGameInterface()->GetDemoExtension().length();
            const std::size_t dirLength = demos.front().parent_path().native().length();
            const std::size_t lhsLength = lhs.native().length();
            const std::size_t rhsLength = rhs.native().length();

            assert(lhsLength > dirLength + extLength + 1 && rhsLength > dirLength + extLength + 1);

            const auto* lhsFileNamePtr = lhs.c_str() + dirLength + 1;
            const auto* rhsFileNamePtr = rhs.c_str() + dirLength + 1;
            const std::size_t lhsSvLength = lhsLength - dirLength - extLength - 1;
            const std::size_t rhsSvLength = rhsLength - dirLength - extLength - 1;

            using StringView = std::wstring_view;
            return CompareNaturally<false>(StringView{lhsFileNamePtr, lhsSvLength},
                                           StringView{rhsFileNamePtr, rhsSvLength});
        });
    }

    void SortDemoDirectories(const auto directories, auto GetPath)
    {
        std::sort(directories.begin(), directories.end(), [&](const auto& lhs, const auto& rhs) {
            const auto& lhsPath = GetPath(lhs);
            const auto& rhsPath = GetPath(rhs);

            const std::size_t parentDirLength = GetPath(directories.front()).parent_path().native().length();
            const std::size_t lhsLength = lhsPath.native().length();
            const std::size_t rhsLength = rhsPath.native().length();

            assert(lhsLength > parentDirLength + 1 && rhsLength > parentDirLength + 1);

            const auto* lhsDirPtr = lhsPath.c_str() + parentDirLength + 1;
            const auto* rhsDirNamePtr = rhsPath.c_str() + parentDirLength + 1;
            const std::size_t lhsSvLength = lhsLength - parentDirLength - 1;
            const std::size_t rhsSvLength = rhsLength - parentDirLength - 1;

            using StringView = std::wstring_view;
            return CompareNaturally<false>(StringView{lhsDirPtr, lhsSvLength}, StringView{rhsDirNamePtr, rhsSvLength});
        });
    }

    bool IsFileDemo(const std::filesystem::path& file)
    {
        return file.extension().compare(Mod::GetGameInterface()->GetDemoExtension())
                   ? false
                   : true;  // .compare() == 0 => strings are equal
    }

    void DemoLoader::AddPathsToSearch(const std::vector<std::filesystem::path>& dirs)
    {
        for (const auto& dir : dirs)
        {
            if (std::filesystem::exists(dir))
            {
                DemoDirectory searchPath = {.path = dir};
                demoDirectories.push_back(searchPath);
            }
        }
        searchPaths = std::make_pair(0, demoDirectories.size());
    }

    void DemoLoader::SearchDir(std::size_t dirIdx)
    {
        auto tempDir = std::string(DEMO_TEMP_DIRECTORY);
        if (demoDirectories[dirIdx].path.wstring().find(std::wstring(tempDir.begin(), tempDir.end())) != std::string::npos)
        {
            demoDirectories[dirIdx].relevant = false;
            return;
        }

        auto subdirsStartIdx = demoDirectories.size();
        auto demosStartIdx = demoPaths.size();

        for (const auto& entry : std::filesystem::directory_iterator(demoDirectories[dirIdx].path))
        {
            if (entry.is_directory())
            {
                DemoDirectory subdir = {.path = entry.path(), .parentIdx = dirIdx};
                demoDirectories.push_back(subdir);
            }
            else if (IsFileDemo(entry.path()))
            {
                demoPaths.push_back(entry.path());
            }
        }

        SortDemoPaths(std::span{(demoPaths.begin() + demosStartIdx), demoPaths.end()});
        SortDemoDirectories(std::span{(demoDirectories.begin() + subdirsStartIdx), demoDirectories.end()},
                            [](const DemoDirectory& data) -> const std::filesystem::path& { return data.path; });

        demoDirectories[dirIdx].demos = std::make_pair(demosStartIdx, demoPaths.size());
        if (demoDirectories[dirIdx].demos.first != demoDirectories[dirIdx].demos.second)
        {
            demoDirectories[dirIdx].relevant = true;
        }

        demoDirectories[dirIdx].subdirectories = std::make_pair(subdirsStartIdx, demoDirectories.size());
        for (auto i = demoDirectories[dirIdx].subdirectories.first; i < demoDirectories[dirIdx].subdirectories.second;
             i++)
        {
            SearchDir(i);
        }
    }

    void DemoLoader::Search()
    {
        for (auto i = searchPaths.first; i < searchPaths.second; i++)
        {
            SearchDir(i);
        }
    }

    void DemoLoader::MarkDirsRelevancy()
    {
        for (auto it = demoDirectories.rbegin(); it != demoDirectories.rend(); it++)
        {
            std::size_t vecIdx = std::abs(it - demoDirectories.rend() + 1);
            if (demoDirectories[vecIdx].relevant)
            {
                while (demoDirectories[vecIdx].parentIdx.has_value())
                {
                    vecIdx = demoDirectories[vecIdx].parentIdx.value();
                    if (demoDirectories[vecIdx].relevant)
                    {
                        break;
                    }
                    demoDirectories[vecIdx].relevant = true;
                }
            }
        }
    }

    void DemoLoader::FindAllDemos()
    {
        if (isScanningDemoPaths.load())
			return;

        LOG_DEBUG("Searching for demo files...");
        isScanningDemoPaths.store(true);
        std::thread([this, recents = std::vector(recentDemos.begin(), recentDemos.end())] {
            demoDirectories.clear();
            demoPaths.clear();

            auto searchPaths = std::vector(PreferencesConfiguration::Get().additionalDemoSearchDirectories);
            searchPaths.push_back(Mod::GetGameInterface()->GetUserDirectory());

            AddPathsToSearch(searchPaths);
            Search();

            // 'demoDirectories' and 'demoPaths' are complete here, but we still need to find out the relevancy of each
            // directory
            MarkDirsRelevancy();

            LOG_DEBUG("Found {0} demo files", demoPaths.size());

            // (the list is drawn meanwhile, and may be scanned again: the mods are read from a copy)
            auto demos = demoPaths;
            demos.insert(demos.end(), recents.begin(), recents.end());
            isScanningDemoPaths.store(false);
            ReadDemoMods(std::move(demos));
        }).detach();
    }

    // On the scan's thread: the mods of the game's folders, and the fs_game of every demo not read before at its size
    // and time
    void DemoLoader::ReadDemoMods(std::vector<std::filesystem::path> demos)
    {
        const auto game = Mod::GetGameInterface();
        if (!game->GetLoadedFsGame())
            return;

        auto found = Components::DemoModLogic::FindMods(game->GetModFolders());
        LOG_INFO("The game runs mod '{0}', and its mods folders have {1}", *game->GetLoadedFsGame(), found.size());
        {
            std::lock_guard lock(demoModsMutex);
            mods = std::move(found);
        }

        std::size_t read = 0;
        for (const auto& path : demos)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            const auto time = error ? std::filesystem::file_time_type() : std::filesystem::last_write_time(path, error);
            if (error)
                continue;
            {
                std::lock_guard lock(demoModsMutex);
                const auto it = demoMods.find(path.native());
                if (it != demoMods.end() && it->second.size == size && it->second.time == time)
                    continue;
            }
            auto fsGame = game->GetDemoFsGame(path);
            std::lock_guard lock(demoModsMutex);
            demoMods[path.native()] = {size, time, std::move(fsGame)};
            ++read;
        }
        LOG_DEBUG("Read the mods of {0} demos", read);
    }

    std::optional<std::string> DemoLoader::GetDemoFsGame(const std::filesystem::path& path)
    {
        std::lock_guard lock(demoModsMutex);
        const auto it = demoMods.find(path.native());
        return it != demoMods.end() ? it->second.fsGame : std::nullopt;
    }

    std::vector<Components::DemoModLogic::GameMod> DemoLoader::GetMods()
    {
        std::lock_guard lock(demoModsMutex);
        return mods;
    }

    std::filesystem::path GetRecentDemosPath()
    {
        return PathUtils::GetModPath() / "recent_demos.json";
    }

    constexpr std::string_view NODE_RECENT_DEMOS = "recent_demos";

    void DemoLoader::SaveRecentDemos()
    {
        try
        {
            nlohmann::json json;

            json[NODE_RECENT_DEMOS] = nlohmann::json::array();

            for (const auto& path : recentDemos)
            {
                json[NODE_RECENT_DEMOS].push_back(path.string());
            }

            const auto savePath = GetRecentDemosPath();
            if (!std::filesystem::exists(savePath.parent_path()))
            {
                std::filesystem::create_directories(savePath.parent_path());
            }

            std::ofstream recentDemosFile(savePath);
            recentDemosFile << json.dump(4);
            recentDemosFile.close();
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Failed to save recent demos: {}", e.what());
        }
    }

    void DemoLoader::LoadRecentDemos()
    {
        try
        {
            const auto loadPath = GetRecentDemosPath();
            if (!std::filesystem::exists(loadPath))
                return;

            std::ifstream file(loadPath);
            if (!file.is_open())
                return;

            nlohmann::json json;
            file >> json;

            if (!json.contains(NODE_RECENT_DEMOS) || !json[NODE_RECENT_DEMOS].is_array())
                return;

            recentDemos.clear();
            for (const auto& item : json[NODE_RECENT_DEMOS])
            {
                if (item.is_string())
                {
                    std::filesystem::path path(item.get<std::string>());
                    if (std::filesystem::exists(path))
                    {
                        recentDemos.emplace_back(std::move(path));
                    }
                }
            }

            while (recentDemos.size() > maxRecentDemos)
            {
                recentDemos.pop_back();
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Failed to load recent demos: {}", e.what());
        }
    }

    bool DemoLoader::DemoFilter(const std::u8string& demoFileName)
    {
        // TODO: possibly make filter more advanced, add features like "" for exact search etc
        bool keepDemo = true;
        std::u8string lowerDemoFileName = demoFileName;
        std::transform(lowerDemoFileName.begin(), lowerDemoFileName.end(), lowerDemoFileName.begin(),
                       ::tolower);
        for (auto& word : searchBarTextSplit)
        {
            std::transform(word.begin(), word.end(), word.begin(), ::tolower);
            if (lowerDemoFileName.find(word) == std::u8string::npos)
            {
                keepDemo = false;
                break;
            }
        }
        return keepDemo;
    }

    namespace
    {
        std::uint64_t NowMs()
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch())
                                                  .count());
        }
    }  // namespace

    bool DemoLoader::Play(const std::filesystem::path& path)
    {
        if (loading || modSwitch)
        {
            LOG_INFO("Not playing demo {0}: {1} is loading", path.string(),
                     loading ? loading->load.name : modSwitch->fsGame);
            return false;
        }
        loading = Loading{path, {.name = path.filename().string(), .since = NowMs()}};
        failed.reset();
        failedNeeds.reset();
        switchFailed.reset();
        std::erase(recentDemos, path);
        recentDemos.emplace_front(path);
        while (recentDemos.size() > maxRecentDemos)
            recentDemos.pop_back();
        SaveRecentDemos();
        return true;
    }

    bool DemoLoader::SwitchMod(const std::string& modName, std::optional<std::filesystem::path> thenPlay)
    {
        const auto loaded = Mod::GetGameInterface()->GetLoadedFsGame();
        if (loading || modSwitch || !loaded)
        {
            LOG_INFO("Not switching to mod '{0}': a demo or a mod is loading", modName);
            return false;
        }
        auto fsGame = Components::DemoModLogic::GetFsGame(modName);
        failed.reset();
        failedNeeds.reset();
        switchFailed.reset();
        if (fsGame == *loaded)
        {
            if (thenPlay)
                Play(*thenPlay);
            return true;
        }
        LOG_INFO("Switching from mod '{0}' to '{1}'{2}", *loaded, fsGame,
                 thenPlay ? ", then playing " + thenPlay->filename().string() : "");
        switchFrom = std::string(Components::DemoModLogic::GetModName(*loaded));
        modSwitch = ModSwitchLogic::Switch{.fsGame = std::move(fsGame), .since = NowMs()};
        playAfterSwitch = std::move(thenPlay);
        return true;
    }

    void DemoLoader::UpdateModSwitch()
    {
        if (!modSwitch)
            return;
        const auto game = Mod::GetGameInterface();
        const ModSwitchLogic::Seen seen{.now = NowMs(),
                                        .state = game->GetGameState(),
                                        .loadedFsGame = game->GetLoadedFsGame().value_or(""),
                                        .initialized = UIManager::Get().GetInitializeCount(),
                                        .mainFrames = game->GetMainThreadFrames()};
        switch (ModSwitchLogic::Advance(*modSwitch, seen))
        {
            case ModSwitchLogic::Outcome::Stop:
                // (as STOP)
                game->Disconnect();
                break;
            case ModSwitchLogic::Outcome::Send:
                sendModNow = true;
                break;
            case ModSwitchLogic::Outcome::Done:
            {
                const auto name = std::string(Components::DemoModLogic::GetModName(modSwitch->fsGame));
                LOG_INFO("Mod '{0}' loaded, {1} ms after it was asked for", modSwitch->fsGame,
                         seen.now - modSwitch->since);
                // the launcher starts the game with it next
                PreferencesConfiguration::Get().gameMod = name;
                Configuration::Get().Write(true);
                const auto then = std::move(playAfterSwitch);
                modSwitch.reset();
                playAfterSwitch.reset();
                if (then)
                    Play(*then);
                break;
            }
            case ModSwitchLogic::Outcome::GaveUp:
                LOG_WARN("Mod '{0}' didn't load within a minute (the game runs '{1}')", modSwitch->fsGame,
                         seen.loadedFsGame);
                switchFailed = modSwitch->fsGame.empty()
                                   ? switchFrom
                                   : std::string(Components::DemoModLogic::GetModName(modSwitch->fsGame));
                switchFailedUnload = modSwitch->fsGame.empty();
                modSwitch.reset();
                playAfterSwitch.reset();
                break;
            default:
                break;
        }
    }

    void DemoLoader::Update()
    {
        UpdateModSwitch();
        if (!loading)
            return;
        const auto game = Mod::GetGameInterface();
        DemoLoadLogic::Seen seen{.now = NowMs(), .state = game->GetGameState(), .reading = game->IsReadingDemo()};
        if (seen.state == Types::GameState::InDemo)
            seen.playing = game->GetDemoInfo().name;
        switch (const auto outcome = DemoLoadLogic::Advance(loading->load, seen))
        {
            case DemoLoadLogic::Outcome::Waiting:
                break;
            case DemoLoadLogic::Outcome::Send:
                sendNow = true;
                loading->load.sent = seen.now;
                break;
            case DemoLoadLogic::Outcome::Played:
                LOG_INFO("Demo {0} plays, {1} ms after PLAY", loading->load.name, seen.now - loading->load.since);
                loading.reset();
                break;
            default:
                Fail(std::string(DemoLoadLogic::Describe(outcome)));
                break;
        }
    }

    void DemoLoader::Send()
    {
        const auto game = Mod::GetGameInterface();
        // (its notice is on screen: the window freezes as the game reloads)
        if (std::exchange(sendModNow, false) && modSwitch)
            game->LoadMod(modSwitch->fsGame);

        if (!std::exchange(sendNow, false) || !loading)
            return;
        // (its notice is on screen: the window freezes as the game starts reading it)
        game->PlayDemo(loading->path);
        if (const auto error = game->GetPlayDemoError(); !error.empty())
        {
            // a demo of another mod gets the notice's Load Mod (or Unload Mod)
            const auto loaded = game->GetLoadedFsGame();
            const auto demoFsGame = loaded ? game->GetDemoFsGame(loading->path) : std::nullopt;
            Fail(error);
            if (demoFsGame && !Components::DemoModLogic::Plays(*demoFsGame, *loaded))
                failedNeeds = *demoFsGame;
        }
    }

    void DemoLoader::Fail(std::string reason)
    {
        LOG_WARN("Demo {0} didn't load: {1}", loading->load.name, reason);
        failed = std::pair(loading->path, std::move(reason));
        failedNeeds.reset();
        loading.reset();
    }

    namespace
    {
        // the notice's main button, in the accent
        bool PrimaryButton(const char* label)
        {
            const auto& p = Theme::Current();
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::Vec4(p.accent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Vec4(p.AccentHover()));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Vec4(p.AccentActive()));
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Vec4(p.onAccent));
            const bool clicked = ImGui::Button(label);
            ImGui::PopStyleColor(4);
            return clicked;
        }
    }  // namespace

    void DemoLoader::RenderLoadNotice()
    {
        if (!loading && !failed && !modSwitch && !switchFailed)
            return;
        const auto fontSize = ImGui::GetFontSize();
        const auto padding = ImVec2(fontSize * 0.6f, fontSize * 0.4f);
        const auto start = ImGui::GetCursorScreenPos();
        const auto width = ImGui::GetContentRegionAvail().x;
        auto drawList = ImGui::GetWindowDrawList();
        // (its background goes under it once its height is known)
        drawList->ChannelsSplit(2);
        drawList->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(start + padding);
        ImGui::BeginGroup();
        // (a switch asked for from the notice starts once it is drawn, as it reads what failed)
        std::optional<std::pair<std::string, std::optional<std::filesystem::path>>> switchTo;
        if (modSwitch)
        {
            const auto name = std::string(Components::DemoModLogic::GetModName(modSwitch->fsGame));
            ImGui::TextUnformatted(name.empty() ? std::format(ICON_FA_HOURGLASS_HALF " Unloading {}", switchFrom).c_str()
                                                : std::format(ICON_FA_HOURGLASS_HALF " Loading {}", name).c_str());
            ImGuiEx::Keyframeable::Record("demo.notice");
            ImGui::SetItemTooltip("The game reloads for a few seconds");
        }
        else if (loading)
        {
            ImGui::TextUnformatted(
                std::format(ICON_FA_HOURGLASS_HALF " Loading {}", loading->path.stem().string()).c_str());
            ImGuiEx::Keyframeable::Record("demo.notice");
        }
        else if (switchFailed)
        {
            const auto closeSize = ImGui::GetFrameHeight();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - padding.x * 3 - closeSize);
            ImGui::TextColored(Theme::Problem(), ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextUnformatted(
                std::format("{} didn't {}", *switchFailed, switchFailedUnload ? "unload" : "load").c_str());
            ImGuiEx::Keyframeable::Record("demo.failed");
            ImGui::TextDisabled("Not done within a minute.");
            ImGui::PopTextWrapPos();
        }
        else if (failedNeeds)
        {
            // a demo of another mod: what it needs, and the buttons that
            // switch to it when it is one of the user's mods
            const auto& needs = *failedNeeds;
            const auto modName = std::string(Components::DemoModLogic::GetModName(needs));
            const bool found = needs.empty() || Components::DemoModLogic::HasMod(GetMods(), modName);
            const auto closeSize = ImGui::GetFrameHeight();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - padding.x * 3 - closeSize);
            ImGui::TextColored(Theme::Problem(), ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextUnformatted(
                Components::DemoModLogic::GetNeedsTitle(failed->first.stem().string(), needs).c_str());
            ImGuiEx::Keyframeable::Record("demo.failed");
            ImGui::TextDisabled(
                "%s", Components::DemoModLogic::GetNeedsLine(needs, loadedFsGame.value_or(""), found).c_str());
            ImGui::PopTextWrapPos();
            if (found)
            {
                const std::string verb = needs.empty() ? "Unload Mod" : "Load Mod";
                if (ImGui::Button(std::format("{}##notice", verb).c_str()))
                    switchTo = std::pair(modName, std::nullopt);
                ImGuiEx::Keyframeable::Record("demo.loadmod");
                ImGui::SetItemTooltip("The game reloads for a few seconds");
                ImGui::SameLine();
                if (PrimaryButton(std::format("{} and Play Demo##notice", verb).c_str()))
                    switchTo = std::pair(modName, failed->first);
                ImGuiEx::Keyframeable::Record("demo.loadmodplay");
            }
        }
        else
        {
            const auto closeSize = ImGui::GetFrameHeight();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - padding.x * 3 - closeSize);
            ImGui::TextColored(Theme::Problem(), ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
            ImGui::TextUnformatted(std::format("{} didn't load", failed->first.stem().string()).c_str());
            ImGuiEx::Keyframeable::Record("demo.failed");
            ImGui::TextDisabled("%s", failed->second.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
        const auto end = ImVec2(start.x + width, ImGui::GetItemRectMax().y + padding.y);
        if ((failed || switchFailed) && !loading && !modSwitch)
        {
            // (closed: it goes)
            const auto closeSize = ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(ImVec2(end.x - padding.x - closeSize, start.y + padding.y * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::Button(ICON_FA_XMARK "##closeFailed", ImVec2(closeSize, 0)))
            {
                failed.reset();
                failedNeeds.reset();
                switchFailed.reset();
            }
            ImGui::PopStyleColor();
            ImGuiEx::Keyframeable::Record("demo.failed.close");
            ImGui::SetItemTooltip("Close");
        }
        drawList->ChannelsSetCurrent(0);
        drawList->AddRectFilled(start, end, Theme::Card(), fontSize * 0.3f);
        drawList->AddRect(start, end, ImGui::GetColorU32(ImGuiCol_Border), fontSize * 0.3f);
        drawList->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(start.x, end.y));
        ImGui::Dummy(ImVec2(width, ImGui::GetStyle().ItemSpacing.y));
        if (switchTo)
            SwitchMod(switchTo->first, switchTo->second);
    }

    // "Mod: mp_bots" or "No mod", with Change..., when the game can switch and the
    // folders have a mod or one runs
    void DemoLoader::RenderModLine(const std::string& loaded)
    {
        const auto found = GetMods();
        if (found.empty() && loaded.empty())
            return;
        const auto loadedName = std::string(Components::DemoModLogic::GetModName(loaded));
        ImGui::AlignTextToFramePadding();
        if (loadedName.empty())
        {
            ImGui::TextUnformatted("No mod");
        }
        else
        {
            ImGui::TextDisabled("Mod:");
            ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 0.5f);
            ImGui::TextUnformatted(loadedName.c_str());
        }
        ImGuiEx::Keyframeable::Record("demo.modline");

        const char* changeLabel = "Change...";
        const auto buttonWidth = ImGui::CalcTextSize(changeLabel).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth);
        ImGui::BeginDisabled(IsLoading());
        if (ImGui::Button("Change...##mod"))
            ImGui::OpenPopup("##modChange");
        ImGui::EndDisabled();
        ImGuiEx::Keyframeable::Record("demo.modchange");
        ImGui::SetItemTooltip("The game reloads for a few seconds");

        if (ImGui::BeginPopup("##modChange"))
        {
            std::optional<std::string> pick;
            if (ImGui::Selectable("No mod", loadedName.empty()))
                pick = "";
            ImGuiEx::Keyframeable::Record("demo.modchange.none");
            for (const auto& mod : found)
            {
                if (ImGui::Selectable(mod.name.c_str(), _stricmp(mod.name.c_str(), loadedName.c_str()) == 0))
                    pick = mod.name;
                ImGuiEx::Keyframeable::Record("demo.modchange." + mod.name);
            }
            ImGui::EndPopup();
            if (pick)
                SwitchMod(*pick);
        }
    }

    // a demo's mod after its name: grey while it runs, the Problem color with the
    // warning sign when it doesn't
    namespace
    {
        // A demo's name up to the panel's edge, less room for its mod's tag: a longer one ends in "...", and shows
        // whole in a tooltip
        void DrawDemoName(const std::string& name, const std::string& fsGame)
        {
            const auto mod = std::string(Components::DemoModLogic::GetModName(fsGame));
            const auto tag = fsGame.empty() ? 0.0f : ImGui::CalcTextSize(mod.c_str()).x + ImGui::GetFontSize() * 2.0f;
            const auto room = ImGui::GetContentRegionAvail().x - tag;
            const auto size = ImGui::CalcTextSize(name.c_str());
            if (size.x <= room || room <= 0.0f)
            {
                ImGui::TextUnformatted(name.c_str());
                return;
            }
            const auto at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(room, size.y));
            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), at, at + ImVec2(room, size.y), at.x + room,
                                      at.x + room, name.c_str(), nullptr, &size);
            ImGui::SetItemTooltip("%s", name.c_str());
        }
    }  // namespace

    void DemoLoader::RenderModTag(const std::string& fsGame, const std::string& loaded)
    {
        const bool runs = fsGame == loaded;
        const auto name = std::string(Components::DemoModLogic::GetModName(fsGame));
        const auto label = runs ? name : std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + name;
        const auto& p = Theme::Current();
        const auto textColor = runs ? Theme::Vec4(p.muted) : Theme::Problem();
        ImGui::SameLine();
        ImGui::TextColored(textColor, "%s", label.c_str());
        const auto fontSize = ImGui::GetFontSize();
        const auto pad = ImVec2(fontSize * 0.35f, fontSize * 0.08f);
        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        auto drawList = ImGui::GetWindowDrawList();
        // (the background over the text, and the text again on it)
        drawList->AddRectFilled(min - pad, max + pad,
                                runs ? Theme::U32(p.raised) : ImGui::GetColorU32(Theme::Problem() * ImVec4(1, 1, 1, 0.18f)),
                                fontSize * 0.15f);
        drawList->AddText(min, ImGui::GetColorU32(textColor), label.c_str());
        ImGui::SetItemTooltip(runs ? "Recorded with %s" : "Recorded with %s, not loaded: PLAY offers to load it",
                              name.c_str());
    }

    template <typename T>
    void DemoLoader::RenderDemos(const T& demos)
    {
        ImGuiListClipper clipper;
        clipper.Begin(demos.size());

        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                try
                {
                    auto demoName = demos[i].filename().string();

                    // (while a demo loads, its row says so, a little wider, and every other waits)
                    const auto loadingLabel = std::string(ICON_FA_HOURGLASS_HALF " Loading...");
                    const auto buttonSize = ImVec2(
                        loading ? std::max(ImGui::GetFontSize() * 4, ImGui::CalcTextSize(loadingLabel.c_str()).x +
                                                                         ImGui::GetStyle().FramePadding.x * 2)
                                : ImGui::GetFontSize() * 4,
                        ImGui::GetFontSize() * 1.5f);
                    if (loading && loading->path == demos[i])
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                        ImGui::Button(std::format("{}##{}", loadingLabel, demoName).c_str(), buttonSize);
                        ImGui::PopStyleColor(2);
                        ImGuiEx::Keyframeable::Record("demo.loading");
                        ImGui::SetItemTooltip("The game stops for a few seconds, then shows its loading screen");
                        ImGui::SameLine();
                        DrawDemoName(demoName, {});
                        continue;
                    }

                    ImGui::BeginDisabled(IsLoading());
                    if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo &&
                        Mod::GetGameInterface()->GetDemoInfo().name == demoName)
                    {
                        if (ImGui::Button(std::format(ICON_FA_STOP " STOP##{0}", demoName).c_str(), buttonSize))
                        {
                            Mod::GetGameInterface()->Disconnect();
                        }
                        ImGuiEx::Keyframeable::Record("demo.stop");
                    }
                    else
                    {
                        if (ImGui::Button(std::format(ICON_FA_PLAY " PLAY##{0}", demoName).c_str(), buttonSize))
                        {
                            demoToPlay = demos[i];
                        }
                    }

                    ImGui::SameLine();

                    const auto fsGame = loadedFsGame ? GetDemoFsGame(demos[i]) : std::nullopt;
                    DrawDemoName(demoName, fsGame ? *fsGame : std::string());
                    ImGui::EndDisabled();

                    if (loadedFsGame)
                    {
                        if (fsGame && !fsGame->empty())
                        {
                            RenderModTag(*fsGame, *loadedFsGame);
                            ImGuiEx::Keyframeable::Record("demo.modtag." + demos[i].stem().string());
                        }
                    }
                }
                catch (std::exception&)
                {
                    ImGui::BeginDisabled();
                    ImGui::Button(ICON_FA_TRIANGLE_EXCLAMATION " ERROR");
                    ImGui::SameLine();
                    ImGui::Text("<invalid demo name>");
                    ImGui::EndDisabled();
                }
            }
        }
    }

    // Wrapper for RenderDemos
    void DemoLoader::FilteredRenderDemos(const std::pair<std::size_t, std::size_t>& demos)
    {
        auto it = cachedfilteredDemos.find(demos);
        if (it != cachedfilteredDemos.end())
        {
            RenderDemos(it->second);
            return;
        }

        std::vector<std::filesystem::path> filteredDemos;
        for (std::size_t i = demos.first; i < demos.second; i++)
        {
            try
            {
                if (searchBarText.empty() || DemoFilter(demoPaths[i].filename().u8string()))
                {
                    filteredDemos.push_back(demoPaths[i]);
                }
            }
            catch (std::exception&)
            {
                LOG_ERROR("Error filtering demoPath");
                // catching errors just in case
            }
        }
        totalCachedFilteredDemosCount += filteredDemos.size();
        cachedfilteredDemos[demos] = filteredDemos;
        RenderDemos(filteredDemos);
    }


    void DemoLoader::RenderDir(const DemoDirectory& dir)
    {
        try
        {
            if (ImGui::TreeNodeEx(dir.path.filename().string().c_str(),
                                  searchBarText.empty() ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (auto i = dir.subdirectories.first; i < dir.subdirectories.second; i++)
                {
                    if (demoDirectories[i].relevant)
                    {
                        RenderDir(demoDirectories[i]);
                    }
                }

                FilteredRenderDemos(dir.demos);

                ImGui::TreePop();
            }
        }
        catch (std::exception&)
        {
			ImGui::BeginDisabled();
			ImGui::TreeNode("<invalid directory name>");
            ImGui::TreePop();
			ImGui::EndDisabled();
		}
    }

    void DemoLoader::RecacheSearchBarTextSplit()
    {
        searchBarTextSplit.clear();

        std::stringstream ss(searchBarText);
        std::string token;

        while (std::getline(ss, token, ' '))
        {
            searchBarTextSplit.push_back(std::u8string(token.begin(), token.end()));
        }
    }

    void DemoLoader::RenderSearchBar()
    {
        int flags = ImGuiInputTextFlags_CallbackResize;
        bool disableInputText =
            Mod::GetGameInterface()->IsConsoleOpen() ||
            (UI::UIManager::Get().IsControllableCameraModeSelected() && UI::UIManager::Get().GetUIComponent(UI::Component::GameView)->HasFocus());
        if (disableInputText)
        {
            ImGui::BeginDisabled();
            flags = flags | ImGuiInputTextFlags_ReadOnly;
        }
        
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (focusSearch && !disableInputText)
            ImGui::SetKeyboardFocusHere();
        focusSearch = false;
        ImGui::InputTextWithHint(
            "##SearchInput", "Search...", &searchBarText[0], searchBarText.capacity() + 1, flags,
            [](ImGuiInputTextCallbackData* data) -> int
            {
                if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
                {
                    std::string* str = reinterpret_cast<std::string*>(data->UserData);
                    str->resize(data->BufTextLen);
                    data->Buf = &(*str)[0];
                }
                return 0;
            },
            &searchBarText);

        if (disableInputText)
        {
            ImGui::EndDisabled();
        }

        if (lastSearchBarText != searchBarText)
        {
            RecacheSearchBarTextSplit();
            cachedfilteredDemos.clear();
            totalCachedFilteredDemosCount = 0;
            lastSearchBarText = searchBarText;
        }
    }

    void DemoLoader::RenderSearchPaths()
    {
        auto& addedDirectories = PreferencesConfiguration::Get().additionalDemoSearchDirectories;

        for (auto i = searchPaths.first; i < searchPaths.second; i++)
        {
            const bool open = ImGui::TreeNodeEx(demoDirectories[i].path.string().c_str(),
                                                searchBarText.empty() ? ImGuiTreeNodeFlags_None
                                                                      : ImGuiTreeNodeFlags_DefaultOpen);

            const auto added = std::find(addedDirectories.begin(), addedDirectories.end(), demoDirectories[i].path);
            if (added != addedDirectories.end())
            {
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::Selectable(ICON_FA_TRASH " Remove search path"))
                    {
                        // the list is rebuilt once this frame is drawn, since a rescan clears it
                        directoryToRemove = demoDirectories[i].path;
                    }
                    ImGui::EndPopup();
                }
                ImGui::SetItemTooltip("Right-click to remove this search path");
            }

            if (open)
            {
                if (demoDirectories[i].relevant)
                {
                    for (auto j = demoDirectories[i].subdirectories.first; j < demoDirectories[i].subdirectories.second;
                         j++)
                    {
                        if (demoDirectories[j].relevant)
                        {
                            RenderDir(demoDirectories[j]);
                        }
                    }
                }
                else
                {
                    ImGui::Text("Search path is empty.");
                }

                FilteredRenderDemos(demoDirectories[i].demos);

                ImGui::TreePop();
            }
        }
    }

    void DemoLoader::Initialize()
    {
        FindAllDemos();
        LoadRecentDemos();
    }

    void DemoLoader::RenderRecentDemos()
    {
        if (recentDemos.size() > 0)
        {
            if (ImGui::TreeNodeEx("Recent", ImGuiTreeNodeFlags_DefaultOpen))
            {
                RenderDemos(recentDemos);

                ImGui::TreePop();
            }
        }
    }

    void DemoLoader::Render()
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoTitleBar;
        // stays behind the tool windows (Preferences, Player Loadouts...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        loadedFsGame = Mod::GetGameInterface()->GetLoadedFsGame();
        if (ImGui::Begin("Demos", nullptr, flags))
        {
            GettingStarted::DrawCard();
            ImGui::AlignTextToFramePadding();

            if (isScanningDemoPaths.load())
            {
                ImGui::Text("Searching for demo files...");
            }
            else
            {
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%d demos", searchBarText.empty() ? demoPaths.size() : totalCachedFilteredDemosCount);
                ImGui::SameLine();

                auto addPathButtonLabel = std::string(ICON_FA_FOLDER_OPEN " Add path");
                auto refreshButtonLabel = std::string(ICON_FA_ROTATE_RIGHT " Refresh");
                auto buttonSize = ImVec2(ImGui::GetFontSize() * 6.0f, ImGui::GetFontSize() * 1.75f);

                if (ImGui::GetWindowWidth() < buttonSize.x * 4)
                {
                    buttonSize = ImVec2(buttonSize.y, buttonSize.y);
                    addPathButtonLabel = addPathButtonLabel.substr(0, addPathButtonLabel.find(" "));
                    refreshButtonLabel = refreshButtonLabel.substr(0, refreshButtonLabel.find(" "));
                }

                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - buttonSize.x * 2 - ImGui::GetStyle().ItemSpacing.x -
                                     ImGui::GetStyle().WindowPadding.x);
                
                // the search paths are saved in the config right away, so they come back when the mod starts again
                auto& addedDirectories = PreferencesConfiguration::Get().additionalDemoSearchDirectories;
                bool rescan = false;

                if (ImGui::Button(addPathButtonLabel.c_str(), buttonSize))
                {
                    auto path = PathUtils::OpenFolderBrowseDialog();
                    if (path.has_value() &&
                        std::find(addedDirectories.begin(), addedDirectories.end(), path.value()) == addedDirectories.end())
                    {
                        addedDirectories.push_back(path.value());
                        Configuration::Get().Write(true);
                        rescan = true;
                    }
                }

                ImGui::SameLine();

                if (ImGui::Button(refreshButtonLabel.c_str(), buttonSize))
                {
                    rescan = true;
                }

                if (loadedFsGame)
                    RenderModLine(*loadedFsGame);
                RenderLoadNotice();
                // Search paths will always be rendered, even if empty
                RenderSearchBar();
                RenderRecentDemos();
                RenderSearchPaths();

                if (directoryToRemove.has_value())
                {
                    std::erase(addedDirectories, directoryToRemove.value());
                    directoryToRemove.reset();
                    Configuration::Get().Write(true);
                    rescan = true;
                }

                // the scan clears the lists drawn above, so it only starts once they have been drawn
                if (rescan)
                {
                    FindAllDemos();
                }
            }
        }
        ImGui::End();

        // (playing reorders the recent demos, which may have been drawn)
        if (demoToPlay)
        {
            Play(*demoToPlay);
            demoToPlay.reset();
        }
    }

    void DemoLoader::Release()
    {
    }
}  // namespace IWXMVM::UI
