#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/UndoHistory.hpp"
#include "Components/VisualsLogic.hpp"

using namespace IWXMVM;
namespace VisualsLogic = Components::VisualsLogic;
using VisualsLogic::Settings;
using Property = Types::KeyframeablePropertyType;

namespace
{
    // settings as a demo might start with them
    Settings MakeSettings()
    {
        Settings settings{};
        settings.dof = {false, 1.0f, 1000.0f, 5000.0f, 0.0f, 0.0f, 100.0f, 2.0f};
        settings.sunColor = glm::vec3(1.0f, 0.9f, 0.8f);
        settings.sunDirection = glm::vec3(-30.0f, 120.0f, 0.0f);
        settings.sunBrightness = 1.5f;
        settings.shadowDistance = 3300.0f;
        settings.surfaceDepth = true;
        settings.surfaceDepthAmount = 2.5f;
        settings.bumpStrength = 1.5f;
        settings.filmtweaks.enabled = true;
        settings.filmtweaks.brightness = 0.1f;
        settings.filmtweaks.contrast = 1.0f;
        settings.filmtweaks.desaturation = 0.2f;
        settings.filmtweaks.tintLight = glm::vec3(1.0f);
        settings.filmtweaks.tintDark = glm::vec3(1.0f);
        settings.hudInfo.show2DElements = true;
        settings.hudInfo.showPlayerHUD = true;
        settings.hudInfo.showKillfeed = true;
        return settings;
    }

    // the same with another map's sun
    Settings MakeOtherMap()
    {
        auto settings = MakeSettings();
        settings.sunColor = glm::vec3(0.5f, 0.6f, 0.7f);
        settings.sunDirection = glm::vec3(-60.0f, 20.0f, 0.0f);
        settings.sunBrightness = 2.0f;
        return settings;
    }

    std::vector<std::size_t> IndexOf(std::initializer_list<std::string_view> dvars)
    {
        std::vector<std::size_t> indices;
        const auto& settings = VisualsLogic::GetSettings();
        for (const auto dvar : dvars)
            indices.push_back(static_cast<std::size_t>(VisualsLogic::FindSetting(dvar) - settings.data()));
        return indices;
    }

    // every property, the campath's too
    std::vector<Property> AllProperties()
    {
        std::vector<Property> all;
        for (int i = 0; i <= static_cast<int>(Property::FilmtweakLightFeather); ++i)
            all.push_back(static_cast<Property>(i));
        return all;
    }
}  // namespace

TEST("every keyframeable property but the campath animates one setting of the Visuals tab")
{
    for (const auto property : AllProperties())
    {
        const auto count =
            std::ranges::count(VisualsLogic::GetSettings(), std::optional(property), &VisualsLogic::Setting::property);
        CHECK(count == (property == Property::CampathCamera ? 0 : 1));
    }
    // (names in presets are unique)
    for (const auto& setting : VisualsLogic::GetSettings())
        CHECK(VisualsLogic::FindSetting(setting.dvar) == &setting);
}

TEST("a property's value goes into the settings and comes out the same, a switch on from a half up")
{
    auto settings = MakeSettings();
    CHECK(VisualsLogic::SetValue(settings, Property::FilmtweakBrightness, Types::KeyframeValue(-0.25f)));
    CHECK(settings.filmtweaks.brightness == -0.25f);
    CHECK(VisualsLogic::GetValue(settings, Property::FilmtweakBrightness)->floatingPoint == -0.25f);

    CHECK(VisualsLogic::SetValue(settings, Property::FilmtweakTemperatureRanges,
                                 Types::KeyframeValue(glm::vec3(3000.0f, 6500.0f, 9000.0f))));
    CHECK(settings.filmtweaks.colorTemperatureRanges == glm::vec3(3000.0f, 6500.0f, 9000.0f));
    CHECK(VisualsLogic::GetValue(settings, Property::FilmtweakTemperatureRanges)->vector3 ==
          glm::vec3(3000.0f, 6500.0f, 9000.0f));

    CHECK(VisualsLogic::SetValue(settings, Property::SunLightColor, Types::KeyframeValue(glm::vec3(0.5f))));
    CHECK(settings.sunColor == glm::vec3(0.5f));
    CHECK(VisualsLogic::SetValue(settings, Property::FilmtweakMidEnd, Types::KeyframeValue(0.6f)));
    CHECK(settings.filmtweaks.midEnd == 0.6f);
    CHECK(VisualsLogic::SetValue(settings, Property::DepthOfFieldBias, Types::KeyframeValue(3.0f)));
    CHECK(settings.dof.bias == 3.0f);

    CHECK(VisualsLogic::SetValue(settings, Property::FilmtweakInvert, Types::KeyframeValue(0.6f)));
    CHECK(settings.filmtweaks.invert);
    CHECK(VisualsLogic::GetValue(settings, Property::FilmtweakInvert)->floatingPoint == 1.0f);
    CHECK(VisualsLogic::SetValue(settings, Property::FilmtweakInvert, Types::KeyframeValue(0.4f)));
    CHECK(!settings.filmtweaks.invert);
    CHECK(VisualsLogic::SetValue(settings, Property::DepthOfFieldEnabled, Types::KeyframeValue(1.0f)));
    CHECK(settings.dof.enabled);
    CHECK(VisualsLogic::SetValue(settings, Property::FilmtweakEnabled, Types::KeyframeValue(0.0f)));
    CHECK(!settings.filmtweaks.enabled);

    // the campath is no setting
    CHECK(!VisualsLogic::GetValue(settings, Property::CampathCamera));
    CHECK(!VisualsLogic::SetValue(settings, Property::CampathCamera, Types::KeyframeValue(1.0f)));
}

TEST("what changed between two settings, those with keys left out, and copied back one by one")
{
    const auto before = MakeSettings();
    auto after = before;
    after.filmtweaks.brightness = 0.5f;
    after.filmtweaks.hueRanges.y = 20.0f;
    after.hudInfo.showKillfeed = false;
    after.dof.farBlur = 4.0f;
    CHECK(VisualsLogic::GetChanged(before, after) ==
          IndexOf({"r_dof_farblur", "r_filmtweakbrightness", "r_filmtweakhue", "ui_hud_obituaries"}));
    // keys set what they animate: not a change of the settings
    const auto keyed = [](Property property) { return property == Property::FilmtweakBrightness; };
    CHECK(VisualsLogic::GetChanged(before, after, keyed) ==
          IndexOf({"r_dof_farblur", "r_filmtweakhue", "ui_hud_obituaries"}));
    CHECK(VisualsLogic::GetChanged(before, before).empty());

    // copying some of them back leaves the others
    auto copied = after;
    VisualsLogic::Copy(before, copied, IndexOf({"r_filmtweakhue", "ui_hud_obituaries"}));
    CHECK(copied.filmtweaks.hueRanges == before.filmtweaks.hueRanges);
    CHECK(copied.hudInfo.showKillfeed);
    CHECK(copied.filmtweaks.brightness == 0.5f);
    CHECK(copied.dof.farBlur == 4.0f);
    CHECK(VisualsLogic::GetChanged(before, copied) == IndexOf({"r_dof_farblur", "r_filmtweakbrightness"}));
}

TEST("a change is named as the undo history shows it")
{
    auto settings = MakeSettings();
    const auto describe = [&](std::initializer_list<std::string_view> dvars) {
        const auto indices = IndexOf(dvars);
        return VisualsLogic::DescribeChange(indices, settings);
    };
    CHECK(describe({"r_dof_farblur"}) == "change Far Blur");
    CHECK(describe({"r_filmtweakcolortemp"}) == "change Temperature");
    settings.filmtweaks.enabled = false;
    CHECK(describe({"r_filmtweakenable"}) == "turn Filmtweaks off");
    settings.dof.enabled = true;
    CHECK(describe({"r_dof_enable"}) == "turn Depth of Field on");
    settings.hudInfo.showKillfeed = false;
    CHECK(describe({"ui_hud_obituaries"}) == "hide Killfeed");
    // (a hardcore HUD is a hidden player HUD)
    settings.hudInfo.showPlayerHUD = true;
    CHECK(describe({"ui_hud_hardcore"}) == "show Player HUD");
    settings.hudInfo.showPlayerHUD = false;
    CHECK(describe({"ui_hud_hardcore"}) == "hide Player HUD");
    CHECK(describe({"r_dof_farblur", "r_dof_farend"}) == "change the visuals");
}

TEST("Black Ops' per range filmtweaks have controls only in games that report them")
{
    constexpr auto RANGES = Types::Features_FilmtweakRanges;
    constexpr auto NONE = Types::Features_None;
    CHECK(VisualsLogic::IsUsed(Property::FilmtweakTemperatureRanges, RANGES));
    CHECK(!VisualsLogic::IsUsed(Property::FilmtweakTemperatureRanges, NONE));
    CHECK(VisualsLogic::IsUsed(Property::FilmtweakDarkFeather, RANGES));
    CHECK(!VisualsLogic::IsUsed(Property::FilmtweakTintMid, NONE));
    CHECK(!VisualsLogic::IsUsed(Property::FilmtweakContrast, RANGES));
    CHECK(VisualsLogic::IsUsed(Property::FilmtweakContrast, NONE));
    // (Black Ops registers these dvars but never reads them)
    for (const auto property :
         {Property::FilmtweakBrightness, Property::FilmtweakDesaturation, Property::FilmtweakInvert})
    {
        CHECK(!VisualsLogic::IsUsed(property, RANGES));
        CHECK(VisualsLogic::IsUsed(property, NONE));
    }
    for (const auto property : {Property::FilmtweakTintLight, Property::FilmtweakTintDark, Property::FilmtweakEnabled,
                                Property::DepthOfFieldEnabled, Property::SunLightColor})
    {
        CHECK(VisualsLogic::IsUsed(property, RANGES));
        CHECK(VisualsLogic::IsUsed(property, NONE));
    }
    // the Quality section's Shadow Distance only in a game that sets it
    CHECK(VisualsLogic::IsUsed(Property::ShadowDistance, Types::Features_ShadowQuality));
    CHECK(!VisualsLogic::IsUsed(Property::ShadowDistance, RANGES));
    // and Surface depth's only in a game that draws it
    for (const auto property : {Property::SurfaceDepth, Property::BumpStrength})
    {
        CHECK(VisualsLogic::IsUsed(property, Types::Features_SurfaceDepth));
        CHECK(!VisualsLogic::IsUsed(property, Types::Features_ShadowQuality));
    }
}

TEST("Surface depth's rows: On or Off, Depth and Bump strength in presets, the two numbers keyed")
{
    Settings settings;
    CHECK(!settings.surfaceDepth && settings.surfaceDepthAmount == 1.0f && settings.bumpStrength == 1.0f);
    const auto on = VisualsLogic::FindSetting("iris_surface");
    const auto depth = VisualsLogic::FindSetting("iris_surfacedepth");
    const auto bump = VisualsLogic::FindSetting("iris_bumpstrength");
    CHECK(on && depth && bump);
    if (!on || !depth || !bump)
        return;
    CHECK(on->kind == VisualsLogic::Kind::Switch && !on->property);
    CHECK(depth->property == Property::SurfaceDepth && bump->property == Property::BumpStrength);
    const float values[] = {1.0f, 2.5f, 1.5f};
    on->read(settings, &values[0]);
    depth->read(settings, &values[1]);
    bump->read(settings, &values[2]);
    CHECK(settings.surfaceDepth && settings.surfaceDepthAmount == 2.5f && settings.bumpStrength == 1.5f);
    CHECK(VisualsLogic::SetValue(settings, Property::SurfaceDepth, Types::KeyframeValue(3.0f)));
    CHECK(VisualsLogic::GetValue(settings, Property::SurfaceDepth)->floatingPoint == 3.0f);
    const std::size_t index = on - VisualsLogic::GetSettings().data();
    CHECK(VisualsLogic::DescribeChange(std::vector{index}, settings) == "turn Surface Depth on");
}

TEST("a slider's drag over many frames is one edit: the settings before it and after it")
{
    VisualsLogic::Edit edit;
    const auto keyed = [](Property property) { return property == Property::FilmtweakBrightness; };
    const auto start = MakeSettings();
    auto now = start;

    // a frame that changes nothing begins nothing
    CHECK(!edit.Track(now, now, keyed, std::nullopt));
    CHECK(!edit.IsOpen());

    // the drag: each frame moves it on
    auto frameStart = now;
    now.dof.farBlur = 2.0f;
    CHECK(edit.Track(frameStart, now, keyed, "change Far Blur"));
    CHECK(edit.IsOpen());
    for (const float blur : {3.0f, 4.0f, 5.0f})
    {
        frameStart = now;
        now.dof.farBlur = blur;
        // (a keyed setting the keys move meanwhile is theirs)
        now.filmtweaks.brightness += 0.1f;
        CHECK(!edit.Track(frameStart, now, keyed, "change something else"));
    }
    const auto step = edit.End(now);
    CHECK(!edit.IsOpen());
    CHECK(step.has_value());
    CHECK(step->label == "change Far Blur");
    CHECK(step->settings == IndexOf({"r_dof_farblur"}));
    CHECK(step->before.dof.farBlur == 1.0f);
    CHECK(step->after.dof.farBlur == 5.0f);

    // named after what it changed without a label
    frameStart = now;
    now.hudInfo.show2DElements = false;
    CHECK(edit.Track(frameStart, now, keyed, std::nullopt));
    CHECK(edit.End(now)->label == "hide 2D Elements");

    // dragged back to where it was: nothing to undo
    frameStart = now;
    now.dof.farEnd = 100.0f;
    edit.Track(frameStart, now, keyed, std::nullopt);
    frameStart = now;
    now.dof.farEnd = start.dof.farEnd;
    edit.Track(frameStart, now, keyed, std::nullopt);
    CHECK(!edit.End(now).has_value());
    CHECK(!edit.End(now).has_value());
}

TEST("an edit's step undoes and redoes only what it changed, and joins a key's edit of the same gesture")
{
    // the tab's settings and the history, as the Visuals tab keeps them
    auto visuals = MakeSettings();
    Components::UndoHistory history;
    VisualsLogic::Edit edit;
    const auto push = [&](const VisualsLogic::Edit::Step& step) {
        history.Push(
            step.label, [&visuals, step] { VisualsLogic::Copy(step.before, visuals, step.settings); },
            [&visuals, step] { VisualsLogic::Copy(step.after, visuals, step.settings); });
    };

    auto frameStart = visuals;
    visuals.filmtweaks.saturationRanges = glm::vec3(1.5f);
    edit.Track(frameStart, visuals, {}, "change Saturation");
    push(*edit.End(visuals));
    // something else changed meanwhile (keys followed, another step) stays as it is
    visuals.sunBrightness = 3.0f;
    CHECK(history.Undo() == "change Saturation");
    CHECK(visuals.filmtweaks.saturationRanges == glm::vec3(1.0f));
    CHECK(visuals.sunBrightness == 3.0f);
    CHECK(history.Redo() == "change Saturation");
    CHECK(visuals.filmtweaks.saturationRanges == glm::vec3(1.5f));

    // a control changing a keyed tint and an unkeyed one at once: the key's edit and the tab's are one step
    int keyChanges = 0;
    history.BeginGroup("change Tint");
    history.Push("change key", [&] { --keyChanges; }, [&] { ++keyChanges; });
    ++keyChanges;
    frameStart = visuals;
    visuals.filmtweaks.tintMid = glm::vec3(2.0f);
    CHECK(edit.Track(frameStart, visuals, {}, "change Tint"));
    history.BeginGroup("change Tint");
    // (the key's edit ends first, then the tab's)
    history.EndGroup();
    push(*edit.End(visuals));
    history.EndGroup();
    CHECK(history.GetUndoLabel() == "change Tint");
    CHECK(history.Undo() == "change Tint");
    CHECK(keyChanges == 0);
    CHECK(visuals.filmtweaks.tintMid == glm::vec3(1.0f));
    CHECK(history.GetUndoLabel() == "change Saturation");
}

TEST("every setting of the tab is in presets, as before: names, values and the colors' fourth value")
{
    const auto& settings = VisualsLogic::GetSettings();
    CHECK(settings.size() == 42);
    // (the Quality section's Shadow Distance in units, and Surface depth's three rows, under names of the mod's: not
    // the game's dvars)
    CHECK(VisualsLogic::FindSetting("iris_shadowdistance")->valueCount == 1);
    CHECK(VisualsLogic::FindSetting(Types::KeyframeablePropertyType::ShadowDistance) ==
          VisualsLogic::FindSetting("iris_shadowdistance"));
    CHECK(VisualsLogic::FindSetting("r_lighttweaksuncolor")->valueCount == 4);
    CHECK(VisualsLogic::FindSetting("g_teamcolor_axis")->valueCount == 4);
    CHECK(VisualsLogic::FindSetting("r_filmtweaklighttint")->valueCount == 3);
    CHECK(VisualsLogic::FindSetting("r_filmtweakcolortemp")->valueCount == 3);
    CHECK(VisualsLogic::FindSetting("r_filmtweakbrightness")->valueCount == 1);

    // written and read back through the list: the same settings
    const auto original = MakeSettings();
    Settings read{};
    for (const auto& setting : settings)
    {
        std::array<float, 4> values{};
        setting.write(original, values.data());
        setting.read(read, values.data());
    }
    CHECK(VisualsLogic::GetChanged(original, read).empty());
}

TEST("Visuals: on a new map, what was changed stays and the rest takes the map's values (#80)")
{
    const auto lastDefaults = MakeSettings();
    const auto defaults = MakeOtherMap();

    // nothing changed: all of it is the new map's
    auto settings = lastDefaults;
    auto followed = VisualsLogic::FollowMap(settings, lastDefaults, defaults, {});
    CHECK(followed.sunColor == defaults.sunColor);
    CHECK(followed.sunDirection == defaults.sunDirection);
    CHECK(followed.sunBrightness == defaults.sunBrightness);

    // a colour changed stays, the other two rows of the Sun follow the map, the rest is left as it was
    settings.sunColor = glm::vec3(1.0f, 0.0f, 0.0f);
    settings.dof.farBlur = 9.0f;
    settings.hudInfo.showKillfeed = false;
    followed = VisualsLogic::FollowMap(settings, lastDefaults, defaults, {});
    CHECK(followed.sunColor == settings.sunColor);
    CHECK(followed.sunDirection == defaults.sunDirection);
    CHECK(followed.sunBrightness == defaults.sunBrightness);
    CHECK(followed.dof.farBlur == 9.0f);
    CHECK(!followed.hudInfo.showKillfeed);

    // what keys were setting isn't the user's: a demo's sun doesn't stay on the next map
    const auto keyed = [](Property property) { return property == Property::SunLightColor; };
    followed = VisualsLogic::FollowMap(settings, lastDefaults, defaults, keyed);
    CHECK(followed.sunColor == defaults.sunColor);
    CHECK(followed.dof.farBlur == 9.0f);

    // a setting back at the old map's value counts as not changed
    settings = lastDefaults;
    followed = VisualsLogic::FollowMap(settings, lastDefaults, lastDefaults, {});
    CHECK(VisualsLogic::GetChanged(followed, lastDefaults).empty());
}

TEST("Visuals: the Look tab's sections are saved by name, none open at first")
{
    namespace Logic = VisualsLogic;
    const std::vector<std::string> nothing;
    CHECK(Logic::ReadOpenSections(nothing).empty());
    const std::set sections = {Logic::Section::DepthOfField, Logic::Section::Sun};
    const auto names = Logic::WriteOpenSections(sections);
    // (in the tab's order)
    CHECK(names == std::vector<std::string>({"sun", "dof"}));
    CHECK(Logic::ReadOpenSections(names) == sections);
    // what an older or newer version wrote is dropped
    const std::vector<std::string> unknown = {"sun", "nothing"};
    CHECK(Logic::ReadOpenSections(unknown).size() == 1);
}

TEST("Visuals: a key's row is in its section")
{
    namespace Logic = VisualsLogic;
    CHECK(Logic::GetSection(Property::SunLightBrightness) == Logic::Section::Sun);
    CHECK(Logic::GetSection(Property::DepthOfFieldFarBlur) == Logic::Section::DepthOfField);
    CHECK(Logic::GetSection(Property::FilmtweakTintMid) == Logic::Section::Filmtweaks);
    CHECK(Logic::GetSection(Property::ShadowDistance) == Logic::Section::Quality);
    CHECK(Logic::GetSection(Property::BumpStrength) == Logic::Section::Quality);
    // (not the tab's)
    CHECK(!Logic::GetSection(Property::CampathCamera).has_value());
}

TEST("Visuals: a closed section marks its keys and what isn't at its default")
{
    namespace Logic = VisualsLogic;
    const auto defaults = MakeSettings();
    auto settings = defaults;
    CHECK(!Logic::GetMarks(Logic::Section::Sun, settings, defaults, {}).changed);
    CHECK(!Logic::GetMarks(Logic::Section::Sun, settings, defaults, {}).keyed);

    settings.sunBrightness = 3.0f;
    auto marks = Logic::GetMarks(Logic::Section::Sun, settings, defaults, {});
    CHECK(marks.changed);
    CHECK(!marks.keyed);
    // (only in its own section)
    CHECK(!Logic::GetMarks(Logic::Section::DepthOfField, settings, defaults, {}).changed);

    // keyed, what keys set is a key's, not a change
    const auto keyed = [](Property property) { return property == Property::SunLightBrightness; };
    marks = Logic::GetMarks(Logic::Section::Sun, settings, defaults, keyed);
    CHECK(marks.keyed);
    CHECK(!marks.changed);
    CHECK(!Logic::GetMarks(Logic::Section::Filmtweaks, settings, defaults, keyed).keyed);

    // the HUD has no keys, and changes show
    settings = defaults;
    settings.hudInfo.showKillfeed = false;
    marks = Logic::GetMarks(Logic::Section::Hud, settings, defaults, keyed);
    CHECK(marks.changed);
    CHECK(!marks.keyed);
}

TEST("Visuals: the Sun's Brightness bar holds the map's own")
{
    CHECK(VisualsLogic::GetSunBrightnessMax(14.0f) == 28.0f);
    CHECK(VisualsLogic::GetSunBrightnessMax(13.0f) == 26.0f);
    // (a dim map keeps a bar to drag on)
    CHECK(VisualsLogic::GetSunBrightnessMax(1.0f) == 4.0f);
}
