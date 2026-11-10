#pragma once
#include "Components/VisualConfiguration.hpp"
#include "Types/Features.hpp"
#include "Types/Keyframe.hpp"

#include <set>
#include <span>

namespace IWXMVM::Components::VisualsLogic
{
    // The settings of the Visuals tab (the Look tab) one by one, apart from the game and the UI, so the tests in
    // tests/actors-tests build it on their own: what presets save them as, what the undo history calls changing them,
    // and the keyframeable property that animates each one that can be.
    using Settings = VisualConfiguration::Settings;

    enum class Kind
    {
        Switch,  // on or off: "turn Filmtweaks on"
        Shown,   // a part of the HUD shown or not: "hide Killfeed"
        Number,
        Values,  // three numbers
        Color,
    };

    struct Setting
    {
        std::string_view dvar;  // its name in a preset, after the game's dvar ("r_filmtweakbrightness")
        std::string_view name;  // in the undo history's steps ("Brightness", "Filmtweaks")
        Kind kind;
        // values in a preset: a color has a fourth, written as 1 and ignored when read, as the game's color dvars do
        std::size_t valueCount;
        std::optional<Types::KeyframeablePropertyType> property;  // the one that animates it
        std::function<void(Settings&, const float*)> read;
        std::function<void(const Settings&, float*)> write;  // valueCount values
    };

    // Every setting of the tab, in the order presets have them. Presets are read and written through this list, so a
    // setting added here is saved too.
    const std::vector<Setting>& GetSettings();
    // the setting a preset names, or a property animates
    const Setting* FindSetting(std::string_view dvar);
    const Setting* FindSetting(Types::KeyframeablePropertyType property);

    // whether a property animates one of the settings
    using IsKeyed = std::function<bool(Types::KeyframeablePropertyType)>;

    // The settings that differ between two (as indices of GetSettings), but those animated by a property keyed says
    // has keys, which the keys set
    std::vector<std::size_t> GetChanged(const Settings& before, const Settings& after, const IsKeyed& keyed = {});
    // Copies those settings from one to another, leaving the others
    void Copy(const Settings& from, Settings& to, std::span<const std::size_t> settings);
    // The settings on a new map: what was changed from the last map's defaults stays, the rest, and what keys
    // were setting, takes the new map's
    Settings FollowMap(const Settings& settings, const Settings& lastDefaults, const Settings& defaults,
                       const IsKeyed& keyed);

    // The value a property's key of the settings would have; nothing for a property animating none of them
    std::optional<Types::KeyframeValue> GetValue(const Settings& settings, Types::KeyframeablePropertyType property);
    // The settings with a property's value, as keys give it (a switch is on from a half up); false for a property
    // animating none of them
    bool SetValue(Settings& settings, Types::KeyframeablePropertyType property, const Types::KeyframeValue& value);

    // What the undo history calls a change of these settings: "turn Filmtweaks on" and "hide Killfeed" for a switch,
    // "change Far Blur" for one other setting, "change the visuals" for more than one
    std::string DescribeChange(std::span<const std::size_t> settings, const Settings& after);

    // The tab's sections, in the order it draws them: each folds, all closed at first
    enum class Section : std::uint8_t
    {
        Hud,
        MapEffects,
        Sun,
        Sky,
        Quality,
        Filmtweaks,
        ReShade,
        DepthOfField,
    };
    inline constexpr std::array SECTIONS = {Section::Hud,        Section::MapEffects, Section::Sun,
                                            Section::Sky,        Section::Quality,    Section::Filmtweaks,
                                            Section::ReShade,    Section::DepthOfField};
    // as config.json keeps it and the harness names it (visuals.section.<name>)
    std::string_view GetSectionName(Section section);
    // the section that shows a property's row, none for a property that isn't the tab's
    std::optional<Section> GetSection(Types::KeyframeablePropertyType property);
    // The sections open, as config.json's lookSectionsOpen has them; unknown names are dropped
    std::set<Section> ReadOpenSections(std::span<const std::string> names);
    std::vector<std::string> WriteOpenSections(const std::set<Section>& sections);

    // What a closed section's header shows: keys on a row of it, a value that isn't its default (what keys set isn't
    // counted: those show as keys)
    struct SectionMarks
    {
        bool keyed = false;
        bool changed = false;
    };
    SectionMarks GetMarks(Section section, const Settings& settings, const Settings& defaults, const IsKeyed& keyed);

    // Where the Sun's Brightness bar ends: at twice the map's, so the map's own sits inside it, and at least the 4 the
    // other games' suns fit
    float GetSunBrightnessMax(float mapBrightness);

    // Whether a game shows a control for a property: Black Ops' per range filmtweaks only with
    // Features_FilmtweakRanges, the one contrast, Brightness, Desaturation and Invert of the other games only without
    // (Black Ops registers their dvars but never reads them), Shadow Distance only with Features_ShadowQuality, Surface Depth and Bump Strength only with Features_SurfaceDepth, the Sky's only with Features_Sky; ReShade's values none (keyed in its own section)
    bool IsUsed(Types::KeyframeablePropertyType property, Types::Features features);

    // An edit of the settings on the tab: from the frame a control first changes them until no control is active (a
    // drag, typing, a click), what it changed, as one step of the undo history
    class Edit
    {
       public:
        struct Step
        {
            std::string label;
            std::vector<std::size_t> settings;  // what it changed, as indices of GetSettings
            Settings before;
            Settings after;
        };

        // A frame of the tab: the settings as they were when it began to draw and as it left them, those animated by
        // keyed properties left out. The first frame that changed one begins the edit, named label (as DescribeChange
        // names what it changed, without one). Returns whether it began now.
        bool Track(const Settings& frameStart, const Settings& now, const IsKeyed& keyed,
                   const std::optional<std::string>& label);
        bool IsOpen() const
        {
            return step.has_value();
        }
        // Ends the edit with the settings as they are now: its step, without the settings that are as they were
        // before it (nothing when none are left, a drag that went back)
        std::optional<Step> End(const Settings& now);

       private:
        std::optional<Step> step;
    };
}  // namespace IWXMVM::Components::VisualsLogic
