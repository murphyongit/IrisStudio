#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// The mod as a ReShade add-on: ReShade's effects, their values and on/off, as the Look tab
// shows and keys them. ReShade 6.8's standard build lets a DLL that is already in the process register (its exports
// ReShadeRegisterAddon) and use the reshade_* events, no add-on build needed. Its headers come with ImGui's function
// table when imgui.h is included before them, so ReShade.cpp is built without the precompiled header, and nothing of
// ReShade's is in this one. Everything that talks to ReShade's runtime happens on the render thread in
// BeforePresent, at the frame's EndScene before ReShade's Present draws the effects (and ReShade's clock in
// reshade_begin_effects, as ReShade sets it just before); what other threads ask for waits for it.
namespace IWXMVM::ReShade
{
    enum class Status
    {
        NotFound,         // no ReShade in the game (D3D9::IsReshadePresent), or it refused the add-on
        Waiting,          // registered, the runtime comes with the first frame it draws
        PerformanceMode,  // values are compiled into the shaders, so they can't be changed
        Ready,
    };

    // What a value of an effect is, as ReShade's menu shows it (its ui_type and type)
    enum class Kind
    {
        Slider,  // a float or int with ui_min and ui_max (ui_type "slider" or "drag")
        Drag,    // a float or int without a range
        Bool,    // a checkbox
        List,    // an int of ui_items (ui_type "combo", "list" or "radio")
        Color,   // a float3 or float4 with ui_type "color" (its first three are keyed)
        Vector,  // a float2 or float3
    };

    struct Uniform
    {
        std::string id;        // "<effect file>/<variable>": "qUINT_ssr.fx/SSR_REFLECTION_INTENSITY"
        std::string file;      // "qUINT_ssr.fx"
        std::string variable;  // "SSR_REFLECTION_INTENSITY"
        std::string label;     // ui_label, else the variable
        std::string category;  // ui_category, "" when none
        std::string tooltip;   // ui_tooltip
        std::string units;     // ui_units
        Kind kind = Kind::Drag;
        bool integer = false;    // an int or uint (values rounded)
        std::uint32_t components = 1;  // 1 to 4
        float min = 0.0f;  // ui_min and ui_max (both 0 for a Drag, which has none)
        float max = 1.0f;
        float step = 0.0f;  // ui_step, 0 when none
        std::vector<std::string> items;  // a List's
    };

    struct Technique
    {
        std::string id;     // "<effect file>/technique:<name>": "qUINT_ssr.fx/technique:SSR"
        std::string file;   // "qUINT_ssr.fx"
        std::string name;   // "SSR"
        std::string label;  // ui_label, else the name
        std::string tooltip;
    };

    // The effects as ReShade loaded them, in its menu's order; made again after each reload
    struct Effects
    {
        std::vector<Technique> techniques;
        std::vector<Uniform> uniforms;
        std::string presetPath;
        std::uint32_t generation = 0;  // one more at each reload

        std::optional<std::size_t> FindUniform(const std::string& id) const
        {
            const auto found = uniformIndex.find(id);
            return found != uniformIndex.end() ? std::optional(found->second) : std::nullopt;
        }
        std::optional<std::size_t> FindTechnique(const std::string& id) const
        {
            const auto found = techniqueIndex.find(id);
            return found != techniqueIndex.end() ? std::optional(found->second) : std::nullopt;
        }

        std::unordered_map<std::string, std::size_t> uniformIndex;
        std::unordered_map<std::string, std::size_t> techniqueIndex;
    };

    using Value = std::array<float, 4>;

    // The values as they are now: the preset's own (what ReShade has without keys) and those drawn last (keyed ones
    // while keys hold them), by the index in Effects
    struct Values
    {
        std::uint32_t generation = 0;  // of the Effects they are of
        std::vector<Value> preset;
        std::vector<Value> drawn;
        std::vector<bool> presetOn;  // the techniques'
        std::vector<bool> drawnOn;
    };

    // Registers with ReShade when the game has it (from D3D9::Initialize)
    void Initialize();
    // (render thread, at the frame's EndScene, before ReShade's Present draws the effects) the frame's values: those
    // asked for, those keys hold, and a save of the preset
    void BeforePresent();
    // (render thread, as the mod unloads) the preset's own values back and the add-on unregistered
    void Shutdown();

    // ReShade loads its effects again after each reset of the device (a capture's render size, Shadow quality), with its
    // toggle key's effects on or off: about 5 s in which it draws the frames without them and its loading ring in the
    // corner, and minutes at a size it hasn't compiled them at. It draws them only
    // once loaded (reshade_begin_effects). (render thread, from D3D9's Reset and CreateDevice) a reset; whether it's in
    // the game, its toggle, whether it's loading them, and the frames it drew them in since the reset. A new device (a
    // mod loaded with loadmod) comes with a new runtime.
    // IsDrawingEffects: whether it draws them (it has, and they aren't off by its toggle key); TakeDrewEffects: since
    // asked last, for each captured frame
    struct Reload
    {
        bool present = false;
        bool effectsOn = true;
        bool loading = false;
        std::uint32_t framesSince = 0;
    };
    void OnDeviceReset();
    void OnDeviceCreated();
    Reload GetReload();
    bool IsDrawingEffects();
    bool TakeDrewEffects();
    // (the harness's) as ReShade's effects toggle key does, at the next frame; taken as loading for a while, as a first
    // compile can't be in a scenario (its cache keeps it)
    void SetEffectsOn(bool on);
    void SetLoadingFor(std::uint32_t ms);

    Status GetStatus();
    std::shared_ptr<const Effects> GetEffects();  // nothing until the runtime came
    Values GetValues();

    // A drawn frame's values (render thread, in BeforePresent): called before the effects draw, it holds the values
    // keys give this frame with Hold and HoldTechnique; what it held the frame before and doesn't now gets the preset's
    // own value back
    using FrameListener = std::function<void(const Effects&)>;
    void SetFrameListener(FrameListener listener);
    void Hold(std::size_t uniform, const Value& value);
    void HoldTechnique(std::size_t technique, bool on);

    // ReShade's clock for this frame (E1, in the frame listener): its "timer", "frametime" and "framecount" sources,
    // which it otherwise runs on the PC's clock
    struct Clock
    {
        float timer = 0.0f;  // ms
        float frameTime = 0.0f;  // ms
        std::uint32_t frameCount = 0;
    };
    void SetClock(const Clock& clock);

    // The preset's own value of one without keys, as ReShade's menu changes it (saved if ReShade saves the preset
    // after its menu's changes), set before the next frame draws
    void SetPresetValue(const std::string& id, const Value& value);
    void SetPresetTechnique(const std::string& id, bool on);
    // Saves the preset as ReShade's menu does after a change, when ReShade's Auto Save is on (AutoSavePreset), with the
    // preset's own values where keys hold values (Q1); at the next frame
    void SavePreset();

    // A change made in ReShade's own menu (D1), since asked last: the value it set
    struct MenuChange
    {
        std::string id;  // a uniform's or a technique's
        Value value{};   // a technique's on: 1, off: 0
        bool technique = false;
    };
    std::vector<MenuChange> TakeMenuChanges();

    // (the harness's) Sets a value or a technique's on/off at the next frame the way ReShade's menu does, not as the
    // mod's own: through ReShade, then with save the preset saved, as ReShade's Auto Save does after a change
    void SetAsMenu(const std::string& id, const Value& value, bool save);

    // How often the mod gave ReShade's preset its own values back for a save of ReShade's while keys held values
    // (Q1), for the harness
    std::uint32_t GetPresetRestores();
}  // namespace IWXMVM::ReShade
