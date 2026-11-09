#include "StdInclude.hpp"
#include "ReShadeKeys.hpp"

#include "Components/CaptureManager.hpp"
#include "Components/KeyframeLogic.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/Playback.hpp"
#include "Components/ReShadeLogic.hpp"
#include "Components/RuntimeProperties.hpp"
#include "Components/UndoHistory.hpp"
#include "Events.hpp"
#include "Mod.hpp"

namespace IWXMVM::Components::ReShadeKeys
{
    namespace
    {
        // the properties of the effects ReShade has now, with their index there
        struct Bound
        {
            Types::KeyframeablePropertyType type;
            bool technique = false;
            std::size_t index = 0;
        };
        std::uint32_t boundGeneration = 0;
        std::vector<Bound> bound;

        ReShadeLogic::ClockState clockState;
        std::mutex clockMutex;
        std::optional<ReShade::Clock> lastClock;

        // A change in ReShade's menu while the row has keys (D1): one drag of its slider keys the same key, one step
        // of the undo history, until the mouse button is let go
        struct MenuEdit
        {
            Types::KeyframeablePropertyType type;
            std::optional<std::int32_t> key;
            bool modifying = false;
            std::chrono::steady_clock::time_point last;
        };
        std::optional<MenuEdit> menuEdit;

        std::mutex traceMutex;
        std::optional<std::string> traced;
        std::vector<TracedFrame> trace;

        // a row's value without keys being changed (ChangePreset)
        struct PresetEdit
        {
            std::string id;
            bool technique = false;
            ReShade::Value before{};
            ReShade::Value after{};
            std::string label;
        };
        std::optional<PresetEdit> presetEdit;

        void Bind(const ReShade::Effects& effects)
        {
            if (effects.generation == boundGeneration)
                return;
            auto& keyframeManager = KeyframeManager::Get();
            bound.clear();
            for (std::size_t i = 0; i < effects.techniques.size(); ++i)
            {
                const auto type = RuntimeProperties::Register(ReShadeLogic::Describe(effects.techniques[i]));
                keyframeManager.AddProperty(type);
                bound.push_back({type, true, i});
            }
            for (std::size_t i = 0; i < effects.uniforms.size(); ++i)
            {
                const auto& uniform = effects.uniforms[i];
                const auto type = RuntimeProperties::Register(
                    ReShadeLogic::Describe(uniform, ReShadeLogic::GetEffectName(effects, uniform.file)));
                keyframeManager.AddProperty(type);
                bound.push_back({type, false, i});
            }
            boundGeneration = effects.generation;
        }

        // (at the frame's EndScene, before ReShade draws its effects) the values keys give the frame at its tick
        void OnFrameDrawn(const ReShade::Effects& effects)
        {
            Bind(effects);
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            {
                clockState = {};
                std::lock_guard lock(clockMutex);
                lastClock.reset();
                return;
            }

            const auto tick = CaptureManager::Get().GetDrawnTick();
            const auto clock = ReShadeLogic::AdvanceClock(clockState, tick);
            ReShade::SetClock(clock);
            {
                std::lock_guard lock(clockMutex);
                lastClock = clock;
            }

            auto& keyframeManager = KeyframeManager::Get();
            std::optional<ReShade::Values> values;
            std::lock_guard traceLock(traceMutex);
            for (const auto& each : bound)
            {
                const auto& property = keyframeManager.GetProperty(each.type);
                const auto& keys = keyframeManager.GetKeyframes(property);
                const auto& id =
                    each.technique ? effects.techniques[each.index].id : effects.uniforms[each.index].id;
                const bool tracing = traced && *traced == id;
                if (keys.empty())
                {
                    if (tracing)
                        trace.push_back({tick, {}, false, CaptureManager::Get().IsCapturing()});
                    continue;
                }
                const auto key = keyframeManager.Interpolate(property, keys, tick);
                if (each.technique)
                {
                    const bool on = KeyframeLogic::GetStepValue(key.floatingPoint) != 0.0f;
                    ReShade::HoldTechnique(each.index, on);
                    if (tracing)
                        trace.push_back({tick, {on ? 1.0f : 0.0f}, true, CaptureManager::Get().IsCapturing()});
                    continue;
                }
                // (what a key doesn't hold, a colour's alpha, the preset's)
                if (!values)
                    values = ReShade::GetValues();
                const auto preset = values->generation == effects.generation && each.index < values->preset.size()
                                        ? values->preset[each.index]
                                        : ReShade::Value{};
                const auto held = ReShadeLogic::FromKey(effects.uniforms[each.index], key, preset);
                ReShade::Hold(each.index, held);
                if (tracing)
                    trace.push_back({tick, held, true, CaptureManager::Get().IsCapturing()});
            }
        }

        void EndMenuEdit()
        {
            if (!menuEdit)
                return;
            auto& keyframeManager = KeyframeManager::Get();
            if (menuEdit->modifying && keyframeManager.HasProperty(menuEdit->type))
            {
                const auto& property = keyframeManager.GetProperty(menuEdit->type);
                auto& keys = keyframeManager.GetKeyframes(property);
                if (const auto key = std::ranges::find(keys, menuEdit->key.value_or(-1), &Types::Keyframe::id);
                    key != keys.end() && keyframeManager.IsKeyframeValueBeingModified(*key))
                    keyframeManager.EndModifyingKeyframeValue(property, *key);
            }
            UndoHistory::Get().EndGroup();
            menuEdit.reset();
        }

        // A change in ReShade's menu of a value with keys keys it at the playhead (D1); one without keys changed
        // the preset, as before
        void TakeMenuChanges(const ReShade::Effects& effects)
        {
            auto& keyframeManager = KeyframeManager::Get();
            const auto now = std::chrono::steady_clock::now();
            for (const auto& change : ReShade::TakeMenuChanges())
            {
                const auto type = RuntimeProperties::FindById(change.id);
                if (!type || !keyframeManager.HasProperty(*type))
                    continue;
                const auto& property = keyframeManager.GetProperty(*type);
                auto& keys = keyframeManager.GetKeyframes(property);
                if (keys.empty())
                    continue;

                Types::KeyframeValue value(change.value[0] >= 0.5f ? 1.0f : 0.0f);
                if (!change.technique)
                {
                    const auto index = effects.FindUniform(change.id);
                    if (!index)
                        continue;
                    value = ReShadeLogic::ToKey(effects.uniforms[*index], change.value);
                }

                if (!menuEdit || menuEdit->type != *type)
                {
                    EndMenuEdit();
                    std::string name(property.name);
                    UndoHistory::Get().BeginGroup(
                        std::format("change {} in ReShade's menu", name.starts_with("ReShade: ") ? name.substr(9) : name));
                    menuEdit = MenuEdit{*type};
                }
                menuEdit->last = now;

                const auto tick = Playback::GetTimelineTick();
                auto key = std::ranges::find(keys, menuEdit->key.value_or(-1), &Types::Keyframe::id);
                if (key == keys.end())
                    key = std::ranges::find(keys, tick, &Types::Keyframe::tick);
                if (key == keys.end())
                {
                    const Types::Keyframe added(property, tick, value);
                    keyframeManager.AddKeyframe(property, added);
                    keyframeManager.SortAndSaveKeyframes(keys);
                    menuEdit->key = added.id;
                    continue;
                }
                menuEdit->key = key->id;
                if (!menuEdit->modifying)
                {
                    keyframeManager.BeginModifyingKeyframeValue(*key);
                    menuEdit->modifying = true;
                }
                key->value = KeyframeLogic::Normalize(Types::Keyframe(property, key->tick, value), *type).value;
                keyframeManager.SortAndSaveKeyframes(keys);
            }

            // (ReShade's menu takes the mouse from the game's window while it is open)
            if (menuEdit && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000) &&
                now - menuEdit->last > std::chrono::milliseconds(250))
                EndMenuEdit();
        }

        void SetPreset(const std::string& id, bool technique, const ReShade::Value& value)
        {
            if (technique)
                ReShade::SetPresetTechnique(id, value[0] >= 0.5f);
            else
                ReShade::SetPresetValue(id, value);
        }

        void EndPresetEdit()
        {
            if (!presetEdit)
                return;
            const auto edit = *std::exchange(presetEdit, std::nullopt);
            ReShade::SavePreset();
            if (edit.before == edit.after)
                return;
            UndoHistory::Get().Push(
                edit.label,
                [edit] {
                    SetPreset(edit.id, edit.technique, edit.before);
                    ReShade::SavePreset();
                },
                [edit] {
                    SetPreset(edit.id, edit.technique, edit.after);
                    ReShade::SavePreset();
                });
        }
    }  // namespace

    void Initialize()
    {
        ReShade::SetFrameListener(OnFrameDrawn);

        Events::RegisterListener(EventType::OnFrame, [] {
            const auto effects = ReShade::GetEffects();
            if (!effects)
                return;
            Bind(*effects);
            TakeMenuChanges(*effects);
        });
        // (what was done in the demo before can't be undone in this one)
        Events::RegisterListener(EventType::PostDemoLoad, [] {
            menuEdit.reset();
            presetEdit.reset();
        });
    }

    std::optional<Types::KeyframeablePropertyType> FindType(const std::string& id)
    {
        const auto effects = ReShade::GetEffects();
        if (!effects || (!effects->FindUniform(id) && !effects->FindTechnique(id)))
            return std::nullopt;
        Bind(*effects);
        return RuntimeProperties::FindById(id);
    }

    void ChangePreset(const std::string& id, bool technique, const ReShade::Value& before, const ReShade::Value& after,
                      const std::string& label)
    {
        SetPreset(id, technique, after);
        if (presetEdit && presetEdit->id != id)
            EndPresetEdit();
        if (!presetEdit)
            presetEdit = PresetEdit{id, technique, before, after, label};
        presetEdit->after = after;
    }

    void EndPresetEditWhenIdle(bool controlActive)
    {
        if (presetEdit && !controlActive)
            EndPresetEdit();
    }

    std::optional<Types::KeyframeValue> GetPresetKey(Types::KeyframeablePropertyType type)
    {
        const auto description = RuntimeProperties::Find(type);
        const auto effects = ReShade::GetEffects();
        if (!description || !effects)
            return std::nullopt;
        const auto values = ReShade::GetValues();
        if (values.generation != effects->generation)
            return std::nullopt;
        if (const auto index = effects->FindUniform(description->id); index && *index < values.preset.size())
            return ReShadeLogic::ToKey(effects->uniforms[*index], values.preset[*index]);
        if (const auto index = effects->FindTechnique(description->id); index && *index < values.presetOn.size())
            return Types::KeyframeValue(values.presetOn[*index] ? 1.0f : 0.0f);
        return std::nullopt;
    }

    std::optional<ReShade::Clock> GetLastClock()
    {
        std::lock_guard lock(clockMutex);
        return lastClock;
    }

    void StartTrace(const std::string& id)
    {
        std::lock_guard lock(traceMutex);
        traced = id;
        trace.clear();
    }

    std::vector<TracedFrame> StopTrace()
    {
        std::lock_guard lock(traceMutex);
        traced.reset();
        return std::exchange(trace, {});
    }
}  // namespace IWXMVM::Components::ReShadeKeys
