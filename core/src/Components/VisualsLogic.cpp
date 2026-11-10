#include "StdInclude.hpp"
#include "VisualsLogic.hpp"

#include "RuntimeProperties.hpp"

namespace IWXMVM::Components::VisualsLogic
{
    namespace
    {
        using Property = Types::KeyframeablePropertyType;

        // A setting that get returns a reference to; colors take a fourth value in presets (see Setting)
        template <typename Get>
        Setting MakeSetting(std::string_view dvar, std::string_view name, Kind kind, std::optional<Property> property,
                            Get get)
        {
            using T = std::remove_cvref_t<decltype(get(std::declval<Settings&>()))>;
            // (get only reads through a const settings)
            const auto value = [get](const Settings& settings) { return get(const_cast<Settings&>(settings)); };

            if constexpr (std::is_same_v<T, glm::vec3>)
            {
                return {dvar,
                        name,
                        kind,
                        kind == Kind::Color ? 4u : 3u,
                        property,
                        [=](Settings& settings, const float* values) {
                            get(settings) = glm::vec3(values[0], values[1], values[2]);
                        },
                        [=](const Settings& settings, float* values) {
                            const auto vector = value(settings);
                            values[0] = vector.x;
                            values[1] = vector.y;
                            values[2] = vector.z;
                            values[3] = 1.0f;
                        }};
            }
            else
            {
                return {
                    dvar,
                    name,
                    kind,
                    1,
                    property,
                    [=](Settings& settings, const float* values) { get(settings) = static_cast<T>(values[0]); },
                    [=](const Settings& settings, float* values) { values[0] = static_cast<float>(value(settings)); }};
            }
        }

        using Values = std::array<float, 4>;

        Values Write(const Setting& setting, const Settings& settings)
        {
            Values values{};
            setting.write(settings, values.data());
            return values;
        }

        bool Same(const Setting& setting, const Settings& a, const Settings& b)
        {
            const auto first = Write(setting, a);
            const auto second = Write(setting, b);
            return std::equal(first.begin(), first.begin() + setting.valueCount, second.begin());
        }
    }  // namespace

    const std::vector<Setting>& GetSettings()
    {
        static const std::vector<Setting> settings = {
            // DOF
            MakeSetting("r_dof_enable", "Depth of Field", Kind::Switch, Property::DepthOfFieldEnabled,
                        [](Settings& s) -> bool& { return s.dof.enabled; }),
            MakeSetting("r_dof_farblur", "Far Blur", Kind::Number, Property::DepthOfFieldFarBlur,
                        [](Settings& s) -> float& { return s.dof.farBlur; }),
            MakeSetting("r_dof_farstart", "Far Start", Kind::Number, Property::DepthOfFieldFarStart,
                        [](Settings& s) -> float& { return s.dof.farStart; }),
            MakeSetting("r_dof_farend", "Far End", Kind::Number, Property::DepthOfFieldFarEnd,
                        [](Settings& s) -> float& { return s.dof.farEnd; }),
            MakeSetting("r_dof_nearblur", "Near Blur", Kind::Number, Property::DepthOfFieldNearBlur,
                        [](Settings& s) -> float& { return s.dof.nearBlur; }),
            MakeSetting("r_dof_nearstart", "Near Start", Kind::Number, Property::DepthOfFieldNearStart,
                        [](Settings& s) -> float& { return s.dof.nearStart; }),
            MakeSetting("r_dof_nearend", "Near End", Kind::Number, Property::DepthOfFieldNearEnd,
                        [](Settings& s) -> float& { return s.dof.nearEnd; }),
            MakeSetting("r_dof_bias", "Bias", Kind::Number, Property::DepthOfFieldBias,
                        [](Settings& s) -> float& { return s.dof.bias; }),

            // SUN
            MakeSetting("r_lighttweaksuncolor", "Sun Color", Kind::Color, Property::SunLightColor,
                        [](Settings& s) -> glm::vec3& { return s.sunColor; }),
            MakeSetting("r_lighttweaksundirection", "Sun Direction", Kind::Values, Property::SunLightDirection,
                        [](Settings& s) -> glm::vec3& { return s.sunDirection; }),
            MakeSetting("r_lighttweaksunlight", "Sun Brightness", Kind::Number, Property::SunLightBrightness,
                        [](Settings& s) -> float& { return s.sunBrightness; }),

            // FILMTWEAKS
            MakeSetting("r_filmtweakenable", "Filmtweaks", Kind::Switch, Property::FilmtweakEnabled,
                        [](Settings& s) -> bool& { return s.filmtweaks.enabled; }),
            MakeSetting("r_filmtweakbrightness", "Brightness", Kind::Number, Property::FilmtweakBrightness,
                        [](Settings& s) -> float& { return s.filmtweaks.brightness; }),
            MakeSetting("r_filmtweakcontrast", "Contrast", Kind::Number, Property::FilmtweakContrast,
                        [](Settings& s) -> float& { return s.filmtweaks.contrast; }),
            MakeSetting("r_filmtweakdesaturation", "Desaturation", Kind::Number, Property::FilmtweakDesaturation,
                        [](Settings& s) -> float& { return s.filmtweaks.desaturation; }),
            // (Black Ops' tints of the light and dark ranges)
            MakeSetting("r_filmtweaklighttint", "Tint Light", Kind::Values, Property::FilmtweakTintLight,
                        [](Settings& s) -> glm::vec3& { return s.filmtweaks.tintLight; }),
            MakeSetting("r_filmtweakdarktint", "Tint Dark", Kind::Values, Property::FilmtweakTintDark,
                        [](Settings& s) -> glm::vec3& { return s.filmtweaks.tintDark; }),
            MakeSetting("r_filmtweakinvert", "Invert", Kind::Switch, Property::FilmtweakInvert,
                        [](Settings& s) -> bool& { return s.filmtweaks.invert; }),

            // Black Ops' per range filmtweaks (dark, mid, light), named after its dvars. r_filmtweakcontrast is the
            // single contrast of the other games, so the per range contrast has a name of its own.
            MakeSetting("r_filmtweakcontrastranges", "Contrast", Kind::Values, Property::FilmtweakContrastRanges,
                        [](Settings& s) -> glm::vec3& { return s.filmtweaks.contrastRanges; }),
            MakeSetting("r_filmtweaksaturation", "Saturation", Kind::Values, Property::FilmtweakSaturationRanges,
                        [](Settings& s) -> glm::vec3& { return s.filmtweaks.saturationRanges; }),
            MakeSetting("r_filmtweakhue", "Hue", Kind::Values, Property::FilmtweakHueRanges,
                        [](Settings& s) -> glm::vec3& { return s.filmtweaks.hueRanges; }),
            MakeSetting("r_filmtweakcolortemp", "Temperature", Kind::Values, Property::FilmtweakTemperatureRanges,
                        [](Settings& s) -> glm::vec3& { return s.filmtweaks.colorTemperatureRanges; }),
            MakeSetting("r_filmtweakbleach", "Bleach", Kind::Values, Property::FilmtweakBleachRanges,
                        [](Settings& s) -> glm::vec3& { return s.filmtweaks.bleachRanges; }),
            MakeSetting("r_filmtweakmidtint", "Tint Mid", Kind::Values, Property::FilmtweakTintMid,
                        [](Settings& s) -> glm::vec3& { return s.filmtweaks.tintMid; }),
            MakeSetting("r_filmtweakmidstart", "Mid Start", Kind::Number, Property::FilmtweakMidStart,
                        [](Settings& s) -> float& { return s.filmtweaks.midStart; }),
            MakeSetting("r_filmtweakmidend", "Mid End", Kind::Number, Property::FilmtweakMidEnd,
                        [](Settings& s) -> float& { return s.filmtweaks.midEnd; }),
            MakeSetting("r_filmtweakdarkfeather", "Dark Feather", Kind::Number, Property::FilmtweakDarkFeather,
                        [](Settings& s) -> float& { return s.filmtweaks.darkFeather; }),
            MakeSetting("r_filmtweaklightfeather", "Light Feather", Kind::Number, Property::FilmtweakLightFeather,
                        [](Settings& s) -> float& { return s.filmtweaks.lightFeather; }),

            // QUALITY: in units, not the game's sm_sunSampleSizeNear (the map's size makes that, see QualityLogic), so
            // a name of the mod's
            MakeSetting("iris_shadowdistance", "Shadow Distance", Kind::Number, Property::ShadowDistance,
                        [](Settings& s) -> float& { return s.shadowDistance; }),
            // (names of the mod's: the game has none of them)
            MakeSetting("iris_surface", "Surface Depth", Kind::Switch, std::nullopt,
                        [](Settings& s) -> bool& { return s.surfaceDepth; }),
            MakeSetting("iris_surfacedepth", "Surface Depth", Kind::Number, Property::SurfaceDepth,
                        [](Settings& s) -> float& { return s.surfaceDepthAmount; }),
            MakeSetting("iris_bumpstrength", "Bump Strength", Kind::Number, Property::BumpStrength,
                        [](Settings& s) -> float& { return s.bumpStrength; }),

            // SKY: the map's sky turned and graded (names of the mod's, as the game has none)
            MakeSetting("iris_skyhue", "Sky Hue", Kind::Number, Property::SkyHue,
                        [](Settings& s) -> float& { return s.sky.hue; }),
            MakeSetting("iris_skysaturation", "Sky Saturation", Kind::Number, Property::SkySaturation,
                        [](Settings& s) -> float& { return s.sky.saturation; }),
            MakeSetting("iris_skybrightness", "Sky Brightness", Kind::Number, Property::SkyBrightness,
                        [](Settings& s) -> float& { return s.sky.brightness; }),
            MakeSetting("iris_skycontrast", "Sky Contrast", Kind::Number, Property::SkyContrast,
                        [](Settings& s) -> float& { return s.sky.contrast; }),
            MakeSetting("iris_skytint", "Sky Tint", Kind::Color, Property::SkyTint,
                        [](Settings& s) -> glm::vec3& { return s.sky.tint; }),
            MakeSetting("iris_skyrotation", "Sky Rotation", Kind::Values, Property::SkyRotation,
                        [](Settings& s) -> glm::vec3& { return s.sky.rotation; }),

            // MISC
            MakeSetting("cg_draw2d", "2D Elements", Kind::Shown, std::nullopt,
                        [](Settings& s) -> bool& { return s.hudInfo.show2DElements; }),
            // a hardcore HUD is a hidden player HUD
            Setting{"ui_hud_hardcore", "Player HUD", Kind::Shown, 1, std::nullopt,
                    [](Settings& s, const float* values) { s.hudInfo.showPlayerHUD = values[0] == 0.0f; },
                    [](const Settings& s, float* values) { values[0] = s.hudInfo.showPlayerHUD ? 0.0f : 1.0f; }},
            MakeSetting("cg_drawshellshock", "Shellshock/Flashbang", Kind::Shown, std::nullopt,
                        [](Settings& s) -> bool& { return s.hudInfo.showShellshock; }),
            MakeSetting("ui_drawcrosshair", "Crosshair", Kind::Shown, std::nullopt,
                        [](Settings& s) -> bool& { return s.hudInfo.showCrosshair; }),
            MakeSetting("iwxmvm_ui_showscore", "Score", Kind::Shown, std::nullopt,
                        [](Settings& s) -> bool& { return s.hudInfo.showScore; }),
            MakeSetting("iwxmvm_ui_showothertext", "Icons and Text", Kind::Shown, std::nullopt,
                        [](Settings& s) -> bool& { return s.hudInfo.showIconsAndText; }),
            MakeSetting("iwxmvm_ui_showbloodoverlay", "Blood Overlay", Kind::Shown, std::nullopt,
                        [](Settings& s) -> bool& { return s.hudInfo.showBloodOverlay; }),
            MakeSetting("ui_hud_obituaries", "Killfeed", Kind::Shown, std::nullopt,
                        [](Settings& s) -> bool& { return s.hudInfo.showKillfeed; }),
            MakeSetting("g_teamcolor_allies", "Team 1 Color", Kind::Color, std::nullopt,
                        [](Settings& s) -> glm::vec3& { return s.hudInfo.killfeedTeam1Color; }),
            MakeSetting("g_teamcolor_axis", "Team 2 Color", Kind::Color, std::nullopt,
                        [](Settings& s) -> glm::vec3& { return s.hudInfo.killfeedTeam2Color; }),
        };

        return settings;
    }

    namespace
    {
        Section GetSection(const Setting& setting)
        {
            if (setting.dvar.starts_with("r_dof_"))
                return Section::DepthOfField;
            if (setting.dvar.starts_with("r_lighttweak"))
                return Section::Sun;
            if (setting.dvar.starts_with("r_filmtweak"))
                return Section::Filmtweaks;
            if (setting.dvar.starts_with("iris_sky"))
                return Section::Sky;
            if (setting.dvar.starts_with("iris_"))
                return Section::Quality;
            return Section::Hud;
        }
    }  // namespace

    std::string_view GetSectionName(Section section)
    {
        switch (section)
        {
            case Section::Hud:
                return "hud";
            case Section::MapEffects:
                return "mapeffects";
            case Section::Sun:
                return "sun";
            case Section::Sky:
                return "sky";
            case Section::Quality:
                return "quality";
            case Section::Filmtweaks:
                return "filmtweaks";
            case Section::ReShade:
                return "reshade";
            case Section::DepthOfField:
                return "dof";
        }
        return "";
    }

    std::optional<Section> GetSection(Types::KeyframeablePropertyType property)
    {
        if (RuntimeProperties::IsRuntime(property))
            return RuntimeProperties::IsActorProperty(property) ? std::nullopt : std::optional(Section::ReShade);
        const auto setting = FindSetting(property);
        return setting ? std::optional(GetSection(*setting)) : std::nullopt;
    }

    std::set<Section> ReadOpenSections(std::span<const std::string> names)
    {
        std::set<Section> sections;
        for (const auto section : SECTIONS)
        {
            if (std::ranges::find(names, GetSectionName(section)) != names.end())
                sections.insert(section);
        }
        return sections;
    }

    std::vector<std::string> WriteOpenSections(const std::set<Section>& sections)
    {
        std::vector<std::string> names;
        for (const auto section : SECTIONS)
        {
            if (sections.contains(section))
                names.emplace_back(GetSectionName(section));
        }
        return names;
    }

    SectionMarks GetMarks(Section section, const Settings& settings, const Settings& defaults, const IsKeyed& keyed)
    {
        SectionMarks marks;
        const auto& all = GetSettings();
        for (const auto& setting : all)
        {
            if (GetSection(setting) == section && setting.property && keyed && keyed(*setting.property))
                marks.keyed = true;
        }
        for (const auto index : GetChanged(defaults, settings, keyed))
        {
            if (GetSection(all[index]) == section)
                marks.changed = true;
        }
        return marks;
    }

    float GetSunBrightnessMax(float mapBrightness)
    {
        return std::max(4.0f, mapBrightness * 2.0f);
    }

    const Setting* FindSetting(std::string_view dvar)
    {
        const auto& settings = GetSettings();
        const auto it = std::ranges::find(settings, dvar, &Setting::dvar);
        return it != settings.end() ? &*it : nullptr;
    }

    const Setting* FindSetting(Types::KeyframeablePropertyType property)
    {
        const auto& settings = GetSettings();
        const auto it = std::ranges::find(settings, std::optional(property), &Setting::property);
        return it != settings.end() ? &*it : nullptr;
    }

    std::vector<std::size_t> GetChanged(const Settings& before, const Settings& after, const IsKeyed& keyed)
    {
        std::vector<std::size_t> changed;
        const auto& settings = GetSettings();
        for (std::size_t i = 0; i < settings.size(); ++i)
        {
            const auto& setting = settings[i];
            if (setting.property && keyed && keyed(*setting.property))
                continue;
            if (!Same(setting, before, after))
                changed.push_back(i);
        }
        return changed;
    }

    void Copy(const Settings& from, Settings& to, std::span<const std::size_t> settings)
    {
        const auto& all = GetSettings();
        for (const auto index : settings)
        {
            if (index >= all.size())
                continue;
            const auto values = Write(all[index], from);
            all[index].read(to, values.data());
        }
    }

    Settings FollowMap(const Settings& settings, const Settings& lastDefaults, const Settings& defaults,
                       const IsKeyed& keyed)
    {
        auto followed = defaults;
        Copy(settings, followed, GetChanged(lastDefaults, settings, keyed));
        return followed;
    }

    std::optional<Types::KeyframeValue> GetValue(const Settings& settings, Types::KeyframeablePropertyType property)
    {
        const auto setting = FindSetting(property);
        if (!setting)
            return std::nullopt;
        const auto values = Write(*setting, settings);
        if (setting->kind == Kind::Values || setting->kind == Kind::Color)
            return Types::KeyframeValue(glm::vec3(values[0], values[1], values[2]));
        return Types::KeyframeValue(values[0]);
    }

    bool SetValue(Settings& settings, Types::KeyframeablePropertyType property, const Types::KeyframeValue& value)
    {
        const auto setting = FindSetting(property);
        if (!setting)
            return false;
        Values values{};
        switch (setting->kind)
        {
            case Kind::Values:
            case Kind::Color:
                values = {value.vector3.x, value.vector3.y, value.vector3.z, 1.0f};
                break;
            case Kind::Switch:
            case Kind::Shown:
                // (a key between on and off, as only a hand-made file can have)
                values[0] = value.floatingPoint >= 0.5f ? 1.0f : 0.0f;
                break;
            case Kind::Number:
                values[0] = value.floatingPoint;
                break;
        }
        setting->read(settings, values.data());
        return true;
    }

    std::string DescribeChange(std::span<const std::size_t> settings, const Settings& after)
    {
        const auto& all = GetSettings();
        if (settings.size() != 1 || settings.front() >= all.size())
            return "change the visuals";
        const auto& setting = all[settings.front()];
        const bool on = Write(setting, after)[0] != 0.0f;
        switch (setting.kind)
        {
            case Kind::Switch:
                return std::format("turn {} {}", setting.name, on ? "on" : "off");
            case Kind::Shown:
                // (the player HUD's preset value says whether it is hidden)
                return std::format("{} {}", (setting.dvar == "ui_hud_hardcore" ? !on : on) ? "show" : "hide",
                                   setting.name);
            default:
                return std::format("change {}", setting.name);
        }
    }

    bool IsUsed(Types::KeyframeablePropertyType property, Types::Features features)
    {
        // (ReShade's values are keyed in the Look tab's ReShade section, too many to offer as tracks)
        if (RuntimeProperties::IsRuntime(property))
            return false;
        const bool filmtweakRanges = features & Types::Features_FilmtweakRanges;
        switch (property)
        {
            // (Black Ops registers r_filmTweakBrightness, r_filmTweakDesaturation and r_filmTweakInvert but never
            // reads them)
            case Property::FilmtweakContrast:
            case Property::FilmtweakBrightness:
            case Property::FilmtweakDesaturation:
            case Property::FilmtweakInvert:
                return !filmtweakRanges;
            case Property::FilmtweakContrastRanges:
            case Property::FilmtweakSaturationRanges:
            case Property::FilmtweakHueRanges:
            case Property::FilmtweakTemperatureRanges:
            case Property::FilmtweakBleachRanges:
            case Property::FilmtweakTintMid:
            case Property::FilmtweakMidStart:
            case Property::FilmtweakMidEnd:
            case Property::FilmtweakDarkFeather:
            case Property::FilmtweakLightFeather:
                return filmtweakRanges;
            case Property::ShadowDistance:
                return features & Types::Features_ShadowQuality;
            case Property::SurfaceDepth:
            case Property::BumpStrength:
                return features & Types::Features_SurfaceDepth;
            case Property::SkyHue:
            case Property::SkySaturation:
            case Property::SkyBrightness:
            case Property::SkyContrast:
            case Property::SkyTint:
            case Property::SkyRotation:
                return features & Types::Features_Sky;
            default:
                return true;
        }
    }

    bool Edit::Track(const Settings& frameStart, const Settings& now, const IsKeyed& keyed,
                     const std::optional<std::string>& label)
    {
        const auto changed = GetChanged(frameStart, now, keyed);
        if (changed.empty())
            return false;
        const bool began = !step;
        if (began)
            step = Step{label.value_or(DescribeChange(changed, now)), {}, frameStart, now};
        for (const auto index : changed)
        {
            if (std::ranges::find(step->settings, index) == step->settings.end())
                step->settings.push_back(index);
        }
        return began;
    }

    std::optional<Edit::Step> Edit::End(const Settings& now)
    {
        if (!step)
            return std::nullopt;
        auto ended = std::move(*step);
        step.reset();
        ended.after = now;
        const auto& all = GetSettings();
        std::erase_if(ended.settings, [&](std::size_t index) { return Same(all[index], ended.before, ended.after); });
        if (ended.settings.empty())
            return std::nullopt;
        std::ranges::sort(ended.settings);
        return ended;
    }
}  // namespace IWXMVM::Components::VisualsLogic
