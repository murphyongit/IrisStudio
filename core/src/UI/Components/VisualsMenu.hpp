#pragma once
#include <set>
#include "UI/UIComponent.hpp"
#include "glm/vec3.hpp"
#include "Types/dof.hpp"
#include "Types/Filmtweaks.hpp"
#include "Components/VisualConfiguration.hpp"
#include "Components/VisualsLogic.hpp"
#include "Components/QualityLogic.hpp"
#include "Components/ReShadeLogic.hpp"

namespace IWXMVM::UI
{
    class VisualsMenu : public UIComponent
    {
       public:
        void Render() final;
        void Release() final;

        // the settings as the tab has them (keyed ones as they were applied last)
        const Components::VisualConfiguration::Settings& GetSettings() const
        {
            return visuals;
        }
        // what Reset goes back to, the map's (the harness's)
        const Components::VisualConfiguration::Settings& GetDefaults() const
        {
            return defaultVisuals;
        }
        // Loads a preset file (nothing: the Default preset) as its combo does, once the tab draws next
        void RequestPreset(std::optional<std::filesystem::path> path);
        // opens or closes a fold of the ReShade section, by its technique's id (the harness's)
        void SetReShadeFoldOpen(const std::string& techniqueId, bool open);
        // sets Surface depth's rows as the tab would, a step of the undo history (the harness's)
        void SetSurface(bool on, float depth, float bumpStrength);
        // shows a property's row when the tab draws next: its section open, scrolled to and flashing (a key picked on
        // the timeline)
        void ShowRow(Types::KeyframeablePropertyType type);
        // opens or closes a section, saved in config.json
        void SetSectionOpen(Components::VisualsLogic::Section section, bool open);

       private:
        struct Preset
        {
            std::string name;
            std::filesystem::path path;
        };
        std::vector<Preset> recentPresets;

        void Initialize() final;
        // A section's band: folds, as config.json's lookSectionsOpen has it; says whether
        // it's open. With a tooltip, Reset on the band, working closed, sets *reset when pressed; closed, its marks
        // (GetSectionMarks). What's drawn after it when open ends with EndSection.
        bool DrawSection(Components::VisualsLogic::Section section, const char* label,
                         const char* resetTooltip = nullptr, bool* reset = nullptr);
        void EndSection(Components::VisualsLogic::Section section);
        Components::VisualsLogic::SectionMarks GetSectionMarks(Components::VisualsLogic::Section section);
        // opens the section of the row ShowRow asked for and flashes the row, as the tab draws
        void ShowRequestedRow();

        void RenderConfigSection();
        void RenderHud();
        // The Map Effects section: on a campaign level, its ambient effects by definition,
        // each shown until turned off, which is a step of the undo history and kept for the level in config.json
        void RenderMapEffects();
        void SetHiddenMapEffects(const std::string& level, const std::set<std::string>& hidden);
        void RenderDOF();
        void RenderSun();
        // The Sky section (Features_Sky): the map's sky turned (yaw, pitch, roll) and graded (hue, saturation,
        // brightness, contrast, tint), settings of the tab, keyed and in presets (Components::Skybox draws them)
        void RenderSky();
        // The Quality section (Features_ShadowQuality): Shadow distance, a setting of
        // the tab (keyed and in presets), the same while working and while Record runs; Shadow quality this PC's, with
        // a value for each (Components::QualityLogic); with Features_SurfaceDepth, Surface depth's On or Off, its Depth
        // and Bump strength, settings of the tab as Shadow distance is
        void RenderQuality();
        // its Surface depth rows, in the section's columns
        void RenderSurfaceRows(float x, float width);
        void RenderTextureRow(float x, float width);
        void RenderFilmtweaks();
        // The ReShade section (under Filmtweaks): ReShade's effects as folds, each with its
        // on/off and its values as rows, keyed as the other rows are; the effects on or keyed first, then All effects
        void RenderReShade();
        void RenderReShadeFold(const ReShade::Effects& effects, const ReShade::Values& values,
                               const Components::ReShadeLogic::Fold& fold);
        void RenderReShadeMissing(const Components::ReShadeLogic::Missing& missing);
        bool RenderFilmtweakRanges();
        bool RenderFilmtweakRange(std::optional<std::size_t> range);  // nullopt edits all ranges

        void UpdateDof();
        void UpdateSun();
        void UpdateFilmtweaks();
        void UpdateHudInfo();
        void UpdateAll();
        // sm_sunSampleSizeNear for the Shadow distance with the shadow map's size now; each frame, as the size changes
        // with a restart of the renderer (a capture's, Apply)
        void UpdateShadows();
        // the Scene shadow quality, if the map isn't of it: the renderer restarts (about 0.6 s)
        void ApplySceneShadows();
        // Surface depth's rows, to the game's drawing (Components::SurfaceDepth); each frame, as keys change them
        void UpdateSurfaces();
        // the Sky's grade, to the game's drawing (Components::Skybox); each frame, as keys change it
        void UpdateSky();
        void SetQuality(const Components::QualityLogic::Settings& settings);

        // Keyed settings take their keys' values at the playhead, those the tab doesn't show now too (a range's tab
        // shows only its own); each frame
        void ApplyKeyed();
        // The defaults for the map loaded, as the game has them: settings still at the last map's follow it, and
        // those changed stay, given to the game again, as a world's load sets the sun's dvars back
        void TakeMapDefaults();

        // Changes of the settings are steps of the undo history: what a frame of
        // the tab changed (but keyed settings, which their keys' steps change) goes into an edit
        // (VisualsLogic::Edit), named after the control changed or what the tab says (a preset, a reset), until no
        // control is active (a drag, typing), then becomes one step
        void TrackEdit(const Components::VisualConfiguration::Settings& frameStart, const Preset& presetBefore);
        void EndEditWhenIdle();
        // names the change this frame makes, with a message once it is a step ("Filmtweaks reset")
        void NameChange(std::string label, std::string message);

        void LoadPreset(Preset);
        void LoadDefaultPreset();
        void AddPresetToRecent(Preset);

        Components::VisualConfiguration::Settings visuals;
        Components::QualityLogic::Settings quality;  // as config.json has it
        Components::VisualConfiguration::Settings defaultVisuals;
        // what keys have set since the last map's defaults were taken: those settings aren't the user's
        std::set<Types::KeyframeablePropertyType> keyedSettings;
        bool visualsInitialized = false;
        Preset defaultPreset;
        Preset currentPreset;
        // a preset the harness asked for (nothing inside: the Default preset)
        std::optional<std::optional<std::filesystem::path>> requestedPreset;

        Components::VisualsLogic::Edit edit;
        // the preset chosen before and after the edit, when it chose one
        std::optional<std::pair<Preset, Preset>> editPresets;
        std::optional<std::string> editMessage;
        // what the tab named the change of this frame, and its message
        std::optional<std::string> namedChange;
        std::optional<std::string> namedMessage;

        // the ReShade section's folds open, by their technique's id
        std::set<std::string> openReShadeFolds;
        // the effects it lists first, until a demo loads (ReShadeLogic::GetFolds)
        std::set<std::string> keptReShadeEffects;

        // the section just opened, scrolled into view once it's drawn, and where the one being drawn starts
        std::optional<Components::VisualsLogic::Section> sectionToShow;
        float sectionTop = 0.0f;
        // what ShowRow asked for, and the id of a ReShade row, whose fold opens
        std::optional<Types::KeyframeablePropertyType> requestedRow;
        std::optional<std::string> reShadeRowToShow;
    };
}  // namespace IWXMVM::UI
