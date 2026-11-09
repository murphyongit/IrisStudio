#include "StdInclude.hpp"
#include "PreferencesConfiguration.hpp"

namespace IWXMVM
{
    PreferencesConfiguration::PreferencesConfiguration()
    {
    }

    void PreferencesConfiguration::Deserialize(const nlohmann::json& j)
    {
        if (!j.is_object())
        {
            throw std::runtime_error("Failed to deserialize preferences configuration: root node is not an object");
        }

        Configuration::ReadValueInto<bool>(j, NODE_SHOW_KEYBIND_HINTS, showKeybindHints);
        Configuration::ReadValueInto<std::string>(j, NODE_PALETTE, palette);
        if (j.contains(NODE_GAME_MOD) && j[NODE_GAME_MOD].is_string())
            gameMod = j[NODE_GAME_MOD].get<std::string>();
        Configuration::ReadValueInto<float>(j, NODE_UI_SCALE, uiScale);
        if (j.contains(NODE_VIEW_LINES))
        {
            Configuration::ReadValueInto<float>(j[NODE_VIEW_LINES], "width", viewLineWidth);
            Configuration::ReadValueInto<float>(j[NODE_VIEW_LINES], "strength", viewLineStrength);
        }
        if (j.contains(NODE_LAYOUT) && j[NODE_LAYOUT].is_object())
        {
            Configuration::ReadValueInto<float>(j[NODE_LAYOUT], "panel", layoutPanel);
            Configuration::ReadValueInto<float>(j[NODE_LAYOUT], "timeline", layoutTimeline);
            Configuration::ReadValueInto<float>(j[NODE_LAYOUT], "timelineWithGraph", layoutTimelineWithGraph);
        }
        Configuration::ReadValueInto<bool>(j, NODE_SHOW_CAMPATH, showCampath);
        Configuration::ReadValueInto<bool>(j, NODE_SHOW_ACTOR_RINGS, showActorRings);
        Configuration::ReadValueInto<bool>(j, NODE_SHOW_ACTOR_PATHS, showActorPaths);
        Configuration::ReadValueInto<bool>(j, NODE_SHOW_SAFE_FRAMES, showSafeFrames);
        Configuration::ReadValueInto<bool>(j, NODE_DEVELOPER_TOOLS, developerTools);
        if (j.contains(NODE_GETTING_STARTED) && j[NODE_GETTING_STARTED].is_object())
        {
            Configuration::ReadValueInto<std::vector<std::string>>(j[NODE_GETTING_STARTED], "done", gettingStartedDone);
            Configuration::ReadValueInto<bool>(j[NODE_GETTING_STARTED], "hidden", gettingStartedHidden);
        }
        Configuration::ReadValueInto<float>(j, NODE_FREECAM_SPEED, freecamSpeed);
        Configuration::ReadValueInto<float>(j, NODE_FREECAM_MOUSE_SPEED, freecamMouseSpeed);
        Configuration::ReadValueInto<float>(j, NODE_ORBIT_ROTATION_SPEED, orbitRotationSpeed);
        Configuration::ReadValueInto<float>(j, NODE_ORBIT_MOVE_SPEED, orbitMoveSpeed);
        Configuration::ReadValueInto<float>(j, NODE_ORBIT_ZOOM_SPEED, orbitZoomSpeed);
        if (j.contains(NODE_AUTO_EASE) && j[NODE_AUTO_EASE].is_object())
        {
            Configuration::ReadValueInto<bool>(j[NODE_AUTO_EASE], "on", autoEase);
            Configuration::ReadValueInto<float>(j[NODE_AUTO_EASE], "start", autoEaseStart);
            Configuration::ReadValueInto<float>(j[NODE_AUTO_EASE], "end", autoEaseEnd);
            autoEaseStart = std::clamp(autoEaseStart, 0.0f, 1.0f);
            autoEaseEnd = std::clamp(autoEaseEnd, 0.0f, 1.0f);
            std::string nodes;
            Configuration::ReadValueInto<std::string>(j[NODE_AUTO_EASE], "nodes", nodes);
            autoEaseNodes =
                magic_enum::enum_cast<Types::AutoEaseNodes>(nodes).value_or(Types::AutoEaseNodes::FirstAndLast);
        }
        Configuration::ReadValueInto<std::filesystem::path>(j, NODE_CAPTURE_OUTPUT_DIRECTORY, captureOutputDirectory);
        Configuration::ReadValueInto<std::vector<std::filesystem::path>>(j, NODE_ADDITIONAL_DEMO_SEARCH_DIRECTORIES,
                                                                         additionalDemoSearchDirectories);
        Configuration::ReadValueInto<std::vector<std::filesystem::path>>(j, NODE_RECENT_VISUAL_PRESETS,
                                                                         recentVisualPresets);
        Configuration::ReadValueInto<std::vector<std::string>>(j, NODE_FAVORITE_DEATH_ANIMATIONS,
                                                               favoriteDeathAnimations);
        Configuration::ReadValueInto<std::vector<std::string>>(j, NODE_FAVORITE_ACTOR_ANIMATIONS,
                                                               favoriteActorAnimations);
        Configuration::ReadValueInto<std::vector<std::string>>(j, NODE_FAVORITE_CHARACTERS, favoriteCharacters);
        Configuration::ReadValueInto<std::vector<std::string>>(j, NODE_RECENT_ACTOR_ANIMATIONS, recentActorAnimations);
        Configuration::ReadValueInto<std::vector<std::string>>(j, NODE_FAVORITE_EFFECTS, favoriteEffects);
        Configuration::ReadValueInto<std::vector<std::string>>(j, NODE_RECENT_EFFECTS, recentEffects);
        Configuration::ReadValueInto<std::vector<std::string>>(j, NODE_RECENT_PROPS, recentProps);
        Configuration::ReadValueInto<std::vector<std::string>>(j, NODE_ACTORS_ADVANCED, actorsAdvancedOpen);
        Configuration::ReadValueInto<std::vector<std::string>>(j, NODE_LOOK_SECTIONS_OPEN, lookSectionsOpen);
        Configuration::ReadValueInto<std::vector<std::string>>(j, NODE_PANEL_SECTIONS_CLOSED, panelSectionsClosed);
        Configuration::ReadValueInto<nlohmann::json>(j, NODE_ACTOR_PRESETS, actorPresets);
        if (!actorPresets.is_object())
            actorPresets = nlohmann::json::object();
        Configuration::ReadValueInto<nlohmann::json>(j, NODE_RELOAD_PROFILES, reloadProfiles);
        if (!reloadProfiles.is_object())
            reloadProfiles = nlohmann::json::object();
        Configuration::ReadValueInto<nlohmann::json>(j, NODE_QUALITY, quality);
        if (!quality.is_object())
            quality = nlohmann::json::object();
        Configuration::ReadValueInto<nlohmann::json>(j, NODE_HIDDEN_MAP_EFFECTS, hiddenMapEffects);
        if (!hiddenMapEffects.is_object())
            hiddenMapEffects = nlohmann::json::object();
    }

    void PreferencesConfiguration::Serialize(nlohmann::json& j) const
    {
        j[NODE_SHOW_KEYBIND_HINTS] = showKeybindHints;
        j[NODE_PALETTE] = palette;
        if (gameMod)
            j[NODE_GAME_MOD] = *gameMod;
        j[NODE_UI_SCALE] = uiScale;
        j[NODE_LAYOUT] = {
            {"panel", layoutPanel}, {"timeline", layoutTimeline}, {"timelineWithGraph", layoutTimelineWithGraph}};
        j[NODE_SHOW_CAMPATH] = showCampath;
        j[NODE_SHOW_ACTOR_RINGS] = showActorRings;
        j[NODE_SHOW_ACTOR_PATHS] = showActorPaths;
        j[NODE_SHOW_SAFE_FRAMES] = showSafeFrames;
        j[NODE_VIEW_LINES] = {{"width", viewLineWidth}, {"strength", viewLineStrength}};
        j[NODE_DEVELOPER_TOOLS] = developerTools;
        j[NODE_GETTING_STARTED] = {{"done", gettingStartedDone}, {"hidden", gettingStartedHidden}};
        j[NODE_FREECAM_SPEED] = freecamSpeed;
        j[NODE_FREECAM_MOUSE_SPEED] = freecamMouseSpeed;
        j[NODE_ORBIT_ROTATION_SPEED] = orbitRotationSpeed;
        j[NODE_ORBIT_MOVE_SPEED] = orbitMoveSpeed;
        j[NODE_ORBIT_ZOOM_SPEED] = orbitZoomSpeed;
        j[NODE_AUTO_EASE] = {{"on", autoEase},
                             {"start", autoEaseStart},
                             {"end", autoEaseEnd},
                             {"nodes", std::string(magic_enum::enum_name(autoEaseNodes))}};
        j[NODE_CAPTURE_OUTPUT_DIRECTORY] = captureOutputDirectory;
        
        j[NODE_ADDITIONAL_DEMO_SEARCH_DIRECTORIES] = nlohmann::json::array();
        for (const auto& dir : additionalDemoSearchDirectories)
        {
			j[NODE_ADDITIONAL_DEMO_SEARCH_DIRECTORIES].push_back(dir);
        }

        j[NODE_RECENT_VISUAL_PRESETS] = nlohmann::json::array();
        for (const auto& preset : recentVisualPresets)
        {
            j[NODE_RECENT_VISUAL_PRESETS].push_back(preset);
        }

        j[NODE_FAVORITE_DEATH_ANIMATIONS] = favoriteDeathAnimations;
        j[NODE_FAVORITE_ACTOR_ANIMATIONS] = favoriteActorAnimations;
        j[NODE_FAVORITE_CHARACTERS] = favoriteCharacters;
        j[NODE_RECENT_ACTOR_ANIMATIONS] = recentActorAnimations;
        j[NODE_FAVORITE_EFFECTS] = favoriteEffects;
        j[NODE_RECENT_EFFECTS] = recentEffects;
        j[NODE_RECENT_PROPS] = recentProps;
        j[NODE_ACTORS_ADVANCED] = actorsAdvancedOpen;
        j[NODE_LOOK_SECTIONS_OPEN] = lookSectionsOpen;
        j[NODE_PANEL_SECTIONS_CLOSED] = panelSectionsClosed;
        j[NODE_ACTOR_PRESETS] = actorPresets;
        j[NODE_RELOAD_PROFILES] = reloadProfiles;
        j[NODE_QUALITY] = quality;
        j[NODE_HIDDEN_MAP_EFFECTS] = hiddenMapEffects;
    }
}  // namespace IWXMVM