#pragma once
#include <mutex>

#include "UI/UIComponent.hpp"
#include "UI/DemoLoadLogic.hpp"
#include "UI/ModSwitchLogic.hpp"
#include "Components/DemoModLogic.hpp"

namespace IWXMVM::UI
{
    class DemoLoader : public UIComponent
    {
       public:
        void Render() final;
        void Release() final;

        // Plays a demo, which becomes the first of the recent ones: its notice is
        // drawn first, and every PLAY waits until it plays or the game is back at the menu without it (see
        // DemoLoadLogic). False, doing nothing, while another one loads.
        bool Play(const std::filesystem::path& path);
        // a frame of a demo loading, every frame whatever the panel shows: Update, before anything is drawn, sees how
        // it goes; Send, once the frame is drawn, sends its command when its notice has been drawn (as the PLAY
        // button did, after drawing)
        void Update();
        void Send();
        bool IsLoading() const
        {
            return loading.has_value() || modSwitch.has_value();
        }
        // Switches the game's mod: its name, empty for none, and a demo to play
        // once it runs. False, doing nothing, while a demo loads or a switch runs.
        bool SwitchMod(const std::string& modName, std::optional<std::filesystem::path> thenPlay = std::nullopt);
        // the mod a switch goes to ("" for none) while one runs
        std::optional<std::string> GetSwitchingMod() const
        {
            return modSwitch ? std::optional(std::string(Components::DemoModLogic::GetModName(modSwitch->fsGame)))
                             : std::nullopt;
        }
        // a demo's fs_game as the list read it ("" for none); none until it has been read
        std::optional<std::string> GetDemoFsGame(const std::filesystem::path& path);
        // the fs_game the last demo that didn't load needs, when the game wouldn't play it with the mod it runs
        std::optional<std::string> GetFailedNeeds() const
        {
            return failed ? failedNeeds : std::nullopt;
        }
        // the mods of the game's mods folders, as the last scan found them
        std::vector<Components::DemoModLogic::GameMod> GetMods();
        // the demo loading (its file name), and the last that didn't load with why
        std::optional<std::string> GetLoadingDemo() const
        {
            return loading ? std::optional(loading->load.name) : std::nullopt;
        }
        std::optional<std::pair<std::string, std::string>> GetFailedDemo() const
        {
            return failed ? std::optional(std::pair(failed->first.filename().string(), failed->second)) : std::nullopt;
        }
        const std::deque<std::filesystem::path>& GetRecentDemos() const
        {
            return recentDemos;
        }
        // puts the keyboard in the search field when it is drawn next
        void FocusSearch()
        {
            focusSearch = true;
        }

       private:
        struct DemoDirectory
        {
            std::filesystem::path path;  // Path
            std::pair<std::size_t, std::size_t>
                subdirectories;  // Pair of indices representing the [first, second) interval of directories in the
                                 // 'demoDirectories' vector
            std::pair<std::size_t, std::size_t>
                demos;  // Pair of indices representing the [first, second) interval of demos in the 'demoPaths' vector
            std::optional<std::size_t> parentIdx;  // Parent directory's index in the 'demoDirectories' vector. Search
                                                   // paths will contain a nullopt value
            bool relevant;                         // Does this directory ever reach a demo down the line?
        };

        void Initialize() final;
        void AddPathsToSearch(const std::vector<std::filesystem::path>& dirs);
        void SearchDir(std::size_t dirIdx);  // Recursive search function
        void Search();
        void MarkDirsRelevancy();
        void FindAllDemos();
        void SaveRecentDemos();
        void LoadRecentDemos();

        template <typename T>
        void RenderDemos(const T& demos);
        bool DemoFilter(const std::u8string& demoFileName);
        void FilteredRenderDemos(const std::pair<std::size_t, std::size_t>& demos);
        void RenderDir(const DemoDirectory& dir);  // Recursive render function
        void RecacheSearchBarTextSplit();
        void RenderSearchBar();
        void RenderRecentDemos();
        void RenderSearchPaths();
        void RenderLoadNotice();
        void RenderModLine(const std::string& loadedFsGame);
        void RenderModTag(const std::string& fsGame, const std::string& loadedFsGame);
        void Fail(std::string reason);
        void ReadDemoMods(std::vector<std::filesystem::path> demos);
        void UpdateModSwitch();

        std::pair<std::size_t, std::size_t> searchPaths;  // Pair of indices representing the [first, second) interval
                                                          // of search paths in the 'demoDirectories' vector
        std::vector<DemoDirectory> demoDirectories;
        constexpr static std::size_t maxRecentDemos = 5;
        std::deque<std::filesystem::path> recentDemos;
        std::vector<std::filesystem::path> demoPaths; // Path to every demo found
        std::atomic<bool> isScanningDemoPaths;
        std::optional<std::filesystem::path> directoryToRemove;
        // a demo whose play button was pressed, played once the lists (the recent one among them) are drawn
        std::optional<std::filesystem::path> demoToPlay;
        // the demo loading, and the last one that didn't load with why, shown until the next PLAY or closed
        struct Loading
        {
            std::filesystem::path path;
            DemoLoadLogic::Load load;
        };
        std::optional<Loading> loading;
        bool sendNow = false;
        std::optional<std::pair<std::filesystem::path, std::string>> failed;
        // a failed demo the game won't play with the mod it runs: the fs_game it needs, for the notice's buttons
        std::optional<std::string> failedNeeds;
        // the game's mod switched, then the demo to play; a switch that failed, by the mod's name
        std::optional<ModSwitchLogic::Switch> modSwitch;
        std::optional<std::filesystem::path> playAfterSwitch;
        bool sendModNow = false;
        std::optional<std::string> switchFailed;
        bool switchFailedUnload = false;
        std::string switchFrom;  // the mod the game ran as the switch started
        // the demos' fs_game, read on the scan's thread, by path with the size and time it was read at
        struct DemoMod
        {
            std::uintmax_t size = 0;
            std::filesystem::file_time_type time;
            std::optional<std::string> fsGame;
        };
        std::mutex demoModsMutex;
        std::unordered_map<std::wstring, DemoMod> demoMods;
        std::vector<Components::DemoModLogic::GameMod> mods;
        // the fs_game the game ran last frame, drawn with this frame's list
        std::optional<std::string> loadedFsGame;
        bool focusSearch = false;

        std::string searchBarText;
        std::string lastSearchBarText;
        std::vector<std::u8string> searchBarTextSplit;
        struct cachedfilteredDemos_pairhash
        {
            size_t operator()(const std::pair<size_t, size_t>& p) const
            {
                size_t h1 = std::hash<size_t>{}(p.first);
                size_t h2 = std::hash<size_t>{}(p.second);
                return h1 ^ (h2 << 1);
            }
        };
        std::unordered_map<std::pair<size_t, size_t>, std::vector<std::filesystem::path>, cachedfilteredDemos_pairhash> cachedfilteredDemos;
        std::size_t totalCachedFilteredDemosCount;
    };
}  // namespace IWXMVM::UI
