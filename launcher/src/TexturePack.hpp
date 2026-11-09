#pragma once

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "App.hpp"
#include "Components/TextureLogic.hpp"

namespace Launcher
{
    // Iris Studio's Texture pack: Settings > Texture pack. The summary of a build (the maps
    // to tick with their size, the scale, the model, the downloads, the space and the time), its progress, and Done
    // (each built map and imported pack with a tick, Use for: Maps / Players / Weapons, Add maps, Import...). The build
    // reads the user's own game files and writes the pack into the mod's folder (textures\v1); nothing is uploaded, and
    // no pack is ever downloaded (AH2): only the tools, pinned and checked (AI1).

    enum class TexturePage
    {
        Reading,   // the game's maps and textures read the first time
        Summary,   // what a build would do
        Building,
        Done,      // the packs there are, their ticks and Use for
        Import,    // an imported folder's summary
    };

    // what the page draws
    struct TextureView
    {
        TexturePage page = TexturePage::Reading;
        std::string line;  // Reading's step, or a problem (drawn in the bad color when bad)
        bool lineBad = false;
        float fraction = 0.0f;

        // Summary: the maps (zone, name, size alone at the scale, built already), in their dropdown: Maps (multiplayer),
        // Campaign or Zombies; none for the guns and hands
        struct Part
        {
            std::string zone, name, size;
            bool ticked = false;
            bool built = false;
            std::string group;
        };
        std::vector<Part> parts;
        std::uint32_t scale = 4;
        std::string model, downloads, space, time;
        bool spaceBad = false;
        bool canBuild = false;
        bool canCancel = false;  // back to Done, when there is a pack

        // Building
        std::string step, count, then;

        // Done: each pack, ticked when on
        struct Pack
        {
            std::string id;  // the zone, or "imported:<name>"
            std::string name, detail;
            bool on = true;
        };
        std::vector<Pack> packs;
        std::set<IWXMVM::Components::TextureLogic::Group> groupsOff;
        // and a pack of an older build: its check running (the line says how far), the images it found wrong and
        // "5,366 textures wrong · about 45 minutes"
        bool checking = false;
        std::size_t wrong = 0;
        std::string repairLine;

        // Import
        std::string folder, fits, leftOut, overBuilt;
        bool canImport = false;
    };

    // The page's work, on a thread of its own: reading the game, building, importing
    class TexturePack
    {
       public:
        TexturePack(const Options& options, std::filesystem::path gameFolder);
        ~TexturePack();

        TextureView View() const;
        bool Busy() const;

        void Tick(const std::string& zone);
        // every map of a dropdown ticked, or none when they all were
        void TickGroup(const std::string& group);
        void SetScale(std::uint32_t scale);
        void Build();
        // Images of an older build against the game's (TextureBuild::IsWrong), then those found wrong made again, the
        // rest of the pack left as it is. The page checks by itself once it has read the
        // game (AR1). logAll: only logged, every image with its numbers, nothing marked
        void CheckPack(bool logAll = false);
        void Repair();
        std::size_t CountWrong() const;
        // at most so many textures a map (tests)
        void SetLimit(std::size_t limit);
        void Stop();
        // Summary's Cancel: back to Done
        void Cancel();
        void AddMaps();
        void PickImport();
        void ReadImport(const std::filesystem::path& folder);
        void Log(std::string_view text);
        void Import();
        void CancelImport();
        void TogglePack(const std::string& id);
        void ToggleGroup(IWXMVM::Components::TextureLogic::Group group);
        void Remove(const std::string& id);

        // Settings' line for the row: "Nuketown 4x, Guns and hands 4x", "None built"
        static std::string Describe(const std::filesystem::path& storage);

       private:
        struct Work;
        std::shared_ptr<Work> work;
    };

    // the mockup's states for --demo texture-summary, texture-building, texture-done, texture-import
    TextureView DemoTextureView(const std::string& state);

    // --build-textures: the zones' pack built without a window, --check-textures or --repair-textures: its images of
    // an older build checked, and those found wrong made again; 0 when it was, 1 when it failed or stopped, 2 when
    // there is no game to build from
    int RunTextureBuild(const Options& options);
}  // namespace Launcher
