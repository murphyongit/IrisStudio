#pragma once
#include "Configuration.hpp"
#include "Types/Keyframe.hpp"

namespace IWXMVM
{
    class PreferencesConfiguration
    {
       public:
        static PreferencesConfiguration& Get()
        {
            static PreferencesConfiguration instance;
            return instance;
        }

        PreferencesConfiguration(PreferencesConfiguration const&) = delete;
        void operator=(PreferencesConfiguration const&) = delete;

        void Deserialize(const nlohmann::json& j);
        void Serialize(nlohmann::json& j) const;


        bool showKeybindHints = true;

        // the editor's palette (UI::Palettes), as the launcher keeps it too
        std::string palette = "mono";

        // the game mod the Demo tab last switched to (its name, empty for none), which
        // the launcher starts the game with next; none until a switch
        std::optional<std::string> gameMod;

        // the UI's size: 1 sizes the font from the window's width, as it always was (see UI::LayoutMath)
        float uiScale = 1.0f;
        // what the layout's splitters were set to, in ems (see UI::LayoutMath)
        float layoutPanel = 26.0f;
        float layoutTimeline = 14.0f;
        float layoutTimelineWithGraph = 25.0f;
        // what the view shows besides the game (View menu)
        bool showCampath = true;     // the campath's nodes and line, in the free and orbit cameras
        bool showActorRings = true;  // the rings under actors, on the Actors tab
        bool showActorPaths = true;  // the paths of the actors that aren't selected, faint, on the Actors tab
        bool showSafeFrames = false;
        // the lines the mod draws over the view (View menu): how wide on the screen, how strong (0 to 1)
        float viewLineWidth = 2.0f;
        float viewLineStrength = 1.0f;
        // the ImGui demo and the debug panel in the Help menu
        bool developerTools = false;
        // the first-run card's steps done by name, and whether it was closed (see UI::GettingStarted)
        std::vector<std::string> gettingStartedDone;
        bool gettingStartedHidden = false;

        float freecamSpeed = 300.0f;
        float freecamMouseSpeed = 0.1f;

        float orbitRotationSpeed = 0.15f;
        float orbitMoveSpeed = 0.3f;
        float orbitZoomSpeed = 0.8f;

        // Auto Ease (see Components::KeyframeManager): whether new campaths ease out of
        // their first node and into their last (or of every node, 6.5), and by how much (0 to 1)
        bool autoEase = true;
        float autoEaseStart = 0.3f;
        float autoEaseEnd = 0.3f;
        Types::AutoEaseNodes autoEaseNodes = Types::AutoEaseNodes::FirstAndLast;

        std::filesystem::path captureOutputDirectory = std::filesystem::path();

        std::vector<std::filesystem::path> additionalDemoSearchDirectories;  // Directories added by the user, to be searched

        std::vector<std::filesystem::path> recentVisualPresets;  // Presets of the Visuals tab, the most recent last

        std::vector<std::string> favoriteDeathAnimations;  // by name, see Components::PlayerAnimation
        std::vector<std::string> favoriteActorAnimations;  // by name, see Components::Actors
        std::vector<std::string> favoriteCharacters;       // by id, see Components::Actors
        std::vector<std::string> recentActorAnimations;    // the latest first
        // the effects starred in the effect browser and those last added to the scene, the latest first, by name (see
        // Components::Actors)
        std::vector<std::string> favoriteEffects;
        std::vector<std::string> recentEffects;
        // the props last added to the scene, the latest first
        std::vector<std::string> recentProps;
        // the pages of an actor whose Advanced fold is open ("look", "clip", "movement", "event")
        std::vector<std::string> actorsAdvancedOpen;
        // the Look tab's sections open (Components::VisualsLogic::ReadOpenSections), none at first
        std::vector<std::string> lookSectionsOpen;
        // the Camera and Record tabs' sections closed, none at first
        std::vector<std::string> panelSectionsClosed;
        // presets of actors' looks and clips by name: {"looks": {name: look}, "clips": {name: clips}}, see
        // Components::Actors
        nlohmann::json actorPresets = nlohmann::json::object();
        // what the user says of reload animations, by animation (see Components::ReloadProfiles)
        nlohmann::json reloadProfiles = nlohmann::json::object();
        // this PC's values of the Look tab's Quality section, while working and while Record runs (see
        // Components::QualityLogic)
        nlohmann::json quality = nlohmann::json::object();
        // the map effects hidden in the Look tab, by level: {level: [definition...]}
        nlohmann::json hiddenMapEffects = nlohmann::json::object();

       private:
        PreferencesConfiguration();

        const std::string_view NODE_SHOW_KEYBIND_HINTS = "showKeybindHints";
        const std::string_view NODE_PALETTE = "palette";
        const std::string_view NODE_GAME_MOD = "gameMod";
        const std::string_view NODE_UI_SCALE = "uiScale";
        const std::string_view NODE_LAYOUT = "layout";
        const std::string_view NODE_SHOW_CAMPATH = "showCampath";
        const std::string_view NODE_SHOW_ACTOR_RINGS = "showActorRings";
        const std::string_view NODE_SHOW_ACTOR_PATHS = "showActorPaths";
        const std::string_view NODE_SHOW_SAFE_FRAMES = "showSafeFrames";
        const std::string_view NODE_VIEW_LINES = "viewLines";
        const std::string_view NODE_DEVELOPER_TOOLS = "developerTools";
        const std::string_view NODE_GETTING_STARTED = "gettingStarted";
        const std::string_view NODE_FREECAM_SPEED = "freecamSpeed";
        const std::string_view NODE_FREECAM_MOUSE_SPEED = "freecamMouseSpeed";
        const std::string_view NODE_ORBIT_ROTATION_SPEED = "orbitRotationSpeed";
        const std::string_view NODE_ORBIT_MOVE_SPEED = "orbitMoveSpeed";
        const std::string_view NODE_ORBIT_ZOOM_SPEED = "orbitZoomSpeed";
        const std::string_view NODE_AUTO_EASE = "autoEase";
        const std::string_view NODE_CAPTURE_OUTPUT_DIRECTORY = "captureOutputDirectory";
        const std::string_view NODE_ADDITIONAL_DEMO_SEARCH_DIRECTORIES = "additionalDemoSearchDirectories";
        const std::string_view NODE_RECENT_VISUAL_PRESETS = "recentVisualPresets";
        const std::string_view NODE_FAVORITE_DEATH_ANIMATIONS = "favoriteDeathAnimations";
        const std::string_view NODE_FAVORITE_ACTOR_ANIMATIONS = "favoriteActorAnimations";
        const std::string_view NODE_FAVORITE_CHARACTERS = "favoriteCharacters";
        const std::string_view NODE_RECENT_ACTOR_ANIMATIONS = "recentActorAnimations";
        const std::string_view NODE_FAVORITE_EFFECTS = "favoriteEffects";
        const std::string_view NODE_RECENT_EFFECTS = "recentEffects";
        const std::string_view NODE_RECENT_PROPS = "recentProps";
        const std::string_view NODE_ACTORS_ADVANCED = "actorsAdvanced";
        const std::string_view NODE_LOOK_SECTIONS_OPEN = "lookSectionsOpen";
        const std::string_view NODE_PANEL_SECTIONS_CLOSED = "panelSectionsClosed";
        const std::string_view NODE_ACTOR_PRESETS = "actorPresets";
        const std::string_view NODE_RELOAD_PROFILES = "reloadProfiles";
        const std::string_view NODE_QUALITY = "quality";
        const std::string_view NODE_HIDDEN_MAP_EFFECTS = "hiddenMapEffects";

    };
}  // namespace IWXMVM