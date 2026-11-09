#include "StdInclude.hpp"
#include "Test.hpp"

#include "nlohmann/json.hpp"

#include "Components/KeyframeLogic.hpp"
#include "Components/ReShadeLogic.hpp"
#include "Components/RuntimeProperties.hpp"
#include "Components/VisualsLogic.hpp"

using namespace IWXMVM;
namespace RuntimeProperties = Components::RuntimeProperties;
namespace ReShadeLogic = Components::ReShadeLogic;
using Type = Types::KeyframeablePropertyType;

namespace
{
    // a preset, a few of its effects
    ReShade::Effects MakeEffects()
    {
        ReShade::Effects effects;
        const auto technique = [&](std::string file, std::string name) {
            ReShade::Technique added{.id = file + "/technique:" + name, .file = file, .name = name, .label = name};
            effects.techniqueIndex[added.id] = effects.techniques.size();
            effects.techniques.push_back(added);
        };
        const auto uniform = [&](std::string file, std::string variable, std::string label, ReShade::Kind kind,
                                 float min, float max) {
            ReShade::Uniform added{.id = file + "/" + variable, .file = file, .variable = variable, .label = label,
                                   .kind = kind, .min = min, .max = max};
            if (kind == ReShade::Kind::Color)
                added.components = 3;
            if (kind == ReShade::Kind::List)
            {
                added.integer = true;
                added.items = {"Low", "Medium", "High"};
            }
            effects.uniformIndex[added.id] = effects.uniforms.size();
            effects.uniforms.push_back(added);
        };
        technique("MartysMods_SMAA.fx", "MartysMods_AntiAliasing");
        technique("qUINT_ssr.fx", "SSR");
        technique("MartysMods_MXAO.fx", "MartysMods_MXAO");
        technique("AdaptiveFog.fx", "AdaptiveFog");
        technique("qUINT_dof.fx", "ADOF");
        technique("MartysMods_SMAA.fx", "MartysMods_AntiAliasing_Prepass");
        uniform("qUINT_ssr.fx", "SSR_REFLECTION_INTENSITY", "Reflection Intensity", ReShade::Kind::Slider, 0, 1);
        uniform("MartysMods_MXAO.fx", "MXAO_SSAO_AMOUNT", "Ambient Occlusion Amount", ReShade::Kind::Slider, 0, 1);
        uniform("MartysMods_MXAO.fx", "MXAO_GLOBAL_SAMPLE_QUALITY_PRESET", "Sample Quality", ReShade::Kind::List, 0,
                2);
        uniform("AdaptiveFog.fx", "FogColor", "FogColor", ReShade::Kind::Color, 0, 1);
        uniform("qUINT_dof.fx", "fManualFocusDepth", "Manual focus depth", ReShade::Kind::Slider, 0, 1);
        uniform("qUINT_dof.fx", "bADOF_AutofocusEnable", "Enable Autofocus", ReShade::Kind::Bool, 0, 1);
        uniform("MartysMods_SMAA.fx", "EDGE_THRESHOLD", "Edge Detection Threshold", ReShade::Kind::Slider, 0.05f,
                0.2f);
        return effects;
    }

    const ReShade::Uniform& UniformOf(const ReShade::Effects& effects, const std::string& id)
    {
        return effects.uniforms[*effects.FindUniform(id)];
    }
}  // namespace

TEST("ReShade: a value's property is registered once per id, with a type of its own")
{
    RuntimeProperties::Description focus{.id = "tests.fx/focus", .name = "ReShade: Tests > Focus"};
    const auto first = RuntimeProperties::Register(focus);
    CHECK(RuntimeProperties::IsRuntime(first));
    CHECK(static_cast<std::uint32_t>(first) >= static_cast<std::uint32_t>(Type::ReShade));
    CHECK(!RuntimeProperties::IsRuntime(Type::ShadowDistance));
    // (again, with another name: the same, and its name as first registered, which its keys are filed under)
    focus.name = "ReShade: Tests > Focus again";
    CHECK(RuntimeProperties::Register(focus) == first);
    CHECK(RuntimeProperties::Find(first)->name == "ReShade: Tests > Focus");
    CHECK(RuntimeProperties::FindById("tests.fx/focus") == first);
    CHECK(!RuntimeProperties::FindById("tests.fx/none").has_value());

    const auto other = RuntimeProperties::Register({.id = "tests.fx/blur", .name = "ReShade: Tests > Blur"});
    CHECK(other != first);
    const auto property = RuntimeProperties::MakeProperty(other);
    CHECK(property.type == other);
    CHECK(property.name == "ReShade: Tests > Blur");
}

TEST("ReShade: two values of the same name get their ids after it")
{
    const auto a = RuntimeProperties::Register({.id = "a.fx/QUALITY", .name = "ReShade: Same > Quality"});
    const auto b = RuntimeProperties::Register({.id = "b.fx/QUALITY", .name = "ReShade: Same > Quality"});
    CHECK(RuntimeProperties::Find(a)->name == "ReShade: Same > Quality");
    CHECK(RuntimeProperties::Find(b)->name == "ReShade: Same > Quality (b.fx/QUALITY)");
}

TEST("ReShade: on/off and checkboxes are switches, lists whole numbers, the rest eased (B1)")
{
    const auto effects = MakeEffects();
    const auto adof = effects.techniques[*effects.FindTechnique("qUINT_dof.fx/technique:ADOF")];
    const auto on = RuntimeProperties::Register(ReShadeLogic::Describe(adof));
    const auto quality = RuntimeProperties::Register(ReShadeLogic::Describe(
        UniformOf(effects, "MartysMods_MXAO.fx/MXAO_GLOBAL_SAMPLE_QUALITY_PRESET"), "MartysMods_MXAO"));
    const auto autofocus = RuntimeProperties::Register(
        ReShadeLogic::Describe(UniformOf(effects, "qUINT_dof.fx/bADOF_AutofocusEnable"), "ADOF"));
    const auto focus = RuntimeProperties::Register(
        ReShadeLogic::Describe(UniformOf(effects, "qUINT_dof.fx/fManualFocusDepth"), "ADOF"));

    CHECK(KeyframeLogic::IsStepped(on));
    CHECK(KeyframeLogic::IsStepped(quality));
    CHECK(KeyframeLogic::IsStepped(autofocus));
    CHECK(!KeyframeLogic::IsStepped(focus));
    CHECK(KeyframeLogic::GetDefaultInterpolation(on) == Types::KeyInterpolation::Hold);
    CHECK(KeyframeLogic::GetDefaultInterpolation(focus) == Types::KeyInterpolation::Smooth);

    // (a list's key its nearest item, a switch's on or off, both held without eases)
    Types::Keyframe key(Types::KeyframeableProperty(quality, "", Types::KeyframeValueType::FloatingPoint, 0, 2), 100,
                        Types::KeyframeValue(1.6f));
    key.easeIn = 0.5f;
    const auto listKey = KeyframeLogic::Normalize(key, quality);
    CHECK(listKey.value.floatingPoint == 2.0f);
    CHECK(listKey.interpolation == Types::KeyInterpolation::Hold);
    CHECK(listKey.easeIn == 0.0f);
    key.value.floatingPoint = 0.7f;
    CHECK(KeyframeLogic::Normalize(key, on).value.floatingPoint == 1.0f);
    key.value.floatingPoint = 0.4f;
    CHECK(KeyframeLogic::Normalize(key, autofocus).value.floatingPoint == 0.0f);
    key.value.floatingPoint = 0.37f;
    CHECK(KeyframeLogic::Normalize(key, focus).value.floatingPoint == 0.37f);
}

TEST("ReShade: rows are named after the effect and the value, as the Look graph's tracks")
{
    const auto effects = MakeEffects();
    CHECK(ReShadeLogic::GetEffectName(effects, "qUINT_dof.fx") == "ADOF");
    // (a file of two techniques by its first; one without any by its name)
    CHECK(ReShadeLogic::GetEffectName(effects, "MartysMods_SMAA.fx") == "MartysMods_AntiAliasing");
    CHECK(ReShadeLogic::GetEffectName(effects, "Vignette.fx") == "Vignette");

    const auto focus = ReShadeLogic::Describe(UniformOf(effects, "qUINT_dof.fx/fManualFocusDepth"), "ADOF");
    CHECK(focus.id == "qUINT_dof.fx/fManualFocusDepth");
    CHECK(focus.name == "ReShade: ADOF > Manual focus depth");
    CHECK(focus.valueType == Types::KeyframeValueType::FloatingPoint);
    CHECK(focus.min == 0.0f && focus.max == 1.0f);

    const auto fog = ReShadeLogic::Describe(UniformOf(effects, "AdaptiveFog.fx/FogColor"), "AdaptiveFog");
    CHECK(fog.valueType == Types::KeyframeValueType::Vector3);
    CHECK(fog.valueNames[0] == "R" && fog.valueNames[2] == "B");

    const auto ssr = ReShadeLogic::Describe(effects.techniques[*effects.FindTechnique("qUINT_ssr.fx/technique:SSR")]);
    CHECK(ssr.id == "qUINT_ssr.fx/technique:SSR");
    CHECK(ssr.name == "ReShade: SSR on");
    CHECK(ssr.steps == RuntimeProperties::Steps::Switch);
}

TEST("ReShade: a key's value is ReShade's as its type takes it")
{
    const auto effects = MakeEffects();
    const auto& quality = UniformOf(effects, "MartysMods_MXAO.fx/MXAO_GLOBAL_SAMPLE_QUALITY_PRESET");
    CHECK(ReShadeLogic::FromKey(quality, Types::KeyframeValue(1.4f), {})[0] == 1.0f);
    CHECK(ReShadeLogic::FromKey(quality, Types::KeyframeValue(7.0f), {})[0] == 2.0f);
    const auto& autofocus = UniformOf(effects, "qUINT_dof.fx/bADOF_AutofocusEnable");
    CHECK(ReShadeLogic::FromKey(autofocus, Types::KeyframeValue(0.6f), {})[0] == 1.0f);

    // (a colour's alpha stays the preset's)
    auto fogColor = UniformOf(effects, "AdaptiveFog.fx/FogColor");
    fogColor.components = 4;
    const auto fog = ReShadeLogic::FromKey(fogColor, Types::KeyframeValue(glm::vec3(0.1f, 0.2f, 0.3f)),
                                           {0.9f, 0.9f, 0.9f, 0.5f});
    CHECK(fog[0] == 0.1f && fog[2] == 0.3f && fog[3] == 0.5f);
    const auto key = ReShadeLogic::ToKey(fogColor, fog);
    CHECK(key.vector3.y == 0.2f);

    auto steps = UniformOf(effects, "qUINT_ssr.fx/SSR_REFLECTION_INTENSITY");
    steps.integer = true;
    CHECK(ReShadeLogic::FromKey(steps, Types::KeyframeValue(63.6f), {})[0] == 64.0f);
}

TEST("ReShade: a value past its range stays, and says which way")
{
    const auto effects = MakeEffects();
    const auto& amount = UniformOf(effects, "MartysMods_MXAO.fx/MXAO_SSAO_AMOUNT");
    CHECK(ReShadeLogic::OutOfRange(amount, 1.2f) == 1);
    CHECK(ReShadeLogic::OutOfRange(amount, 1.0f) == 0);
    CHECK(ReShadeLogic::OutOfRange(amount, -0.5f) == -1);
    const auto [low, high] = ReShadeLogic::GetSliderRange(amount, 1.2f, 1.2f);
    CHECK(low == 0.0f && high == 1.0f);

    // (a value without a range gets one that holds it)
    ReShade::Uniform drag{.kind = ReShade::Kind::Drag, .min = 0, .max = 0};
    const auto [dragLow, dragHigh] = ReShadeLogic::GetSliderRange(drag, 3.0f, 2.0f);
    CHECK(dragLow <= 0.0f && dragHigh >= 3.0f);
    CHECK(ReShadeLogic::OutOfRange(drag, 50.0f) == 0);
}

TEST("ReShade: effects on or keyed are listed first, the others under All effects (C1)")
{
    const auto effects = MakeEffects();
    // SMAA, SSR and MXAO on; ADOF off but keyed (its focus), the fog and the prepass off
    const std::vector<bool> on = {true, true, true, false, false, false};
    std::set<std::string> kept;
    const auto folds = ReShadeLogic::GetFolds(effects, on, {"qUINT_dof.fx/fManualFocusDepth"}, kept);
    CHECK(folds.listed.size() == 4);
    CHECK(folds.others.size() == 2);
    CHECK(effects.techniques[folds.listed[3].technique].name == "ADOF");
    CHECK(folds.listed[3].uniforms.size() == 2);
    CHECK(effects.techniques[folds.others[0].technique].name == "AdaptiveFog");
    // (SMAA's values are under its first technique, the prepass has none)
    CHECK(folds.listed[0].uniforms.size() == 1);
    CHECK(effects.techniques[folds.others[1].technique].name == "MartysMods_AntiAliasing_Prepass");
    CHECK(folds.others[1].uniforms.empty());

    // (keying an effect's on lists it too)
    std::set<std::string> keptOn;
    const auto keyedOn = ReShadeLogic::GetFolds(effects, on, {"AdaptiveFog.fx/technique:AdaptiveFog"}, keptOn);
    CHECK(keyedOn.listed.size() == 4);
}

TEST("ReShade: an effect turned off stays listed until a demo loads, and isn't counted as keyed (#83)")
{
    const auto effects = MakeEffects();
    std::set<std::string> kept;
    // SMAA, SSR and MXAO on
    auto folds = ReShadeLogic::GetFolds(effects, {true, true, true, false, false, false}, {}, kept);
    CHECK(folds.listed.size() == 3);
    CHECK(folds.kept == 0);

    // SSR turned off: its row stays where it was, among the listed, in the same order
    const std::vector<bool> ssrOff = {true, false, true, false, false, false};
    folds = ReShadeLogic::GetFolds(effects, ssrOff, {}, kept);
    CHECK(folds.listed.size() == 3);
    CHECK(folds.others.size() == 3);
    CHECK(folds.kept == 1);
    CHECK(folds.listed[1].technique == 1);

    // still after any number of frames, and an effect turned on joins at once
    const std::vector<bool> fogOn = {true, false, true, true, false, false};
    folds = ReShadeLogic::GetFolds(effects, fogOn, {}, kept);
    CHECK(folds.listed.size() == 4);
    CHECK(folds.kept == 1);

    // an effect only keyed is listed but isn't one kept
    folds = ReShadeLogic::GetFolds(effects, ssrOff, {"qUINT_dof.fx/fManualFocusDepth"}, kept);
    CHECK(folds.listed.size() == 5);
    CHECK(folds.kept == 2);

    // a demo loaded: what's on or keyed now is what's listed
    kept.clear();
    folds = ReShadeLogic::GetFolds(effects, ssrOff, {}, kept);
    CHECK(folds.listed.size() == 2);
    CHECK(folds.others.size() == 4);
    CHECK(folds.kept == 0);
}

TEST("ReShade: keys of an effect that isn't loaded are kept, by file, and saved by id")
{
    const auto effects = MakeEffects();
    const auto missing = ReShadeLogic::GetMissing(
        effects, {"Vignette.fx/Amount", "qUINT_dof.fx/fManualFocusDepth", "Vignette.fx/technique:Vignette"});
    CHECK(missing.size() == 1);
    CHECK(missing[0].file == "Vignette.fx");
    CHECK(missing[0].ids.size() == 2);
    CHECK(ReShadeLogic::GetFile("Vignette.fx/technique:Vignette") == "Vignette.fx");

    // what the keyframe file keeps of a property makes it again as it was
    RuntimeProperties::Description vignette{.id = "Vignette.fx/Amount",
                                            .name = "ReShade: Vignette > Amount",
                                            .min = 0.0f,
                                            .max = 2.0f};
    nlohmann::json node;
    RuntimeProperties::Write(vignette, node);
    const auto read = RuntimeProperties::Read(node);
    CHECK(read.has_value());
    CHECK(read->id == vignette.id);
    CHECK(read->name == vignette.name);
    CHECK(read->max == 2.0f);
    CHECK(read->steps == RuntimeProperties::Steps::None);

    RuntimeProperties::Description color{.id = "Vignette.fx/Color",
                                         .name = "ReShade: Vignette > Color",
                                         .valueType = Types::KeyframeValueType::Vector3,
                                         .steps = RuntimeProperties::Steps::None,
                                         .valueNames = {"R", "G", "B"}};
    nlohmann::json colorNode;
    RuntimeProperties::Write(color, colorNode);
    const auto readColor = RuntimeProperties::Read(colorNode);
    CHECK(readColor->valueType == Types::KeyframeValueType::Vector3);
    CHECK(readColor->valueNames[1] == "G");

    RuntimeProperties::Description onOff{.id = "Vignette.fx/technique:Vignette", .steps = RuntimeProperties::Steps::Switch};
    nlohmann::json onNode;
    RuntimeProperties::Write(onOff, onNode);
    CHECK(RuntimeProperties::Read(onNode)->steps == RuntimeProperties::Steps::Switch);

    // (nothing without an id, or with a campath's value)
    CHECK(!RuntimeProperties::Read(nlohmann::json{{"name", "x"}}).has_value());
    CHECK(!RuntimeProperties::Read(nlohmann::json{{"id", "x.fx/y"}, {"type", "CameraData"}}).has_value());
}

TEST("ReShade: its values aren't offered as tracks, they're keyed in the Look tab")
{
    const auto type = RuntimeProperties::Register({.id = "tracks.fx/x", .name = "ReShade: Tracks > X"});
    CHECK(!Components::VisualsLogic::IsUsed(type, Types::Features_ShadowQuality));
    CHECK(Components::VisualsLogic::IsUsed(Type::SunLightBrightness, Types::Features_ShadowQuality));
}

TEST("ReShade: its clock follows the demo, standing still while paused (E1)")
{
    ReShadeLogic::ClockState state;
    auto clock = ReShadeLogic::AdvanceClock(state, 1000);
    CHECK(clock.timer == 1000.0f);
    CHECK(clock.frameCount == 0);
    clock = ReShadeLogic::AdvanceClock(state, 1004);
    CHECK(clock.frameTime == 4.0f);
    CHECK(clock.frameCount == 1);
    // paused: the same frame again
    clock = ReShadeLogic::AdvanceClock(state, 1004);
    CHECK(clock.timer == 1004.0f);
    CHECK(clock.frameTime == 0.0f);
    CHECK(clock.frameCount == 1);
    // a rewind: on to another frame
    clock = ReShadeLogic::AdvanceClock(state, 500);
    CHECK(clock.timer == 500.0f);
    CHECK(clock.frameCount == 2);
}
