#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include "Settings.hpp"
#include "UI/Palettes.hpp"

namespace Launcher
{
    // What the command line sets (Main.cpp): folders other than the usual ones, for tests, and the mockup's states
    struct Options
    {
        std::filesystem::path home;       // the launcher's folder: launcher.json, T5iris.dll, tools
                                          // (%LOCALAPPDATA%\IrisStudio)
        std::filesystem::path storage;    // the mod's folder the zones and pack go into (Plutonium's storage\t5\iris)
        std::filesystem::path plutonium;  // Plutonium's folder (%LOCALAPPDATA%\Plutonium)
        std::string demo;                 // a state shown with the mockup's data (first, asking, setup, ready, ...)
        std::string palette;              // a palette for this run only
        std::filesystem::path shot;       // where to save a PNG of the window, then quit
        float scale = 0.0f;               // the UI's scale; 0 for the monitor's
        bool noSteam = false;             // not looking in Steam's libraries (tests of a machine without it)
        std::filesystem::path oatZip;     // a local oat-windows.zip instead of the download (tests; Linker.exe's
                                          // SHA-256 is still checked, the zip's isn't)
        std::filesystem::path ffmpegZip;  // a local zip with bin\ffmpeg.exe instead of FFmpeg's download (tests)
        bool ownTools = false;            // only the launcher's own OpenAssetTools and FFmpeg count (tests)
        bool headlessSetup = false;       // --setup: sets up without a window, then quits (tests)
        bool rebuild = false;             // with --setup: every zone and the pack, not only what is missing
        bool download = false;            // with --setup: downloads OpenAssetTools if it isn't there
        float cancelAfter = 0.0f;         // with --setup: cancels after this many seconds (tests)
        std::string press;                // presses a button once the first detection is done (setup, launch)
        std::optional<std::string> mod;   // the Mod row's pick for this run (tests): a mod's name, empty for none
        bool noUpdateCheck = false;       // doesn't ask GitHub for the latest release, whatever the setting
        bool migrateOnly = false;         // --migrate: moves the folders from their old names, then quits
        float shotDelay = 0.0f;           // with --shot: seconds to wait after the first detection
        std::string betaKey;              // what Paste key reads instead of the clipboard (tests of a beta build)
        bool noKeyCheck = false;          // a beta build with the test key pair: no key screen, so a launch reaches
                                          // the mod's own check (tests)
        std::string buildTextures;        // --build-textures: the zones of a texture pack built without a window
        std::filesystem::path importTextures;  // --import-textures: a folder imported without a window
        std::uint32_t textureScale = 4;   // with --build-textures
        std::size_t textureLimit = 0;     // with --build-textures: at most so many textures a map (tests)
        bool checkTextures = false;       // --check-textures: the pack checked against the game's, without a window
        bool repairTextures = false;      // --repair-textures: and the wrong images made again
    };

    // what the buttons do
    enum class Action
    {
        None,
        SetUp,    // the summary of a setup
        Accept,   // the setup it sums up
        Launch,
        Cancel,   // a setup running, or the summary
        BrowseGame,
        GetPlutonium,
        OpenPlutonium,
        OpenLog,
        Rebuild,  // the summary of a setup that builds everything again
        OpenModFolder,
        RemoveFiles,
        PasteKey,  // a beta build's key, from the clipboard
        CopyPcCode,
    };

    // the big button: the accent's for what to do, a quiet one for what is going on
    enum class ButtonStyle
    {
        Accent,
        Quiet,
    };

    enum class ButtonIcon
    {
        None,
        Play,
        Busy,
        Done,
        Paste,
    };

    // The home view (the logo, a line only when needed, one button) or the summary of a setup
    struct Home
    {
        std::string line;
        bool lineBad = false;
        bool lineGood = false;  // with a check (a beta key that worked)

        // a beta build without a key that works: the PC code with Copy above the line
        bool keyScreen = false;
        std::string pcCode;  // "KZ18-TW01"; empty when it couldn't be read
        bool copied = false;
        std::string link;  // after the line, when not empty (Open log)
        Action linkAction = Action::None;

        bool progress = false;  // a setup's progress in the line's place
        std::string progressLabel, progressCount;
        float progressFraction = 0.0f;

        ButtonStyle button = ButtonStyle::Accent;
        std::string buttonLabel;
        Action action = Action::None;
        ButtonIcon icon = ButtonIcon::None;
        bool enabled = true;

        // the Mod row above the button, when the mods folders have a mod: the pick
        // (a mod's name, empty for No mod), and the mods with the folder each is in
        bool modRow = false;
        std::string gameMod;
        std::vector<std::pair<std::string, std::string>> mods;

        bool summary = false;  // the summary instead
        std::string gameFolder;      // the Black Ops folder it builds from (and starts the game from): its name, and
                                     // "(Steam)" for Steam's copy; with Change
        std::string gameFolderPath;  // the whole path, shown under the mouse
        std::string downloads, downloadsSize;
        std::string builds, buildsSize;
        std::string spaceNeeded;
        std::string duration;  // "Takes about 3 minutes."
    };

    class Controller;
    class TexturePack;
    struct TextureView;

    class App
    {
       public:
        explicit App(Options options);
        ~App();

        // the fonts at a scale, into ImGui's atlas (the caller makes the device objects again)
        void LoadFonts(float scale);
        // draws a frame of the whole window
        void Frame(float width, float height);
        // whether something moves on screen (a spinner, the progress), so frames are drawn without input
        bool Animating() const;
        // whether a point of the window (in pixels) is where it is dragged from: the title bar but its buttons
        bool IsDragArea(float x, float y) const;
        // the window came to the front
        void Activated();
        // whether what is shown is settled: the first detection done (--shot waits for it)
        bool Settled() const;
        // seconds since it settled, and how long --shot waits after that
        double SettledFor() const;
        float ShotDelay() const
        {
            return options.shotDelay;
        }

        bool WantsClose() const
        {
            return wantsClose;
        }
        bool TakeMinimize()
        {
            return std::exchange(wantsMinimize, false);
        }
        const IWXMVM::UI::Palettes::Palette& Palette() const;
        float Scale() const
        {
            return scale;
        }

       private:
        struct Fonts
        {
            ImFont* regular12 = nullptr;
            ImFont* regular12_5 = nullptr;
            ImFont* regular13 = nullptr;
            ImFont* semibold12 = nullptr;
            ImFont* semibold12_5 = nullptr;
            ImFont* semibold13 = nullptr;
            ImFont* semibold15 = nullptr;
            ImFont* semibold16 = nullptr;
            ImFont* semibold17 = nullptr;
            ImFont* semibold18 = nullptr;
            ImFont* semibold19 = nullptr;
            ImFont* icons13 = nullptr;
            ImFont* icons14 = nullptr;
        };

        void DrawTitleBar(float width);
        void DrawHome(const Home& home, float width, float height);
        void DrawModRow(const Home& home, ImVec2 min, ImVec2 max);
        void PickMod(const std::string& name);
        void DrawSummary(const Home& home, float width, float height);
        void DrawSettings(float width, float height);
        void DrawTexturePack(const TextureView& view, float width, float height);
        void OpenTexturePack();
        void Do(Action action);
        Home DemoHome(const std::string& state) const;
        void SaveSettings();

        Options options;
        Settings settings;
        std::filesystem::path settingsFile;
        std::unique_ptr<Controller> controller;
        Fonts fonts;
        float scale = 1.0f;
        bool settingsOpen = false;
        bool textureOpen = false;  // Settings > Texture pack
        std::unique_ptr<TexturePack> texturePack;
        std::string textureDetail;     // Settings' line for it
        float partsScroll = 0.0f;      // the summary's list of maps, scrolled
        std::set<std::string> openGroups;  // its dropdowns open (Maps, Campaign, Zombies)
        bool repairPressed = false;
        std::size_t packsFirst = 0;        // the Done page's list of packs, its first row shown
        std::string removeArmedId;     // a pack whose remove asks for a second click until removePackUntil
        double removePackUntil = 0.0;
        bool wantsClose = false;
        bool wantsMinimize = false;
        bool animating = false;
        std::vector<std::pair<ImVec2, ImVec2>> titleButtons;  // the title bar's buttons, not dragged from
        float titleHeight = 0.0f;
        char playerName[64]{};
        double removeArmedUntil = 0.0;  // Remove the mod's files asks for a second click until then
        double copiedUntil = 0.0;       // the PC code's Copy says Copied until then
        double settledAt = -1.0;        // when the first detection was done
        bool pressed = false;           // --press done
        bool openModList = false;       // --press mod: the Mod row's list opens
    };
}  // namespace Launcher
