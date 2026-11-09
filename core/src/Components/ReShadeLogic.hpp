#pragma once
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ReShade.hpp"
#include "Components/RuntimeProperties.hpp"
#include "Types/Keyframe.hpp"

// The Look tab's ReShade section without the game: what its rows are called and keyed
// as, the order of its effects, and ReShade's clock following the demo; tests in tests/actors-tests
namespace IWXMVM::Components::ReShadeLogic
{
    // An effect file's name as its rows are named: its first technique's label in ReShade's order ("ADOF" for
    // qUINT_dof.fx), the file without ".fx" when it has none
    std::string GetEffectName(const ReShade::Effects& effects, const std::string& file);

    // The run-time properties of a value and of a technique's on/off (B1): "ReShade: ADOF > Manual focus depth" and
    // "ReShade: ADOF on". Floats eased between keys, colours and float2/float3 a Vector3, bools and on/off switches,
    // lists whole numbers.
    RuntimeProperties::Description Describe(const ReShade::Uniform& uniform, const std::string& effectName);
    RuntimeProperties::Description Describe(const ReShade::Technique& technique);

    // ReShade's value of a uniform as a key holds it, and a key's as ReShade takes it (an int's rounded, a list's
    // clamped to its items, a bool's 0 or 1)
    Types::KeyframeValue ToKey(const ReShade::Uniform& uniform, const ReShade::Value& value);
    ReShade::Value FromKey(const ReShade::Uniform& uniform, const Types::KeyframeValue& key,
                           const ReShade::Value& preset);

    // Where a value is against its range (MXAO's Ambient Occlusion Amount is 1.20 over a top of 1.00, which ReShade's
    // Ctrl+click allows): 1 above it, -1 below it, 0 within it or without one
    int OutOfRange(const ReShade::Uniform& uniform, float value);
    // a slider's range: the value's own, or for a Drag one that holds the value and its preset's
    std::pair<float, float> GetSliderRange(const ReShade::Uniform& uniform, float value, float preset);

    // The section's folds (C1): the effects on (in the preset or drawn) or keyed first, in ReShade's order, then the
    // others under All effects. A file's values are under the first of its techniques listed, the others of the file
    // have none.
    struct Fold
    {
        std::size_t technique = 0;  // in Effects
        std::vector<std::size_t> uniforms;
    };
    struct Folds
    {
        std::vector<Fold> listed;
        std::vector<Fold> others;
        std::size_t kept = 0;  // of listed, those neither on nor keyed
    };
    // kept holds the technique ids listed so far and takes those listed now: an effect turned off stays where it is
    // (its row doesn't jump away from the mouse) until kept is cleared, when a demo loads
    Folds GetFolds(const ReShade::Effects& effects, const std::vector<bool>& on, const std::set<std::string>& keyedIds,
                   std::set<std::string>& kept);

    // Keys of an effect that isn't loaded, kept: its file, and the ids of its properties with keys
    struct Missing
    {
        std::string file;
        std::vector<std::string> ids;
    };
    // ids are those with keys; the ones neither a uniform nor a technique of the effects loaded, by file
    std::vector<Missing> GetMissing(const ReShade::Effects& effects, const std::vector<std::string>& keyedIds);
    // the file of an id ("qUINT_dof.fx" of "qUINT_dof.fx/fFocusDepth")
    std::string GetFile(const std::string& id);

    // ReShade's clock as the demo makes it (E1): its timer the demo's time, its frame time the time since the frame
    // drawn before, its frame count one more each frame the demo moved on, so grain and noise stand still while it is
    // paused and step one frame at a time in a recording
    struct ClockState
    {
        std::optional<std::uint32_t> tick;  // the frame drawn before's
        std::uint32_t frameCount = 0;
    };
    ReShade::Clock AdvanceClock(ClockState& state, std::uint32_t tick);
}  // namespace IWXMVM::Components::ReShadeLogic
