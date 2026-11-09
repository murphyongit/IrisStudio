#pragma once
#include <optional>
#include <string>

#include "ReShade.hpp"
#include "Types/Keyframe.hpp"

// ReShade's values and effects as keyframeable properties: their properties as ReShade
// loads them, the values keys give each drawn frame at its own tick, ReShade's clock following the demo (E1), changes
// made in ReShade's own menu as the rows' (D1), and the preset's own values changed from the Look tab as steps of the
// undo history. On the render thread, as the UI and ReShade's events are.
namespace IWXMVM::Components::ReShadeKeys
{
    void Initialize();

    // The property of a uniform or a technique of the effects ReShade has now, by its id (made as the effects are
    // read): nothing for one ReShade doesn't have
    std::optional<Types::KeyframeablePropertyType> FindType(const std::string& id);

    // A row's value without keys changed (all of the edit, from before): ReShade's preset gets it at once, and as
    // ReShade's menu does, the preset is saved when the edit ends (with ReShade's Auto Save on), one step of the undo
    // history named label. A technique's on/off the same, with on in value[0].
    void ChangePreset(const std::string& id, bool technique, const ReShade::Value& before, const ReShade::Value& after,
                      const std::string& label);
    // ends the edit of ChangePreset once no control is active (each frame)
    void EndPresetEditWhenIdle(bool controlActive);

    // The value a key added to a property without keys gets: the preset's own, as a key holds it
    std::optional<Types::KeyframeValue> GetPresetKey(Types::KeyframeablePropertyType type);

    // what the harness and the Look tab show of the clock: the one the frame drawn last had
    std::optional<ReShade::Clock> GetLastClock();

    // (the harness's) each frame drawn from now on, with its tick and the value keys gave a uniform or technique (a
    // technique's on in value[0]), until StopTrace
    struct TracedFrame
    {
        std::uint32_t tick = 0;
        ReShade::Value value{};
        bool held = false;
        bool capturing = false;
    };
    void StartTrace(const std::string& id);
    std::vector<TracedFrame> StopTrace();
}  // namespace IWXMVM::Components::ReShadeKeys
